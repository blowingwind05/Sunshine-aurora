/**
 * @file src/platform/macos/aurora_session_displays.cpp
 * @brief Adapt Quartz topology and native Aurora monitor leases to the stream owner.
 */
#include "aurora_virtual_monitor.h"
#include "aurora_virtual_monitor_policy.h"
#include "src/aurora_session_displays.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include <CoreGraphics/CoreGraphics.h>

namespace aurora::session_display {
  namespace {
    using native_monitor = display_device::aurora_virtual_monitor_t;
    using native_error = display_device::aurora_virtual_monitor_error;

    /** @brief Keep the CoreGraphics monitor alive exactly as long as this lease. */
    class mac_lease final: public lease {
    public:
      explicit mac_lease(std::unique_ptr<native_monitor> native):
          native {std::move(native)} {}

      display_id id() const override {
        return native->display_id();
      }

      bool ready() const override {
        const auto current = id();
        return current && native->matches(current);
      }

      bool close() override {
        return native->close();
      }

    private:
      std::unique_ptr<native_monitor> native;
    };

    /** @brief Capture-independent snapshot of selectable, extended Quartz displays. */
    class quartz_backend final: public backend {
    public:
      bool available() const override {
        return native_monitor::driver_available();
      }

      std::optional<std::vector<display>> topology() const override {
        std::array<CGDirectDisplayID, display_device::mac_virtual_monitor::topology_t::limit> ids {};
        uint32_t count = 0;
        if (CGGetOnlineDisplayList(static_cast<uint32_t>(ids.size()), ids.data(), &count) != kCGErrorSuccess ||
            count >= ids.size()) return {};
        const auto virtuals = native_monitor::enumerate_owned();
        if (!virtuals) return {};

        std::vector<display> targets;
        targets.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
          const auto id = ids[i];
          if (!id) continue;
          const bool ready = CGDisplayIsActive(id) && !CGDisplayIsAsleep(id) && !CGDisplayIsInMirrorSet(id);
          const auto owned = std::ranges::find(*virtuals, id, &display_device::aurora_virtual_target_t::id);
          if (owned != virtuals->end()) {
            targets.push_back({id, "Aurora Virtual Display", true, ready, false,
              static_cast<unsigned>(owned->width), static_cast<unsigned>(owned->height), owned->refresh_rate});
            continue;
          }
          if (native_monitor::owns_capture_id(id)) continue;

          const auto mode = CGDisplayCopyDisplayMode(id);
          const auto width = mode ? static_cast<unsigned>(CGDisplayModeGetPixelWidth(mode)) : 0;
          const auto height = mode ? static_cast<unsigned>(CGDisplayModeGetPixelHeight(mode)) : 0;
          const auto refresh = mode ? CGDisplayModeGetRefreshRate(mode) : 0.0;
          if (mode) CGDisplayModeRelease(mode);
          auto name = CGDisplayIsBuiltin(id) ? "Built-in Display" : id == CGMainDisplayID() ? "Main Display" :
            "Display " + std::to_string(id);
          targets.push_back({id, std::move(name), false, ready && width && height, false,
            width, height, std::isfinite(refresh) && refresh > 0 ? refresh : 0.0});
        }
        return targets;
      }

      std::unique_ptr<lease> create(display_mode mode, status &failure) const override {
        native_error error = native_error::none;
        auto native = native_monitor::create(static_cast<int>(mode.width), static_cast<int>(mode.height),
          static_cast<int>(mode.fps), &error);
        if (native) return std::make_unique<mac_lease>(std::move(native));
        switch (error) {
          case native_error::invalid_mode: failure = status::invalid_request; break;
          case native_error::capacity: failure = status::capacity; break;
          case native_error::cleanup_unconfirmed: failure = status::cleanup_unconfirmed; break;
          case native_error::no_active_desktop:
          case native_error::topology_changed:
          case native_error::not_ready: failure = status::not_ready; break;
          default: failure = status::unavailable; break;
        }
        return {};
      }
    };
  }

  const backend &mac_backend() {
    static const quartz_backend platform;
    return platform;
  }
}
