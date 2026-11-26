/**
 * @file src/platform/macos/aurora_capture_geometry.h
 * @brief Pure geometry policy for the exact Mac capture and pointer route.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace platf::mac_capture {
  /** @brief Source pixel dimensions and its Quartz global logical rectangle. */
  struct geometry_t {
    int x, y, width, height;
    int pixel_width, pixel_height;
  };

  /** @brief Convert bounds that can be represented by Sunshine's integer touch-port format. */
  inline std::optional<geometry_t> make_geometry(double x, double y, double width, double height, std::size_t pixels_wide, std::size_t pixels_high) {
    constexpr auto max_size = static_cast<std::size_t>(std::numeric_limits<int>::max());
    const auto integral = [](double value) {
      return std::isfinite(value) && value >= std::numeric_limits<int>::min() &&
             value <= std::numeric_limits<int>::max() && std::trunc(value) == value;
    };
    if (!integral(x) || !integral(y) || !integral(width) || !integral(height) || width <= 0 || height <= 0 || !pixels_wide || !pixels_high || pixels_wide > max_size || pixels_high > max_size) {
      return std::nullopt;
    }
    if (static_cast<std::int64_t>(width) * static_cast<std::int64_t>(pixels_high) != static_cast<std::int64_t>(height) * static_cast<std::int64_t>(pixels_wide)) {
      return std::nullopt;
    }
    return geometry_t {static_cast<int>(x), static_cast<int>(y), static_cast<int>(width), static_cast<int>(height), static_cast<int>(pixels_wide), static_cast<int>(pixels_high)};
  }

  /** @brief Whether the pinned display's origin, logical bounds and source mode still match. */
  constexpr bool same_geometry(const geometry_t &a, const geometry_t &b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height &&
           a.pixel_width == b.pixel_width && a.pixel_height == b.pixel_height;
  }

  /** @brief Accept a VideoToolbox output size that preserves the source's aspect ratio. */
  constexpr bool compatible_output(const geometry_t &source, int width, int height) {
    return width > 0 && height > 0 && width % 2 == 0 && height % 2 == 0 &&
           static_cast<std::int64_t>(width) * source.pixel_height ==
             static_cast<std::int64_t>(height) * source.pixel_width;
  }

  /** @brief A Quartz point in the global coordinate space. */
  struct point_t {
    double x, y;
  };

  /** @brief Reject a stale touch port and clamp the global point inside the selected display. */
  inline std::optional<point_t> absolute_point(const geometry_t &target, int port_x, int port_y, int port_width, int port_height, double x, double y) {
    if (port_x != target.x || port_y != target.y || port_width != target.width || port_height != target.height || !std::isfinite(x) || !std::isfinite(y)) {
      return std::nullopt;
    }
    return point_t {
      std::clamp(x, static_cast<double>(target.x), static_cast<double>(target.x) + target.width - 1),
      std::clamp(y, static_cast<double>(target.y), static_cast<double>(target.y) + target.height - 1)
    };
  }
}  // namespace platf::mac_capture
