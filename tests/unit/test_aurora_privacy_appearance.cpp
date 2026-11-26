/**
 * @file tests/unit/test_aurora_privacy_appearance.cpp
 * @brief Offline coverage for the normalized PNG and profile storage.
 */
#if defined(AURORA_SUNSHINE_HOST) && (defined(_WIN32) || defined(__APPLE__))

#include "src/aurora_privacy_appearance.h"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>

#include <gtest/gtest.h>
#include <zlib.h>

#ifdef _WIN32
  #include <windows.h>
#endif

namespace {
  namespace appearance = aurora::privacy_appearance;
  namespace fs = std::filesystem;
  using appearance::status;
  using namespace std::literals;

  /** @brief Append a PNG integer in network byte order. */
  void append_word(std::string &png, uint32_t value) {
    for (const auto shift : {24, 16, 8, 0}) png.push_back(static_cast<char>(value >> shift));
  }

  /** @brief Add a chunk with a valid CRC, including for malformed semantic fixtures. */
  void append_chunk(std::string &png, std::string_view tag, std::string_view data) {
    append_word(png, static_cast<uint32_t>(data.size()));
    const auto start = png.size();
    png.append(tag);
    png.append(data);
    append_word(png, crc32(0, reinterpret_cast<const Bytef *>(png.data() + start), static_cast<uInt>(4 + data.size())));
  }

  /** @brief Build a PNG with optional metadata, splitting the zlib stream across multiple chunks. */
  std::string fixture(std::string_view pixels = "\0\x10\x20\x30"sv, uint32_t width = 1, uint32_t height = 1,
    unsigned color = 2, std::string_view metadata = {}, bool extra_zlib_bytes = false) {
    std::string png {"\x89PNG\r\n\x1a\n"};
    std::string ihdr;
    append_word(ihdr, width);
    append_word(ihdr, height);
    ihdr.push_back(8);
    ihdr.push_back(static_cast<char>(color));
    ihdr.append(3, '\0');
    append_chunk(png, "IHDR", ihdr);
    if (!metadata.empty()) append_chunk(png, metadata, "\0\0\0\0\0\0\0\0\1"sv);
    std::string data(compressBound(static_cast<uLong>(pixels.size())), '\0');
    auto length = static_cast<uLongf>(data.size());
    if (compress(reinterpret_cast<Bytef *>(data.data()), &length, reinterpret_cast<const Bytef *>(pixels.data()),
          static_cast<uLong>(pixels.size())) != Z_OK) return {};
    data.resize(length);
    if (extra_zlib_bytes) data.push_back('\0');
    append_chunk(png, "IDAT", std::string_view {data}.substr(0, 3));
    append_chunk(png, "IDAT", std::string_view {data}.substr(3));
    append_chunk(png, "IEND", {});
    return png;
  }

  TEST(AuroraPrivacyAppearance, ValidatesCompleteBoundedOpaquePng) {
    auto png = fixture();
    EXPECT_TRUE(appearance::valid_png(png));
    EXPECT_TRUE(appearance::valid_png(fixture(std::string((1 + 3 * 192) * 100, '\0'), 192, 100, 2, "pHYs")));
    EXPECT_FALSE(appearance::valid_png(fixture("\0\x10\x20\x30\xff"sv, 1, 1, 6)));
    EXPECT_FALSE(appearance::valid_png(fixture("\0\x10\x20\x30"sv, 1, 1, 2, "tRNS")));
    EXPECT_FALSE(appearance::valid_png(fixture("\0\x10\x20\x30"sv, 1, 1, 2, "acTL")));
    EXPECT_FALSE(appearance::valid_png(fixture("\0\x10\x20\x30"sv, 4097)));
    EXPECT_FALSE(appearance::valid_png(fixture("\0\x10\x20\x30"sv, 4096, 4096)));
    EXPECT_FALSE(appearance::valid_png(fixture("\5\x10\x20\x30"sv)));
    EXPECT_FALSE(appearance::valid_png(fixture("\0\x10\x20"sv)));
    EXPECT_FALSE(appearance::valid_png(fixture("\0\x10\x20\x30\0"sv)));
    EXPECT_FALSE(appearance::valid_png(fixture("\0\x10\x20\x30"sv, 1, 1, 2, {}, true)));
    EXPECT_FALSE(appearance::valid_png(png + "trailing"));
    EXPECT_FALSE(appearance::valid_png(png.substr(0, png.size() - 12)));
    png.back() ^= 1;
    EXPECT_FALSE(appearance::valid_png(png));
    EXPECT_FALSE(appearance::valid_png(std::string(appearance::max_png_bytes + 1, '\0')));
  }

