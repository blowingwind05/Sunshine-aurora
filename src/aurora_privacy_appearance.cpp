/**
 * @file src/aurora_privacy_appearance.cpp
 * @brief Validate normalized PNGs and replace the managed image without following links.
 */
#include "aurora_privacy_appearance.h"

#include <array>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <zlib.h>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <unistd.h>

  #include <cerrno>
#endif

namespace aurora::privacy_appearance {
  namespace {
    namespace fs = std::filesystem;
    using namespace std::literals;

    constexpr auto image_name = "privacy-screensaver.png";  ///< The only image property exposed over HTTPS.
    constexpr auto png_signature = "\x89PNG\r\n\x1a\n"sv;  ///< Fixed PNG signature.

    /** @brief Read a big-endian PNG word from four bytes already checked by the caller. */
    uint32_t word(std::string_view bytes) {
      uint32_t value = 0;
      for (unsigned char byte : bytes.substr(0, 4)) {
        value = (value << 8) | byte;
      }
      return value;
    }

    /** @brief Stream PNG IDAT through zlib, validating row lengths and filter bytes without retaining pixels. */
    class scanlines {
    public:
      explicit scanlines(std::size_t row_size, std::size_t height):
          row_size {row_size}, expected {row_size * height}, ready {inflateInit(&stream) == Z_OK} {}

      ~scanlines() {
        if (ready) inflateEnd(&stream);
      }

      /** @brief Consume an IDAT chunk and reject extra decoded data or an invalid zlib stream. */
      bool append(std::string_view chunk) {
        if (!ready) return false;
        if (chunk.empty()) return true;
        if (ended) return false;
        stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(chunk.data()));
        stream.avail_in = static_cast<uInt>(chunk.size());
        do {
          std::array<unsigned char, 16 * 1024> output;
          stream.next_out = output.data();
          stream.avail_out = static_cast<uInt>(output.size());
          const auto code = inflate(&stream, Z_NO_FLUSH);
          const auto size = output.size() - stream.avail_out;
          if (size > expected - decoded) return false;
          for (auto row = (row_size - decoded % row_size) % row_size; row < size; row += row_size) {
            if (output[row] > 4) return false;
          }
          decoded += size;
          if (code == Z_STREAM_END) {
            ended = true;
            return stream.avail_in == 0 && decoded == expected;
          }
          if (code == Z_BUF_ERROR) return stream.avail_in == 0 && size == 0;
          if (code != Z_OK) return false;
        } while (stream.avail_in || !stream.avail_out);
        return true;
      }

      /** @brief Whether the complete image's zlib stream was consumed. */
      bool complete() const { return ended; }

    private:
      z_stream stream {};  ///< One zlib stream for all consecutive IDAT chunks.
      std::size_t row_size;  ///< Filter byte and RGB samples per row.
      std::size_t expected;  ///< Exact decoded byte count.
      bool ready;  ///< Whether inflateInit succeeded.
      std::size_t decoded = 0;  ///< Bytes already checked.
      bool ended = false;  ///< Set only when zlib reaches the end of its stream.
    };

    /** @brief Accept only bounded, non-compressed color or physical-size metadata. */
    unsigned metadata_kind(std::string_view type, std::string_view payload) {
      if (type == "sRGB"sv && payload.size() == 1 && static_cast<unsigned char>(payload[0]) <= 3) return 1;
      if (type == "gAMA"sv && payload.size() == 4 && word(payload) != 0) return 2;
      if (type == "cHRM"sv && payload.size() == 32) return 4;
      if (type == "pHYs"sv && payload.size() == 9 && static_cast<unsigned char>(payload[8]) <= 1) return 8;
      return 0;
    }

    /** @brief Encode fixed-size digests and temporary-name entropy as lowercase hex. */
    std::string hex(const unsigned char *bytes, std::size_t size) {
      constexpr char digits[] = "0123456789abcdef";
      std::string output;
      output.reserve(2 * size);
      for (std::size_t i = 0; i < size; ++i) {
        output.push_back(digits[bytes[i] >> 4]);
        output.push_back(digits[bytes[i] & 15]);
      }
      return output;
    }

