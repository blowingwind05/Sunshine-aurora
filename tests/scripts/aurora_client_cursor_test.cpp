/**
 * @file tests/scripts/aurora_client_cursor_test.cpp
 * @brief Offline fixture for Sunshine's cursor negotiation and RTSP admission policy.
 *
 * From the source root, compile with:
 * c++ -std=c++17 -I. tests/scripts/aurora_client_cursor_test.cpp -o aurora-client-cursor-test
 */

#include <array>
#include <iostream>
#include <map>
#include <string>

#include "src/aurora_client_cursor.h"

using aurora::client_cursor::compatible;
using aurora::client_cursor::launch_result;
using aurora::client_cursor::negotiate;
using aurora::client_cursor::requested;

using args_t = std::multimap<std::string, std::string>;

int main() {
  const args_t opted_in {{"auroraClientCursor", "1"}, {"unrelated", "1"}};
  if (!requested(opted_in, true) || requested(opted_in, false) ||
      requested(args_t {}, true) || requested(args_t {{"auroraClientCursor", ""}}, true) ||
      requested(args_t {{"auroraClientCursor", "0"}}, true) ||
      requested(args_t {{"auroraClientCursor", "true"}}, true) ||
      requested(args_t {{"auroraClientCursor", "01"}}, true) ||
      requested(args_t {{"auroraClientCursor", "1"}, {"auroraClientCursor", "0"}}, true) ||
      requested(args_t {{"auroraClientCursor", "1"}, {"auroraClientCursor", "1"}}, true)) {
    std::cerr << "Invalid authenticated query handling\n";
    return 1;
  }

  struct launch_case {
    const char *name;
    bool verified;
    bool pending;
    bool pending_excludes;
    bool active;
    bool active_excludes;
    launch_result expected;
  };
  const std::array launches {
    launch_case {"verified backend", true, false, false, false, false, launch_result::cursorless},
    launch_case {"unsupported backend or no opt-in", false, false, false, false, false, launch_result::legacy},
    launch_case {"legacy active", true, false, false, true, false, launch_result::legacy},
    launch_case {"cursorless active", true, false, false, true, true, launch_result::cursorless},
    launch_case {"legacy attempt with cursorless active", false, false, false, true, true, launch_result::legacy},
    launch_case {"cursorless slot already pending", true, true, true, false, false, launch_result::pending_conflict},
    launch_case {"legacy attempt with cursorless slot pending", false, true, true, false, false, launch_result::pending_conflict},
    launch_case {"legacy slot already pending", true, true, false, false, false, launch_result::legacy},
    launch_case {"ordinary legacy retry", false, true, false, false, false, launch_result::legacy},
    launch_case {"previous cursorless capture ended", false, false, false, false, true, launch_result::legacy}
  };
  for (const auto &test : launches) {
    if (negotiate(test.verified, test.pending, test.pending_excludes, test.active, test.active_excludes) != test.expected) {
      std::cerr << "Incorrect launch result: " << test.name << '\n';
      return 1;
    }
  }

  if (!compatible(false, false, true) || !compatible(false, true, false) ||
      !compatible(true, false, false) || !compatible(true, true, true) ||
      compatible(true, true, false) || compatible(true, false, true)) {
    std::cerr << "Incorrect RTSP admission of a shared capture mode\n";
    return 1;
  }

  std::cout << "PASS: 9 query checks, 10 launch cases, 6 RTSP admission cases\n";
}