  /** @brief Give each storage test a fresh absolute profile inside the test machine's temporary directory. */
  class AppearanceProfile: public testing::Test {
  protected:
    void SetUp() override {
      const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
      root = fs::canonical(fs::temp_directory_path()) / ("aurora-appearance-" + std::to_string(nonce));
      profile = root / "profile";
      ASSERT_TRUE(fs::create_directories(profile));
      image = profile / "privacy-screensaver.png";
    }

    void TearDown() override {
      fs::remove_all(root);
    }

    fs::path root;  ///< Isolated test root.
    fs::path profile;  ///< Managed profile fixture.
    fs::path image;  ///< Fixed file inside the profile.
  };

  TEST_F(AppearanceProfile, SavesReplacesAndResetsWithStableDigest) {
    EXPECT_EQ(appearance::read(profile).state, status::default_image);
    const auto first = fixture();
    const auto saved = appearance::save(profile, first);
    ASSERT_EQ(saved.state, status::custom);
    EXPECT_EQ(saved.digest.size(), 64);
    EXPECT_EQ(appearance::read(profile).png, first);
    EXPECT_EQ(appearance::read(profile).digest, saved.digest);
    EXPECT_EQ(appearance::save(profile, "garbage").state, status::invalid_image);
    EXPECT_EQ(appearance::read(profile).digest, saved.digest);
    const auto second = fixture("\0\x30\x20\x10"sv);
    ASSERT_EQ(appearance::save(profile, second).state, status::custom);
    EXPECT_EQ(appearance::read(profile).png, second);
    EXPECT_NE(appearance::read(profile).digest, saved.digest);
    ASSERT_EQ(appearance::reset(profile).state, status::default_image);
    EXPECT_EQ(appearance::reset(profile).state, status::default_image);
    EXPECT_EQ(appearance::read(profile).state, status::default_image);
    EXPECT_TRUE(fs::is_empty(profile));
  }

  TEST_F(AppearanceProfile, RejectsTraversalAndNonregularFile) {
    const auto png = fixture();
    EXPECT_EQ(appearance::save(profile / ".." / "profile", png).state, status::storage_error);
    EXPECT_EQ(appearance::read("relative-profile").state, status::storage_error);
    ASSERT_TRUE(fs::create_directory(image));
    EXPECT_EQ(appearance::save(profile, png).state, status::storage_error);
    EXPECT_EQ(appearance::reset(profile).state, status::storage_error);
    EXPECT_EQ(appearance::read(profile).state, status::storage_error);
    EXPECT_TRUE(fs::is_directory(image));
  }

  TEST_F(AppearanceProfile, RejectsHardLinkedImage) {
    const auto outside = root / "outside.png";
    std::ofstream {outside, std::ios::binary} << fixture();
    fs::create_hard_link(outside, image);
    EXPECT_EQ(appearance::read(profile).state, status::storage_error);
    EXPECT_EQ(appearance::save(profile, fixture()).state, status::storage_error);
    EXPECT_EQ(appearance::reset(profile).state, status::storage_error);
    EXPECT_TRUE(fs::equivalent(image, outside));
  }

  TEST_F(AppearanceProfile, RejectsLinkedProfileOrImage) {
    const auto alternate = root / "alternate";
    std::error_code error;
    fs::create_directory_symlink(profile, alternate, error);
    if (error) GTEST_SKIP() << "Test process cannot create symlinks";
    EXPECT_EQ(appearance::read(alternate).state, status::storage_error);
    EXPECT_EQ(appearance::save(alternate, fixture()).state, status::storage_error);
    const auto outside = root / "outside.png";
    std::ofstream {outside, std::ios::binary} << fixture();
    fs::create_symlink(outside, image);
    EXPECT_EQ(appearance::read(profile).state, status::storage_error);
    EXPECT_EQ(appearance::save(profile, fixture()).state, status::storage_error);
    EXPECT_EQ(appearance::reset(profile).state, status::storage_error);
    EXPECT_TRUE(fs::is_symlink(image));
  }

#ifdef _WIN32
  TEST_F(AppearanceProfile, FailedReplacementPreservesOldImage) {
    const auto first = fixture();
    ASSERT_EQ(appearance::save(profile, first).state, status::custom);
    const auto handle = CreateFileW(image.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    const std::unique_ptr<void, decltype(&CloseHandle)> locked {handle, CloseHandle};
    EXPECT_EQ(appearance::save(profile, fixture("\0\x30\x20\x10"sv)).state, status::storage_error);
    EXPECT_EQ(appearance::read(profile).png, first);
    EXPECT_EQ(std::distance(fs::directory_iterator(profile), fs::directory_iterator()), 1);
  }
#endif
}

#endif
