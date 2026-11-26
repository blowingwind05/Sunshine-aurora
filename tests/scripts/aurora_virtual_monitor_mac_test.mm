/**
 * @file tests/scripts/aurora_virtual_monitor_mac_test.mm
 * @brief Native test entry for the Mac Host virtual monitor backend.
 */
#include "src/platform/macos/aurora_virtual_monitor.h"
#include "src/platform/macos/aurora_virtual_monitor_policy.h"

#import <CoreGraphics/CoreGraphics.h>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unistd.h>

namespace {
  using display_device::aurora_virtual_monitor_error;
  using display_device::aurora_virtual_monitor_t;
  using namespace display_device::mac_virtual_monitor;

  /** @brief Verify a condition while allowing lease destructors to run on failure. */
  int fail(const char *message, aurora_virtual_monitor_error error = aurora_virtual_monitor_error::none) {
    std::fprintf(stderr, "%s (backend status %d)\n", message, static_cast<int>(error));
    return 1;
  }

  /** @brief Independently record original online IDs and their physical layout. */
  std::optional<topology_t> read_layout() {
    topology_t layout;
    std::array<CGDirectDisplayID, topology_t::limit> displays {};
    std::uint32_t count {};
    if (CGGetOnlineDisplayList(static_cast<std::uint32_t>(displays.size()), displays.data(), &count) != kCGErrorSuccess || count >= displays.size()) {
      return std::nullopt;
    }
    layout.main = CGMainDisplayID();
    for (std::uint32_t i = 0; i < count; ++i) {
      const auto id = displays[i];
      const auto bounds = CGDisplayBounds(id);
      layout.displays[layout.count++] = {
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
    return layout;
  }

  /** @brief Exercise the production backend against a logged-in test WindowServer. */
  int smoke() {
    const auto original = read_layout();
    if (!original || !original->main) {
      return fail("could not snapshot the original layout");
    }
    auto error = aurora_virtual_monitor_error::none;

    auto one = aurora_virtual_monitor_t::create(1920, 1080, 60, &error);
    if (!one) {
      return fail("first monitor could not be created or verified", error);
    }
    const auto first_id = one->display_id();
    auto two = aurora_virtual_monitor_t::create(1280, 720, 60, &error);
    if (!two) {
      return fail("second monitor could not be created or verified", error);
    }
    const auto second_id = two->display_id();
    const auto targets = aurora_virtual_monitor_t::enumerate_owned();
    if (!first_id || !second_id || first_id == second_id || !one->matches(first_id) || one->matches(second_id) || !two->matches(second_id) || !targets || targets->size() != 2) {
      return fail("owned display IDs do not match two ready targets");
    }
    if (aurora_virtual_monitor_t::create(1920, 1080, 60, &error) || error != aurora_virtual_monitor_error::capacity) {
      return fail("third create did not hit the Host-wide capacity", error);
    }
    if (!two->close() || CGDisplayIsOnline(second_id) || !one->matches(first_id)) {
      return fail("release did not remove only the second monitor");
    }
    one.reset();
    const auto remaining = aurora_virtual_monitor_t::enumerate_owned();
    const auto restored = read_layout();
    if (CGDisplayIsOnline(first_id) || !remaining || !remaining->empty() || !restored || !layout_preserved(*original, *restored, 0)) {
      return fail("RAII release or original display layout verification failed");
    }
    std::printf("PASS: verified and removed only owned IDs %u and %u\n", first_id, second_id);
    return 0;
  }
}  // namespace

/** @brief Probe by default, with real creation only on explicit invocation on a test Mac. */
int main(int argc, const char *argv[]) {
  const bool run_smoke = argc == 2 && std::strcmp(argv[1], "--smoke") == 0;
  if (argc != 1 && !(argc == 2 && (run_smoke || std::strcmp(argv[1], "--probe") == 0))) {
    std::fputs("usage: aurora-mac-virtual-monitor-test [--probe|--smoke]\n", stderr);
    return 2;
  }
  @autoreleasepool {
    auto error = aurora_virtual_monitor_error::none;
    if (aurora_virtual_monitor_t::create(-1, 1080, 60, &error) || error != aurora_virtual_monitor_error::invalid_mode) {
      return fail("invalid mode should be rejected before native creation", error);
    }
    const bool available = aurora_virtual_monitor_t::driver_available();
    std::printf("CoreGraphics virtual-display selectors: %s\n", available ? "available" : "unavailable");
    if (!run_smoke) {
      return 0;
    }
    if (!available) {
      return fail("this OS cannot run the smoke test");
    }
    alarm(30);
    int result = 1;
    std::thread worker {[&] {
      result = smoke();
    }};
    worker.join();
    return result;
  }
}
