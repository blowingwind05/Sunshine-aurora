/**
 * @file src/platform/macos/aurora_virtual_monitor.mm
 * @brief Session-scoped Mac virtual monitor ownership and Quartz placement.
 *
 * Adapted from Aurora's VirtualDisplay.h/.mm handoff. Private classes are resolved
 * at runtime and the session transaction uses the public Quartz Display Services API.
 */
#include "aurora_virtual_monitor.h"

#include "aurora_virtual_monitor_policy.h"

#include <chrono>
#import <CoreGraphics/CoreGraphics.h>
#include <cstdlib>
#import <dispatch/dispatch.h>
#import <Foundation/Foundation.h>
#include <initializer_list>
#include <mutex>
#include <thread>

/** @brief Dynamically resolved descriptor setters used by the Aurora handoff. */
@protocol AuroraDisplayDescriptor
@property (nonatomic, strong) NSString *name;
@property (nonatomic, strong) dispatch_queue_t queue;
@property (nonatomic) unsigned vendorID;
@property (nonatomic) unsigned productID;
@property (nonatomic) unsigned serialNum;
@property (nonatomic) unsigned serialNumber;
@property (nonatomic) unsigned maxPixelsWide;
@property (nonatomic) unsigned maxPixelsHigh;
@property (nonatomic) CGSize sizeInMillimeters;
@property (nonatomic) CGPoint whitePoint;
@property (nonatomic) CGPoint redPrimary;
@property (nonatomic) CGPoint greenPrimary;
@property (nonatomic) CGPoint bluePrimary;
@end

/** @brief Dynamically resolved virtual display mode initializer. */
@protocol AuroraDisplayMode
- (instancetype)initWithWidth:(unsigned)width height:(unsigned)height refreshRate:(double)rate;
@end

/** @brief Dynamically resolved virtual display settings setters. */
@protocol AuroraDisplaySettings
@property (nonatomic, strong) NSArray *modes;
@property (nonatomic) unsigned hiDPI;
@end

/** @brief Dynamically resolved virtual display object and its assigned Quartz ID. */
@protocol AuroraDisplayOwner
- (instancetype)initWithDescriptor:(id<AuroraDisplayDescriptor>)descriptor;
- (BOOL)applySettings:(id<AuroraDisplaySettings>)settings;
@property (nonatomic, readonly) CGDirectDisplayID displayID;
@end

namespace display_device {
  namespace {
    using namespace std::chrono_literals;
    using namespace mac_virtual_monitor;
    using steady_clock = std::chrono::steady_clock;
    using monitor_error = aurora_virtual_monitor_error;

    std::mutex registry_mutex;  ///< Serializes creation, release and enumeration within this process.
    slots_t registry;  ///< Capacity and exact owned IDs. Protected by registry_mutex.

    /** @brief Allocate a distinct descriptor serial per creation under the registry lock. */
    std::uint32_t next_serial() {
      static auto serial = arc4random_uniform(UINT32_MAX - 1) + 1;
      if (++serial == 0) {
        ++serial;
      }
      return serial;
    }

    /** @brief Private CoreGraphics classes used by the handoff. */
    struct native_api_t {
      Class descriptor;
      Class mode;
      Class settings;
      Class display;
    };

    /** @brief Reject missing classes and any required selector before touching topology. */
    bool supports(Class type, std::initializer_list<SEL> selectors) {
      return type && std::all_of(selectors.begin(), selectors.end(), [type](SEL selector) {
               return [type instancesRespondToSelector:selector];
             });
    }

