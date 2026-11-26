/**
 * @file src/platform/macos/aurora_virtual_monitor_policy.h
 * @brief Display request, layout and capacity validation for the Mac virtual monitor backend.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace display_device::mac_virtual_monitor {
  using display_id_t = std::uint32_t;  ///< Platform-neutral representation of CGDirectDisplayID.

  /** @brief Verify a mode before allocating anything or converting to unsigned values. */
  constexpr bool legal_mode(int width, int height, int refresh_rate) {
    return width >= 640 && width <= 7680 && width % 2 == 0 &&
           height >= 480 && height <= 4320 && height % 2 == 0 &&
           refresh_rate >= 30 && refresh_rate <= 240;
  }

  /** @brief Display state used to recognize an unrelated topology change. */
  struct display_t {
    display_id_t id;
    double x, y, width, height;
    bool active;
    bool mirrored;
    bool builtin;
    display_id_t mirror_source;  ///< Primary of this display's mirror set, or zero.
  };

  /** @brief One bounded online display snapshot. */
  struct topology_t {
    static constexpr std::size_t limit = 64;  ///< Snapshot capacity. A full native result is treated as truncated.
    display_id_t main = 0;  ///< Current main display.
    std::array<display_t, limit> displays {};  ///< Storage for online display states.
    std::size_t count = 0;  ///< Number of valid entries.

    /** @brief Find a window-server display ID in this snapshot. */
    const display_t *find(display_id_t id) const {
      const auto end = displays.begin() + count;
      const auto found = std::find_if(displays.begin(), end, [id](const auto &display) {
        return display.id == id;
      });
      return found == end ? nullptr : &*found;
    }
  };

  /** @brief Right-hand placement in global Quartz points. */
  struct origin_t {
    int x, y;
  };

  /** @brief Compute a safe origin to the right of all existing active displays. */
  inline std::optional<origin_t> extension_origin(const topology_t &topology, int width) {
    const auto *main = topology.find(topology.main);
    if (!main || !main->active) {
      return std::nullopt;
    }
    double right = main->x + main->width;
    for (std::size_t i = 0; i < topology.count; ++i) {
      const auto &display = topology.displays[i];
      if (!display.active) {
        continue;
      }
      const double edge = display.x + display.width;
      if (!std::isfinite(edge) || display.width <= 0 || display.height <= 0) {
        return std::nullopt;
      }
      right = std::max(right, edge);
    }
    const auto x = std::ceil(right);
    const auto y = std::round(main->y);
    if (!std::isfinite(x) || !std::isfinite(y) || x < std::numeric_limits<int>::min() || x > std::numeric_limits<int>::max() - width || y < std::numeric_limits<int>::min() || y > std::numeric_limits<int>::max()) {
      return std::nullopt;
    }
    return origin_t {static_cast<int>(x), static_cast<int>(y)};
  }

  /** @brief Accept only the original layout plus the new ID and its own temporary mirroring if allowed. */
  inline bool layout_preserved(const topology_t &before, const topology_t &current, display_id_t new_id, bool allow_new_mirror = false) {
    const auto *added = new_id ? current.find(new_id) : nullptr;
    if (before.main != current.main || (new_id && before.find(new_id)) || current.count != before.count + (added ? 1 : 0)) {
      return false;
    }
    for (std::size_t i = 0; i < before.count; ++i) {
      const auto &old = before.displays[i];
      const auto *now = current.find(old.id);
      if (!now || now->x != old.x || now->y != old.y || now->width != old.width || now->height != old.height || now->active != old.active || now->builtin != old.builtin || now->mirror_source != old.mirror_source) {
        return false;
      }
      const bool only_new_mirror = allow_new_mirror && added && added->mirror_source == old.id && !old.mirrored && now->mirrored;
      if (now->mirrored != old.mirrored && !only_new_mirror) {
        return false;
      }
    }
    return true;
  }

  /** @brief Candidate must be the new non-built-in target, never a preexisting display. */
  inline bool may_configure(const topology_t &before, const topology_t &current, display_id_t id) {
    const auto *display = id ? current.find(id) : nullptr;
    return display && !display->builtin && layout_preserved(before, current, id, true);
  }

  /** @brief Confirm extension geometry and the captureable pixel mode before publishing. */
  inline bool ready(const display_t &display, origin_t origin, int width, int height, int rate, int actual_width, int actual_height, double actual_rate) {
    return display.active && !display.mirrored && !display.mirror_source && !display.builtin &&
           display.x == origin.x && display.y == origin.y && display.width > 0 && display.height > 0 &&
           actual_width == width && actual_height == height && std::abs(actual_rate - rate) <= 0.1;
  }

  /** @brief Maintain a small Host-wide cap, including owned IDs whose removal is pending. */
  class slots_t {
  public:
    static constexpr std::size_t limit = 2;  ///< Maximum number of simultaneously retained slots.

    /** @brief Reserve a slot before calling the native constructor. */
    std::optional<std::size_t> reserve() {
      for (std::size_t i = 0; i < limit; ++i) {
        if (!slots_[i].used) {
          slots_[i].used = true;
          return i;
        }
      }
      return std::nullopt;
    }

    /** @brief Publish an ID only after the extended target has been verified. */
    void publish(std::size_t index, display_id_t id) {
      slots_[index].id = id;
    }

    /** @brief Keep a release in the cap until the exact ID disappears. Zero is unidentifiable. */
    void retire(std::size_t index, display_id_t id) {
      slots_[index] = {true, true, id};
    }

    /** @brief Free a slot that never acquired a native display object. */
    void abandon(std::size_t index) {
      slots_[index] = {};
    }

    /** @brief Reclaim retired known IDs absent from a successful online enumeration. */
    void reap(const topology_t &online) {
      for (auto &slot : slots_) {
        if (slot.retired && slot.id && !online.find(slot.id)) {
          slot = {};
        }
      }
    }

    /** @brief Return the published owner ID at this slot, or zero if unavailable. */
    display_id_t published(std::size_t index) const {
      return slots_[index].retired ? 0 : slots_[index].id;
    }

  private:
    /** @brief Slot reservation, publication and release state. */
    struct entry_t {
      bool used = false;
      bool retired = false;
      display_id_t id = 0;
    };

    std::array<entry_t, limit> slots_ {};
  };
}  // namespace display_device::mac_virtual_monitor
