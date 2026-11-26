/**
 * @file tests/scripts/aurora_mac_capture_target_test.cpp
 * @brief Offline checks for exact capture geometry and absolute pointer targeting.
 */
#include "src/platform/macos/aurora_capture_geometry.h"

#include <cstdio>
#include <cstdlib>

namespace {
  using namespace platf::mac_capture;

  /** @brief Stop with the failing fixture name. */
  void require(bool ok, const char *what) {
    if (!ok) {
      std::fprintf(stderr, "%s\n", what);
      std::exit(1);
    }
  }
}  // namespace

/** @brief Test global Quartz mapping without accessing the host display or cursor. */
int main() {
  const auto right = make_geometry(1470, 0, 1280, 720, 1920, 1080);
  const auto above_left = make_geometry(-960, -540, 960, 540, 1920, 1080);
  require(right && above_left, "fractional scale and Retina layouts");
  require(!make_geometry(0.5, 0, 1280, 720, 1920, 1080) && !make_geometry(0, 0, 1280, 721, 1920, 1080) && !make_geometry(0, 0, 0, 720, 1920, 1080), "reject unrepresentable geometry");
  require(same_geometry(*right, *right) && !same_geometry(*right, *above_left), "target revision geometry");
  const auto resized = make_geometry(1470, 0, 1280, 720, 2560, 1440);
  require(resized && !same_geometry(*right, *resized), "backing mode change invalidates old capture");
  require(compatible_output(*right, 1280, 720) && !compatible_output(*right, 1024, 768) && !compatible_output(*right, 0, 0) && !compatible_output(*right, 161, 91), "output size and aspect checks");

  const auto center = absolute_point(*right, 1470, 0, 1280, 720, 2110, 360);
  require(center && center->x == 2110 && center->y == 360, "global position is not translated twice");
  const auto edge = absolute_point(*right, 1470, 0, 1280, 720, 2750, 720);
  require(edge && edge->x == 2749 && edge->y == 719, "right and bottom clamp before adjacent monitor");
  const auto negative = absolute_point(*above_left, -960, -540, 960, 540, -1000, -1000);
  require(negative && negative->x == -960 && negative->y == -540, "negative global origin");
  const auto negative_edge = absolute_point(*above_left, -960, -540, 960, 540, 0, 0);
  require(negative_edge && negative_edge->x == -1 && negative_edge->y == -1, "negative edge stays off the primary monitor");
  require(!absolute_point(*right, -960, -540, 960, 540, -100, -100) && !absolute_point(*right, 1470, 0, 1200, 720, 2110, 360), "discard old touch-port geometry");
  require(!absolute_point(*right, 1470, 0, 1280, 720, std::numeric_limits<double>::quiet_NaN(), 0), "discard invalid pointer");
  double vertical = 0.0;
  double horizontal = 0.0;
  require(scroll_pixels(6, vertical) == 0 && scroll_pixels(-120, horizontal) == -10 &&
            scroll_pixels(6, vertical) == 1 && scroll_pixels(0, horizontal) == 0,
          "keep subpixel wheel motion and axis signs independent");
  std::puts("PASS: exact Mac capture geometry, scaling, stale touch ports, and absolute pointer bounds");
}