    /** @brief Resolve the exact method surface used by the implementation. */
    std::optional<native_api_t> resolve_api() {
      native_api_t api {
        NSClassFromString(@"CGVirtualDisplayDescriptor"),
        NSClassFromString(@"CGVirtualDisplayMode"),
        NSClassFromString(@"CGVirtualDisplaySettings"),
        NSClassFromString(@"CGVirtualDisplay")
      };
      if (!supports(api.descriptor, {
            @selector(setName:), @selector(setQueue:), @selector(setVendorID:), @selector(setProductID:),
              @selector(setSerialNum:), @selector(setMaxPixelsWide:), @selector(setMaxPixelsHigh:),
              @selector(setSizeInMillimeters:), @selector(setWhitePoint:), @selector(setRedPrimary:),
              @selector(setGreenPrimary:), @selector(setBluePrimary:)
          }) ||
          !supports(api.mode, { @selector(initWithWidth:height:refreshRate:) }) || !supports(api.settings, { @selector(setModes:), @selector(setHiDPI:) }) || !supports(api.display, { @selector(initWithDescriptor:), @selector(applySettings:), @selector(displayID) })) {
        return std::nullopt;
      }
      return api;
    }

    /** @brief Take a complete online topology snapshot or report unavailable. */
    std::optional<topology_t> online_topology() {
      topology_t topology;
      std::array<CGDirectDisplayID, topology_t::limit> ids {};
      std::uint32_t count {};
      topology.main = CGMainDisplayID();
      if (CGGetOnlineDisplayList(static_cast<std::uint32_t>(ids.size()), ids.data(), &count) != kCGErrorSuccess || count >= ids.size()) {
        return std::nullopt;
      }
      for (std::uint32_t i = 0; i < count; ++i) {
        const auto id = ids[i];
        if (!id || topology.find(id) || !CGDisplayIsOnline(id)) {
          return std::nullopt;
        }
        const auto bounds = CGDisplayBounds(id);
        topology.displays[topology.count++] = {
          id,
          bounds.origin.x,
          bounds.origin.y,
          bounds.size.width,
          bounds.size.height,
          CGDisplayIsActive(id) && !CGDisplayIsAsleep(id),
          CGDisplayIsInMirrorSet(id) != 0,
          CGDisplayIsBuiltin(id) != 0,
          CGDisplayMirrorsDisplay(id)
        };
      }
      if (CGMainDisplayID() != topology.main) {
        return std::nullopt;
      }
      return topology;
    }

    /** @brief Submit changes to the new display only, scoped to the login session. */
    bool place_as_extension(CGDirectDisplayID id, origin_t origin) {
      CGDisplayConfigRef transaction = nullptr;
      if (CGBeginDisplayConfiguration(&transaction) != kCGErrorSuccess || !transaction) {
        return false;
      }

      CGError error = kCGErrorSuccess;
      if (CGDisplayMirrorsDisplay(id) != kCGNullDirectDisplay) {
        error = CGConfigureDisplayMirrorOfDisplay(transaction, id, kCGNullDirectDisplay);
      }
      if (error == kCGErrorSuccess) {
        error = CGConfigureDisplayOrigin(transaction, id, origin.x, origin.y);
      }
      if (error != kCGErrorSuccess) {
        CGCancelDisplayConfiguration(transaction);
        return false;
      }
      // Completion consumes the transaction even when WindowServer rejects it.
      return CGCompleteDisplayConfiguration(transaction, kCGConfigureForSession) == kCGErrorSuccess;
    }

    /** @brief Read the exact active display mode without falling back to the main screen. */
    std::optional<aurora_virtual_target_t> read_target(const display_t &display) {
      if (!display.active || display.mirrored || display.mirror_source || display.builtin || !std::isfinite(display.x) || !std::isfinite(display.y) || !std::isfinite(display.width) || !std::isfinite(display.height) || display.width <= 0 || display.height <= 0) {
        return std::nullopt;
      }
      const auto mode = CGDisplayCopyDisplayMode(display.id);
      if (!mode) {
        return std::nullopt;
      }
      const auto width = CGDisplayModeGetPixelWidth(mode);
      const auto height = CGDisplayModeGetPixelHeight(mode);
      const auto rate = CGDisplayModeGetRefreshRate(mode);
      CGDisplayModeRelease(mode);
      constexpr auto max_pixels = static_cast<std::size_t>(std::numeric_limits<int>::max());
      if (!width || !height || width > max_pixels || height > max_pixels || !std::isfinite(rate) || rate <= 0) {
        return std::nullopt;
      }
      return aurora_virtual_target_t {
        display.id,
        display.x,
        display.y,
        display.width,
        display.height,
        static_cast<int>(width),
        static_cast<int>(height),
        rate
      };
    }
  }  // namespace

