/**
 * @file src/aurora_privacy_appearance.h
 * @brief Bounded storage for the managed Aurora privacy screensaver image.
 */
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace aurora::privacy_appearance {
  inline constexpr std::size_t max_png_bytes = 8 * 1024 * 1024;  ///< Maximum uploaded or stored PNG size.
  inline constexpr unsigned max_png_edge = 4096;  ///< Maximum width or height in pixels.
  inline constexpr unsigned max_png_pixels = 8 * 1024 * 1024;  ///< Maximum decoded pixel count.

  /** @brief Result of inspecting or changing the managed image. */
  enum class status { custom, default_image, invalid_image, storage_error };

  /** @brief Storage result, with bytes for GET and a lower-case SHA-256 for custom images. */
  struct result {
    status state = status::storage_error;  ///< Operation outcome.
    std::string png;  ///< Current PNG contents, populated by read().
    std::string digest;  ///< SHA-256 of the exact custom PNG contents.
  };

  /** @brief Verify a bounded, non-interlaced, opaque 8-bit RGB PNG. */
  bool valid_png(std::string_view png);

  /** @brief Read the fixed PNG in the authorized host profile. */
  result read(const std::filesystem::path &profile);

  /** @brief Validate a PNG and atomically save it in the authorized host profile. */
  result save(const std::filesystem::path &profile, std::string_view png);

  /** @brief Remove the fixed custom PNG in the authorized host profile. */
  result reset(const std::filesystem::path &profile);
}