    /** @brief Digest the exact bytes stored on disk. */
    std::string sha256(std::string_view data) {
      std::array<unsigned char, 32> digest {};
      unsigned length = 0;
      return EVP_Digest(data.data(), data.size(), digest.data(), &length, EVP_sha256(), nullptr) == 1 &&
          length == digest.size() ? hex(digest.data(), digest.size()) : std::string {};
    }

    /** @brief Generate a sibling name for an exclusively created temporary file. */
    std::string temporary_name() {
      std::array<unsigned char, 16> random {};
      return RAND_bytes(random.data(), static_cast<int>(random.size())) == 1
        ? ".aurora-privacy-" + hex(random.data(), random.size()) + ".tmp" : std::string {};
    }

    /** @brief Require an absolute profile without aliases such as traversal or NTFS alternate data streams. */
    bool safe_profile(const fs::path &profile) {
      if (!profile.is_absolute() || profile.relative_path().empty()) return false;
#ifdef _WIN32
      const auto root = profile.root_name().native();
      if (root.size() != 2 || root[1] != L':') return false;
#endif
      for (const auto &part : profile.relative_path()) {
        if (part == "." || part == "..") return false;
#ifdef _WIN32
        const auto &name = part.native();
        if (!name.empty() && (name.back() == L'.' || name.back() == L' ' || name.find(L':') != std::wstring::npos)) return false;
#endif
      }
      return true;
    }

#ifdef _WIN32
    /** @brief Close a validated Win32 handle at scope exit. */
    struct close_handle {
      void operator()(void *handle) const { CloseHandle(handle); }
    };
    using handle_t = std::unique_ptr<void, close_handle>;  ///< Null represents a failed handle acquisition.

    /** @brief Turn an INVALID_HANDLE_VALUE return into an empty scoped handle. */
    handle_t owned(HANDLE handle) {
      return handle_t {handle != INVALID_HANDLE_VALUE ? handle : nullptr};
    }