  struct aurora_virtual_monitor_t::state_t {
    id<AuroraDisplayOwner> owner = nil;  ///< Releasing this object removes only its native display.
    std::optional<std::size_t> slot;  ///< Reservation in the Host registry.
    CGDirectDisplayID native_id = kCGNullDirectDisplay;  ///< Unique newly assigned ID once known.
    bool removal_pending = false;  ///< Owner was released and disappearance has yet to be observed.
  };

  aurora_virtual_monitor_t::aurora_virtual_monitor_t():
      state_ {std::make_unique<state_t>()} {}

  bool aurora_virtual_monitor_t::driver_available() {
    @autoreleasepool {
      return resolve_api().has_value();
    }
  }

  std::unique_ptr<aurora_virtual_monitor_t> aurora_virtual_monitor_t::create(int width, int height, int refresh_rate, monitor_error *error) {
    const auto report = [error](monitor_error result) {
      if (error) {
        *error = result;
      }
    };
    if (!legal_mode(width, height, refresh_rate)) {
      report(monitor_error::invalid_mode);
      return {};
    }

    // Keep the wrapper outside the lock's scope so an unwind releases the lock before its destructor.
    auto monitor = std::unique_ptr<aurora_virtual_monitor_t> {new aurora_virtual_monitor_t};
    std::lock_guard lock {registry_mutex};
    auto &state = *monitor->state_;
    const auto fail = [&](monitor_error reason) -> std::unique_ptr<aurora_virtual_monitor_t> {
      report(monitor->close_locked() ? reason : monitor_error::cleanup_unconfirmed);
      return {};
    };

    const auto before = online_topology();
    const auto origin = before ? extension_origin(*before, width) : std::nullopt;
    if (!origin) {
      return fail(monitor_error::no_active_desktop);
    }
    registry.reap(*before);
    state.slot = registry.reserve();
    if (!state.slot) {
      return fail(monitor_error::capacity);
    }

    auto registration = monitor_error::native_create;
    @autoreleasepool {
      @try {
        const auto api = resolve_api();
        if (!api) {
          registration = monitor_error::unavailable;
        } else {
          const auto w = static_cast<unsigned>(width);
          const auto h = static_cast<unsigned>(height);
          id<AuroraDisplayDescriptor> descriptor = [api->descriptor new];
          descriptor.name = @"Aurora Virtual Display";
          descriptor.queue = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
          descriptor.vendorID = 0x4155;
          descriptor.productID = 0xA701;
          const auto serial = next_serial();
          descriptor.serialNum = serial;
          if (supports(api->descriptor, { @selector(setSerialNumber:) })) {
            descriptor.serialNumber = serial;
          }
          descriptor.maxPixelsWide = w;
          descriptor.maxPixelsHigh = h;
          descriptor.sizeInMillimeters = CGSizeMake(w * 25.4 / 96.0, h * 25.4 / 96.0);
          descriptor.whitePoint = CGPointMake(0.3127, 0.3290);
          descriptor.redPrimary = CGPointMake(0.64, 0.33);
          descriptor.greenPrimary = CGPointMake(0.30, 0.60);
          descriptor.bluePrimary = CGPointMake(0.15, 0.06);

          id<AuroraDisplayMode> mode = [(id<AuroraDisplayMode>) [api->mode alloc]
            initWithWidth:w
                   height:h
              refreshRate:refresh_rate];
          id<AuroraDisplaySettings> settings = [api->settings new];
          if (descriptor && mode && settings) {
            settings.hiDPI = 0;
            settings.modes = @[mode];
            state.owner = [(id<AuroraDisplayOwner>) [api->display alloc] initWithDescriptor:descriptor];
            const auto assigned = state.owner.displayID;
            if (assigned && !before->find(assigned)) {
              state.native_id = assigned;
            }
            const bool configured = state.owner && (assigned == 0 || state.native_id != 0) && [state.owner applySettings:settings];
            const auto after_settings = state.owner.displayID;
            if (assigned && after_settings && assigned != after_settings) {
              // An inconsistent ID cannot prove which registration went offline.
              state.native_id = kCGNullDirectDisplay;
            } else if (!state.native_id && after_settings && !before->find(after_settings)) {
              state.native_id = after_settings;
            }
            if (configured && state.native_id && after_settings == state.native_id) {
              registration = monitor_error::none;
            }
          }
        }
      } @catch (NSException *) {
        registration = monitor_error::native_create;
      }
    }
    if (registration != monitor_error::none) {
      return fail(registration);
    }

    bool placed = false;
    const auto deadline = steady_clock::now() + 5s;
    while (steady_clock::now() < deadline) {
      if (const auto online = online_topology()) {
        if (!layout_preserved(*before, *online, state.native_id, true)) {
          return fail(monitor_error::topology_changed);
        }
        if (const auto *display = online->find(state.native_id)) {
          if (!placed) {
            if (!may_configure(*before, *online, state.native_id) || !place_as_extension(state.native_id, *origin)) {
              return fail(monitor_error::configure);
            }
            placed = true;
          } else if (const auto mode = read_target(*display);
                     mode && layout_preserved(*before, *online, state.native_id) &&
                     ready(*display, *origin, width, height, refresh_rate, mode->width, mode->height, mode->refresh_rate)) {
            registry.publish(*state.slot, state.native_id);
            report(monitor_error::none);
            return monitor;
          }
        }
      }
      std::this_thread::sleep_for(50ms);
    }
    return fail(monitor_error::not_ready);
  }

