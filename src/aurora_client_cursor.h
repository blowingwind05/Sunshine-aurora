/**
 * @file src/aurora_client_cursor.h
 * @brief Negotiation rules for Aurora's client-rendered desktop cursor.
 */
#pragma once

#include <iterator>
#include <string>

namespace aurora::client_cursor {
  /** @brief Accept exactly one opt-in value from the authenticated GameStream client. */
  template<class Args>
  bool requested(const Args &args, bool paired) {
    const auto [first, end] = args.equal_range("auroraClientCursor");
    return paired && first != end && first->second == "1" && std::next(first) == end;
  }

  /** @brief Whether the incoming video configuration can share the current capture. */
  constexpr bool compatible(bool has_active, bool active_excludes_cursor, bool incoming_excludes_cursor) {
    return !has_active || active_excludes_cursor == incoming_excludes_cursor;
  }

  /** @brief Result of publishing a GameStream launch ticket to the RTSP server. */
  enum class launch_result { legacy, cursorless, pending_conflict };

  /** @brief Reserve a verified cursorless capture only if the live and pending modes allow it. */
  constexpr launch_result negotiate(bool verified, bool pending, bool pending_excludes_cursor, bool has_active, bool active_excludes_cursor) {
    if (pending) {
      return pending_excludes_cursor ? launch_result::pending_conflict : launch_result::legacy;
    }
    return verified && compatible(has_active, active_excludes_cursor, true) ? launch_result::cursorless : launch_result::legacy;
  }
}  // namespace aurora::client_cursor
