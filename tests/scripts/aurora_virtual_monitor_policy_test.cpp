/**
 * @file tests/scripts/aurora_virtual_monitor_policy_test.cpp
 * @brief Offline tests for the production virtual monitor guards. Does not link CoreGraphics.
 */
#include "src/platform/macos/aurora_virtual_monitor_policy.h"

#include <cstdio>
#include <cstdlib>

namespace {
  using namespace display_device::mac_virtual_monitor;

  /** @brief Fail the fixture with the applicable guard name. */
  void require(bool ok, const char *message) {
    if (!ok) {
      std::fprintf(stderr, "%s\n", message);
      std::exit(1);
    }
  }

  /** @brief Build a snapshot with a physical primary and an existing display to its right. */
  topology_t original_layout() {
    topology_t topology;
    topology.main = 10;
    topology.displays[0] = {10, 0, 0, 1470, 956, true, false, true, 0};
    topology.displays[1] = {20, 1470, -100, 1920, 1080, true, false, false, 0};
    topology.count = 2;
    return topology;
  }
}  // namespace

/** @brief Exercise mode, placement, topology, activation and rollback-slot policy. */
int main() {
  require(legal_mode(1920, 1080, 60), "normal mode");
  require(legal_mode(640, 480, 30) && legal_mode(7680, 4320, 240), "mode limits");
  require(!legal_mode(-1920, 1080, 60) && !legal_mode(1921, 1080, 60) && !legal_mode(1920, 0, 60) && !legal_mode(1920, 1081, 60) && !legal_mode(7682, 1080, 60) && !legal_mode(1920, 4322, 60) && !legal_mode(1920, 1080, 29) && !legal_mode(1920, 1080, 241), "reject invalid mode");

  const auto before = original_layout();
  require(!extension_origin({}, 1920), "headless placement");
  auto malformed = before;
  malformed.displays[1].width = std::numeric_limits<double>::infinity();
  require(!extension_origin(malformed, 1920), "invalid geometry");
  malformed.displays[1].width = std::numeric_limits<int>::max();
  require(!extension_origin(malformed, 1920), "placement bounds overflow");
  const auto origin = extension_origin(before, 1920);
  require(origin && origin->x == 3390 && origin->y == 0, "placement to the right of all active outputs");

  auto after = before;
  after.displays[after.count++] = {42, 3390, 0, 1920, 1080, true, false, false, 0};
  require(may_configure(before, after, 42), "new owned output eligible");
  require(!may_configure(before, after, 20) && !may_configure(before, after, 0), "reject preexisting and invalid IDs");
  auto moved = after;
  moved.displays[0].x = 1;
  require(!may_configure(before, moved, 42), "reject changed physical geometry");
  moved = after;
  moved.main = 20;
  require(!may_configure(before, moved, 42), "reject changed main display");
  moved = after;
  moved.displays[moved.count++] = {43, -640, 0, 640, 480, true, false, false, 0};
  require(!may_configure(before, moved, 42), "reject unrelated hotplug");
  moved = after;
  moved.displays[2].builtin = true;
  require(!may_configure(before, moved, 42), "never configure a built-in display");

  auto mirroring = after;
  mirroring.displays[0].mirrored = true;
  mirroring.displays[2].mirrored = true;
  mirroring.displays[2].mirror_source = 10;
  require(may_configure(before, mirroring, 42), "only the new display may be unmirrored");
  require(!layout_preserved(before, mirroring, 42), "do not publish a transient mirror layout");
  mirroring.displays[2].mirror_source = 20;
  require(!may_configure(before, mirroring, 42), "reject unrelated mirror change");

  const auto &virtual_display = after.displays[2];
  require(ready(virtual_display, *origin, 1920, 1080, 60, 1920, 1080, 59.94), "verified mode and origin");
  require(!ready(virtual_display, *origin, 1920, 1080, 60, 1280, 720, 60) && !ready(virtual_display, *origin, 1920, 1080, 60, 1920, 1080, 0) && !ready(virtual_display, {1470, 0}, 1920, 1080, 60, 1920, 1080, 60), "reject mismatched mode and origin");

  slots_t slots;
  const auto first = slots.reserve();
  const auto second = slots.reserve();
  require(first && second && *first != *second && !slots.reserve(), "Host-wide capacity includes pending creates");
  require(!slots.published(*first), "pending output must not be enumerated");
  slots.publish(*second, 43);
  slots.retire(*first, 42);
  slots.reap(after);
  require(!slots.reserve() && slots.published(*second) == 43 && !slots.published(*first), "failed owner still online holds only its slot");
  slots.reap(before);
  require(slots.reserve() == first && slots.published(*second) == 43, "exact released ID can free capacity");
  slots.abandon(*first);
  require(slots.reserve() == first, "failure before native allocation frees its slot");
  slots.retire(*first, 0);
  slots.reap(before);
  require(!slots.reserve(), "unknown native ID cannot yield an unverified reusable slot");
  std::puts("PASS: virtual monitor mode, extension, topology, activation and owned-slot rollback");
}
