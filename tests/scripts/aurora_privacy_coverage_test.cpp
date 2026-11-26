/**
 * @file tests/scripts/aurora_privacy_coverage_test.cpp
 * @brief Offline tests for the WindowServer curtain metadata and reorder policy.
 *
 * From the Sunshine source root:
 * c++ -std=c++17 -I. tests/scripts/aurora_privacy_coverage_test.cpp -o /tmp/aurora-privacy-coverage-test
 * /tmp/aurora-privacy-coverage-test
 */
#include "src/platform/macos/aurora_privacy_coverage.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
  namespace coverage = aurora_privacy::coverage;
  using coverage::result;
  using coverage::window_t;
  using namespace std::chrono_literals;

  constexpr std::int64_t level = 2147483631;  ///< Example maximum layer reported by WindowServer.
  constexpr coverage::rect_t screen {-25, 10, 1280, 720};
  constexpr window_t curtain {42, 111, level, 1, screen};
  constexpr window_t overlap {43, 222, level, 1, {-10, 20, 50, 50}};
  constexpr window_t transparent {44, 222, level, 0, screen};
  constexpr window_t outside {45, 222, level, 1, {1255, 10, 200, 200}};
  constexpr window_t higher {46, 222, level + 1, 1, screen};
  constexpr window_t lower {47, 222, level - 1, 1, screen};

  /** @brief Report a failing fixture and stop the test. */
  void require(bool condition, const char *message) {
    if (!condition) {
      std::fprintf(stderr, "%s\n", message);
      std::exit(1);
    }
  }
}  // namespace

/** @brief Exercise the production decision logic without windows, capture, or permissions. */
int main() {
  struct fixture_t {
    const char *name;
    std::vector<window_t> windows;
    result expected;
  };

  const std::array fixtures {
    fixture_t {"empty snapshot", {}, result::unconfirmed},
    fixture_t {"covered", {curtain}, result::covered},
    fixture_t {"same-layer overlap", {overlap, curtain}, result::same_layer_overlap},
    fixture_t {"same-layer overlap behind curtain", {curtain, overlap}, result::covered},
    fixture_t {"transparent same-layer window", {transparent, curtain}, result::covered},
    fixture_t {"off-display edge touch", {outside, curtain}, result::covered},
    fixture_t {"higher-layer overlap", {higher, curtain}, result::unconfirmed},
    fixture_t {"inconsistent lower window in front", {lower, curtain}, result::unconfirmed},
    fixture_t {"overlap with missing curtain", {overlap}, result::unconfirmed},
    fixture_t {"replacement ID", {{41, 111, level, 1, screen}}, result::unconfirmed},
    fixture_t {"foreign PID", {{42, 112, level, 1, screen}}, result::unconfirmed},
    fixture_t {"wrong layer", {{42, 111, level - 1, 1, screen}}, result::unconfirmed},
    fixture_t {"transparent curtain", {{42, 111, level, 0.95, screen}}, result::unconfirmed},
    fixture_t {"shifted geometry", {{42, 111, level, 1, {-24, 10, 1280, 720}}}, result::unconfirmed},
    fixture_t {"short geometry", {{42, 111, level, 1, {-25, 10, 1280, 719}}}, result::unconfirmed},
    fixture_t {"malformed front metadata", {{44, 222, level, std::numeric_limits<double>::quiet_NaN(), screen}, curtain}, result::unconfirmed}
  };
  for (const auto &fixture : fixtures) {
    require(coverage::inspect(fixture.windows, curtain.id, curtain.owner_pid, level, screen) == fixture.expected, fixture.name);
  }
  require(coverage::inspect({curtain}, curtain.id, curtain.owner_pid, level, {}) == result::unconfirmed, "missing display geometry");

  const auto check_reorder = [](result first, result after, bool expected, int expected_reorders) {
    int snapshots = 0;
    int reorders = 0;
    const bool confirmed = coverage::confirm(
      [&] {
        return snapshots++ == 0 ? first : after;
      },
      [&] {
        ++reorders;
      }
    );
    require(confirmed == expected && reorders == expected_reorders && snapshots == 1 + expected_reorders, "bounded reorder policy");
  };
  check_reorder(result::covered, result::unconfirmed, true, 0);
  check_reorder(result::unconfirmed, result::covered, false, 0);
  check_reorder(result::same_layer_overlap, result::covered, true, 1);
  check_reorder(result::same_layer_overlap, result::same_layer_overlap, false, 1);
  check_reorder(result::same_layer_overlap, result::unconfirmed, false, 1);

  const coverage::clock::time_point checked {1s};
  require(!coverage::fresh({}, checked), "missing confirmation");
  require(!coverage::fresh(checked, checked - 1ms), "confirmation from the future");
  require(coverage::fresh(checked, checked), "fresh confirmation");
  require(coverage::fresh(checked, checked + coverage::confirmation_timeout - 1ms), "confirmation just before timeout");
  require(!coverage::fresh(checked, checked + coverage::confirmation_timeout), "expired confirmation");
  std::puts("PASS: 17 metadata checks, 5 reorder cases, 5 confirmation-age checks");
}
