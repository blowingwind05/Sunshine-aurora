/**
 * @file src/platform/macos/aurora_privacy_coverage.h
 * @brief Coverage checks for the Mac privacy curtain using WindowServer metadata.
 */
#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

namespace aurora_privacy::coverage {
  using clock = std::chrono::steady_clock;  ///< Clock for the most recent WindowServer confirmation.
  inline constexpr auto snapshot_interval = std::chrono::milliseconds {100};  ///< Minimum interval between routine snapshots.
  inline constexpr auto confirmation_timeout = 4 * snapshot_interval;  ///< Maximum age of a confirmation while delivering video.

  /** @brief Rectangle in global Quartz screen coordinates. */
  struct rect_t {
    double x, y, width, height;
  };

  /** @brief Public metadata for an onscreen window. */
  struct window_t {
    std::uint32_t id;
    std::int64_t owner_pid;
    std::int64_t layer;
    double alpha;
    rect_t bounds;
  };

  /** @brief Whether the curtain covers the display or may be recovered by reordering. */
  enum class result {
    unconfirmed,
    covered,
    same_layer_overlap
  };

  /** @brief Check that the rectangle has meaningful, finite coordinates. */
  inline bool valid_rect(rect_t rect) {
    return std::isfinite(rect.x) && std::isfinite(rect.y) &&
           rect.width >= 0 && rect.height >= 0 &&
           std::isfinite(rect.x + rect.width) && std::isfinite(rect.y + rect.height);
  }

  /** @brief Check overlap of nonempty rectangles, excluding shared edges. */
  constexpr bool overlaps(rect_t a, rect_t b) {
    return a.width > 0 && a.height > 0 && b.width > 0 && b.height > 0 &&
           a.x < b.x + b.width && b.x < a.x + a.width &&
           a.y < b.y + b.height && b.y < a.y + a.height;
  }

  /**
   * @brief Find the exact curtain in a front-to-back onscreen snapshot.
   * @param windows WindowServer snapshot in front-to-back order.
   * @param curtain_id ID used for the ScreenCaptureKit exclusion.
   * @param owner_pid PID of the host process.
   * @param layer Expected maximum window layer.
   * @param display Display bounds in the same Quartz coordinate system.
   * @return A covered result requires a full-size opaque curtain and no overlapping window in front.
   */
  inline result inspect(const std::vector<window_t> &windows, std::uint32_t curtain_id, std::int64_t owner_pid, std::int64_t layer, rect_t display) {
    if (curtain_id == 0 || !valid_rect(display) || display.width == 0 || display.height == 0) {
      return result::unconfirmed;
    }
    bool same_layer_overlap = false;
    for (const auto &window : windows) {
      if (window.id == 0 || !std::isfinite(window.alpha) || window.alpha < 0 || window.alpha > 1 || !valid_rect(window.bounds)) {
        return result::unconfirmed;
      }
      if (window.id == curtain_id) {
        const auto &bounds = window.bounds;
        if (window.owner_pid != owner_pid || window.layer != layer || window.alpha != 1 || bounds.x != display.x || bounds.y != display.y || bounds.width != display.width || bounds.height != display.height) {
          return result::unconfirmed;
        }
        return same_layer_overlap ? result::same_layer_overlap : result::covered;
      }
      if (window.alpha > 0 && overlaps(window.bounds, display)) {
        if (window.layer != layer) {
          return result::unconfirmed;
        }
        same_layer_overlap = true;
      }
    }
    return result::unconfirmed;
  }

  /** @brief Allow one reorder for a verified same-layer conflict, then require fresh coverage. */
  template<class Observe, class Reorder>
  bool confirm(Observe observe, Reorder reorder) {
    const auto first = observe();
    if (first == result::same_layer_overlap) {
      reorder();
      return observe() == result::covered;
    }
    return first == result::covered;
  }

  /** @brief Whether a WindowServer confirmation is recent enough to keep streaming. */
  inline bool fresh(clock::time_point checked, clock::time_point now) {
    return checked != clock::time_point {} && now >= checked && now - checked < confirmation_timeout;
  }
}  // namespace aurora_privacy::coverage