  aurora_virtual_monitor_t::~aurora_virtual_monitor_t() {
    if (state_->slot) {
      (void) close();
    }
  }

  bool aurora_virtual_monitor_t::close_locked() {
    auto &state = *state_;
    if (state.slot) {
      if (state.owner) {
        @autoreleasepool {
          state.owner = nil;
        }
        registry.retire(*state.slot, state.native_id);
        state.removal_pending = true;
      } else {
        registry.abandon(*state.slot);
      }
      state.slot.reset();
    }
    if (!state.removal_pending) {
      return true;
    }
    if (!state.native_id) {
      return false;
    }

    const auto deadline = steady_clock::now() + 3s;
    do {
      if (const auto online = online_topology()) {
        registry.reap(*online);
        if (!online->find(state.native_id)) {
          state.removal_pending = false;
          return true;
        }
      }
      if (NSThread.isMainThread) {
        break;
      }
      std::this_thread::sleep_for(50ms);
    } while (steady_clock::now() < deadline);
    return false;
  }

  bool aurora_virtual_monitor_t::close() {
    std::lock_guard lock {registry_mutex};
    return close_locked();
  }

  std::uint32_t aurora_virtual_monitor_t::display_id() const {
    std::lock_guard lock {registry_mutex};
    return state_->slot ? registry.published(*state_->slot) : 0;
  }

  bool aurora_virtual_monitor_t::matches(std::uint32_t id) const {
    std::lock_guard lock {registry_mutex};
    if (!id || !state_->slot || registry.published(*state_->slot) != id) {
      return false;
    }
    const auto online = online_topology();
    const auto *display = online ? online->find(id) : nullptr;
    return display && read_target(*display).has_value();
  }

  std::optional<std::vector<aurora_virtual_target_t>> aurora_virtual_monitor_t::enumerate_owned() {
    std::lock_guard lock {registry_mutex};
    const auto online = online_topology();
    if (!online) {
      return std::nullopt;
    }
    registry.reap(*online);
    std::vector<aurora_virtual_target_t> outputs;
    for (std::size_t i = 0; i < slots_t::limit; ++i) {
      const auto id = registry.published(i);
      if (const auto *display = id ? online->find(id) : nullptr) {
        if (const auto target = read_target(*display)) {
          outputs.push_back(*target);
        }
      }
    }
    return outputs;
  }

  bool aurora_virtual_monitor_t::owns_capture_id(std::uint32_t id) {
    std::lock_guard lock {registry_mutex};
    return registry.contains(id);
  }
}  // namespace display_device