    /** @brief Remove a file by handle without resolving its path a second time. */
    bool discard(HANDLE handle) {
      FILE_DISPOSITION_INFO disposition {TRUE};
      return SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition)) != 0;
    }

    /** @brief Pin every directory in the profile and open only non-reparse regular image files. */
    class profile_directory {
    public:
      explicit profile_directory(const fs::path &profile) {
        if (!safe_profile(profile) || !pin(profile.root_path())) return;
        auto directory = profile.root_path();
        for (const auto &part : profile.relative_path()) {
          if (part.empty()) continue;
          directory /= part;
          if (!pin(directory)) return;
        }
        path = std::move(directory);
      }

      explicit operator bool() const { return !path.empty(); }

      /** @brief Read only bounded bytes from the opened non-reparse image. */
      result read() const {
        auto image = open_image(GENERIC_READ);
        if (image.missing) return {status::default_image, {}, {}};
        if (!image.file || !image.size || image.size > max_png_bytes) return {};
        std::string png(static_cast<std::size_t>(image.size), '\0');
        DWORD count = 0;
        if (!ReadFile(image.file.get(), png.data(), static_cast<DWORD>(png.size()), &count, nullptr) ||
            count != png.size()) return {};
        return {status::custom, std::move(png), {}};
      }

      /** @brief Flush an exclusive sibling and atomically rename the opened file over the fixed destination. */
      bool replace(std::string_view bytes, const std::string &temporary) const {
        const auto target = path / image_name;
        {
          const auto existing = open_image(FILE_READ_ATTRIBUTES);
          if (!existing.file && !existing.missing) return false;
        }
        const auto name = (path / temporary).native();
        auto file = owned(CreateFileW(name.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!file) return false;

        const auto &destination = target.native();
        const auto name_bytes = destination.size() * sizeof(wchar_t);
        std::vector<unsigned char> storage(sizeof(FILE_RENAME_INFO) + name_bytes);
        auto *rename = reinterpret_cast<FILE_RENAME_INFO *>(storage.data());
        rename->ReplaceIfExists = TRUE;
        rename->FileNameLength = static_cast<DWORD>(name_bytes);
        std::memcpy(rename->FileName, destination.data(), name_bytes);
        DWORD count = 0;
        if (!WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) ||
            count != bytes.size() || !FlushFileBuffers(file.get()) ||
            !SetFileInformationByHandle(file.get(), FileRenameInfo, rename, static_cast<DWORD>(storage.size()))) {
          discard(file.get());
          return false;
        }
        return true;
      }

      /** @brief Remove only a validated regular image, or accept an already default profile. */
      bool remove() const {
        auto image = open_image(DELETE);
        return image.missing || (image.file && discard(image.file.get()));
      }

    private:
      /** @brief Identifies an opened regular file or a clean absence. */
      struct opened_image {
        handle_t file;  ///< Open handle with reparse points and hard links excluded.
        uint64_t size = 0;  ///< Size from the open file handle.
        bool missing = false;  ///< The fixed filename is absent.
      };

      /** @brief Check a directory itself and retain its handle without delete sharing. */
      bool pin(const fs::path &directory) {
        auto handle = owned(CreateFileW(directory.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
          nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION info {};
        if (!handle || !GetFileInformationByHandle(handle.get(), &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != FILE_ATTRIBUTE_DIRECTORY) return false;
        directories.push_back(std::move(handle));
        return true;
      }

      /** @brief Open the fixed filename as a single-link regular disk file. */
      opened_image open_image(DWORD access) const {
        auto file = owned(CreateFileW((path / image_name).c_str(), access | FILE_READ_ATTRIBUTES,
          FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!file) return {nullptr, 0, GetLastError() == ERROR_FILE_NOT_FOUND};
        BY_HANDLE_FILE_INFORMATION info {};
        if (GetFileType(file.get()) != FILE_TYPE_DISK || !GetFileInformationByHandle(file.get(), &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) || info.nNumberOfLinks != 1) return {};
        return {std::move(file), (uint64_t {info.nFileSizeHigh} << 32) | info.nFileSizeLow, false};
      }

      fs::path path;  ///< Original absolute profile, valid only after all directory checks.
      std::vector<handle_t> directories;  ///< Ancestors held through the entire file operation.
    };
#else
    /** @brief Scoped descriptor used for profile-relative operations. */
    struct fd_t {
      explicit fd_t(int value): value {value} {}
      ~fd_t() { if (value >= 0) close(value); }
      fd_t(const fd_t &) = delete;
      fd_t &operator=(const fd_t &) = delete;
      int value;  ///< -1 if opening failed.
    };

    /** @brief Walk every component without following links and keep the profile directory descriptor. */
    class profile_directory {
    public:
      explicit profile_directory(const fs::path &profile) {
        if (!safe_profile(profile)) return;
        fd_t current {open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
        for (const auto &part : profile.relative_path()) {
          if (part.empty()) continue;
          if (current.value < 0) return;
          fd_t child {openat(current.value, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
          std::swap(current.value, child.value);
        }
        std::swap(directory.value, current.value);
      }

      explicit operator bool() const { return directory.value >= 0; }

      /** @brief Read only a regular file reached through the pinned profile directory. */
      result read() const {
        fd_t file {openat(directory.value, image_name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC)};
        if (file.value < 0) return errno == ENOENT ? result {status::default_image, {}, {}} : result {};
        struct stat info {};
        if (fstat(file.value, &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1 ||
            info.st_size <= 0 || static_cast<uint64_t>(info.st_size) > max_png_bytes) return {};
        std::string png(static_cast<std::size_t>(info.st_size), '\0');
        for (std::size_t offset = 0; offset < png.size();) {
          const auto size = ::read(file.value, png.data() + offset, png.size() - offset);
          if (size < 0 && errno == EINTR) continue;
          if (size <= 0) return {};
          offset += static_cast<std::size_t>(size);
        }
        return {status::custom, std::move(png), {}};
      }

      /** @brief Write and sync an exclusive sibling before publishing it with renameat. */
      bool replace(std::string_view bytes, const std::string &temporary) const {
        if (!image_state().has_value()) return false;
        fd_t file {openat(directory.value, temporary.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0600)};
        if (file.value < 0) return false;
        while (!bytes.empty()) {
          const auto size = write(file.value, bytes.data(), bytes.size());
          if (size < 0 && errno == EINTR) continue;
          if (size <= 0) break;
          bytes.remove_prefix(static_cast<std::size_t>(size));
        }
        if (!bytes.empty() || fsync(file.value) != 0 ||
            !image_state().has_value() || renameat(directory.value, temporary.c_str(), directory.value, image_name) != 0) {
          unlinkat(directory.value, temporary.c_str(), 0);
          return false;
        }
        return true;
      }

      /** @brief Unlink a regular file without following its path, and accept a clean absence. */
      bool remove() const {
        const auto exists = image_state();
        return exists && (!*exists || unlinkat(directory.value, image_name, 0) == 0);
      }

    private:
      /** @brief Distinguish a valid existing regular image, an absent image, and an unsafe path. */
      std::optional<bool> image_state() const {
        struct stat info {};
        if (fstatat(directory.value, image_name, &info, AT_SYMLINK_NOFOLLOW) != 0) {
          return errno == ENOENT ? std::optional {false} : std::nullopt;
        }
        return S_ISREG(info.st_mode) && info.st_nlink == 1 ? std::optional {true} : std::nullopt;
      }

      fd_t directory {-1};  ///< Pinned profile used as the base for every file operation.
    };
#endif
  }

  bool valid_png(std::string_view png) {
    constexpr auto header_size = 8 + 12 + 13;
    if (png.size() < header_size || png.size() > max_png_bytes || !png.starts_with(png_signature)) return false;
    png.remove_prefix(png_signature.size());
    if (word(png) != 13 || png.substr(4, 4) != "IHDR"sv ||
        png.substr(16, 5) != "\x08\x02\x00\x00\x00"sv) return false;
    const auto width = word(png.substr(8));
    const auto height = word(png.substr(12));
    if (!width || !height || width > max_png_edge || height > max_png_edge || width * height > max_png_pixels) return false;
    scanlines scan {1 + 3 * width, height};
    unsigned metadata = 0;
    bool seen_data = false;
    bool first = true;
    while (png.size() >= 12) {
      const auto size = word(png);
      if (size > png.size() - 12) return false;
      const auto type = png.substr(4, 4);
      const auto payload = png.substr(8, size);
      const auto crc = crc32(0, reinterpret_cast<const Bytef *>(png.data() + 4), size + 4);
      if (crc != word(png.substr(8 + size))) return false;
      png.remove_prefix(12 + size);
      if (first) {
        first = false;
      } else if (type == "IDAT"sv) {
        seen_data = true;
        if (!scan.append(payload)) return false;
      } else if (type == "IEND"sv) {
        return size == 0 && png.empty() && scan.complete();
      } else {
        const auto kind = metadata_kind(type, payload);
        if (seen_data || !kind || (metadata & kind)) return false;
        metadata |= kind;
      }
    }
    return false;
  }

  result read(const fs::path &profile) {
    const profile_directory directory {profile};
    if (!directory) return {};
    auto image = directory.read();
    if (image.state == status::custom) {
      if (!valid_png(image.png)) return {};
      image.digest = sha256(image.png);
      if (image.digest.empty()) return {};
    }
    return image;
  }

  result save(const fs::path &profile, std::string_view png) {
    if (!valid_png(png)) return {status::invalid_image, {}, {}};
    const profile_directory directory {profile};
    if (!directory) return {};
    const auto temporary = temporary_name();
    auto digest = sha256(png);
    if (temporary.empty() || digest.empty() || !directory.replace(png, temporary)) return {};
    return {status::custom, {}, std::move(digest)};
  }

  result reset(const fs::path &profile) {
    const profile_directory directory {profile};
    if (!directory || !directory.remove()) return {};
    return {status::default_image, {}, {}};
  }
}
