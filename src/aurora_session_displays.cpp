/**
 * @file src/aurora_session_displays.cpp
 * @brief Bind display operations to one capture and to leases scoped to that stream.
 */
#include "aurora_session_displays.h"
#include "platform/macos/aurora_virtual_monitor_policy.h"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace aurora::session_display {
  namespace {
    using namespace std::literals;
    using display_device::mac_virtual_monitor::legal_mode;
    constexpr std::size_t max_leases = display_device::mac_virtual_monitor::slots_t::limit;
    // Keep exact removal retries safe if Quartz reuses a retired ID during this stream.
    constexpr std::size_t max_retired = 64;

    /** @brief Read a positive decimal integer without signs, escapes or trailing data. */
    bool number(std::string_view text, unsigned &value) {
      const auto end = text.data() + text.size();
      const auto parsed = std::from_chars(text.data(), end, value);
      return parsed.ec == std::errc {} && parsed.ptr == end && value != 0;
    }

    /** @brief Keep exact pixel geometry and use the negotiated rate when Quartz has no usable rate. */
    std::optional<display_mode> default_mode(const result &topology, unsigned stream_fps) {
      const auto active = std::ranges::find(topology.displays, topology.active, &display::id);
      if (active == topology.displays.end() || !active->ready || active->width > 7680 || active->height > 4320) return {};
      const auto rate = active->refresh_rate;
      int fps = stream_fps <= 240 ? static_cast<int>(stream_fps) : 0;
      if (std::isfinite(rate) && rate >= 29.5 && rate < 240.5) {
        fps = static_cast<int>(std::lround(rate));
      }
      if (!legal_mode(static_cast<int>(active->width), static_cast<int>(active->height), fps)) return {};
      return display_mode {active->width, active->height, static_cast<unsigned>(fps)};
    }

    /** @brief Compare pixel aspect ratios without rounding or overflowing 32-bit arithmetic. */
    bool same_aspect(unsigned first_width, unsigned first_height, unsigned second_width, unsigned second_height) {
      return first_width && first_height && second_width && second_height &&
             std::uint64_t {first_width} * second_height == std::uint64_t {second_width} * first_height;
    }
  }

  std::optional<command> parse(std::string_view query) {
    if (query.empty() || query.size() > 128 || query.back() == '&') return {};
    std::optional<operation> action;
    unsigned values[4] {};
    constexpr std::string_view keys[] {"id", "width", "height", "fps"};
    while (!query.empty()) {
      const auto separator = query.find('&');
      const auto field = query.substr(0, separator);
      query = separator == query.npos ? std::string_view {} : query.substr(separator + 1);
      const auto equals = field.find('=');
      if (equals == field.npos) return {};
      const auto name = field.substr(0, equals);
      const auto value = field.substr(equals + 1);
      if (name == "operation"sv) {
        if (action) return {};
        if (value == "list"sv) action = operation::list;
        else if (value == "create"sv) action = operation::create;
        else if (value == "select"sv) action = operation::select;
        else if (value == "remove"sv) action = operation::remove;
        else return {};
      } else {
        const auto key = std::ranges::find(keys, name);
        if (key == std::end(keys)) return {};
        auto &parsed = values[key - keys];
        if (parsed || !number(value, parsed)) return {};
      }
    }
    if (!action) return {};
    const auto [id, width, height, fps] = values;
    const bool has_mode = width || height || fps;
    if (*action == operation::create) {
      if (id || (has_mode && (width > 7680 || height > 4320 || fps > 240 ||
          !legal_mode(static_cast<int>(width), static_cast<int>(height), static_cast<int>(fps))))) return {};
    } else if (has_mode || (*action == operation::list ? id != 0 : id == 0)) {
      return {};
    }
    return command {*action, id, {width, height, fps}};
  }

  result controller::describe(status state, display_id affected, const backend &platform,
    const std::function<bool()> &authorized, bool switching_available, display_mode stream_mode) const {
    if (!authorized()) return result {status::denied};
    const auto capture_state = capture.snapshot();
    if (!capture_state.active) return result {status::not_ready};
    auto topology = platform.topology();
    if (!topology) return result {status::not_ready};

    result reply {state};
    reply.affected = affected;
    if (capture_state.pending) {
      reply.pending = capture_state.pending->id;
      reply.generation = capture_state.pending->generation;
      reply.first_frame = capture_state.first_frame;
    }
    reply.failed = capture_state.failed;
    reply.can_select = switching_available;
    const bool inventory_allowed = (capture_state.inventory_safe || switching_available) && !capture_state.pending;
    reply.can_create = inventory_allowed && owned.size() < max_leases &&
      removed.size() < max_retired && platform.available();
    for (auto &target : *topology) {
      const auto owner = std::ranges::find(owned, target.id, &owned_display::id);
      // Never expose another Aurora component's private lease, even if Quartz can enumerate it.
      if (target.owned_virtual && owner == owned.end()) continue;
      if (owner != owned.end()) {
        target.owned_virtual = true;
        target.ready = target.ready && !owner->retiring;
        target.removable = inventory_allowed && target.id != capture_state.active;
      }
      if (target.id == capture_state.active && target.ready) reply.active = target.id;
      reply.displays.push_back(std::move(target));
    }
    for (const auto &owner : owned) {
      if (std::ranges::find(reply.displays, owner.id, &display::id) != reply.displays.end()) continue;
      reply.displays.push_back({owner.id, "Aurora Virtual Display", true, false,
        inventory_allowed && owner.id != capture_state.active, 0, 0, 0});
    }
    if (reply.displays.size() > 32) return result {status::not_ready};
    if (!reply.active) {
      reply.can_create = false;
      if (reply.state == status::ok) reply.state = status::not_ready;
    } else if (reply.can_select && !reply.pending) {
      const auto active = std::ranges::find(reply.displays, reply.active, &display::id);
      for (auto &target : reply.displays) {
        target.selectable = target.ready &&
                            same_aspect(active->width, active->height, target.width, target.height) &&
                            (!stream_mode.width || same_aspect(stream_mode.width, stream_mode.height, target.width, target.height));
      }
    }
    return authorized() ? std::move(reply) : result {status::denied};
  }

  result controller::apply(const command &request, const backend &platform, const std::function<bool()> &authorized,
    bool switching_available, display_mode stream_mode) {
    if (!authorized()) return result {status::denied};
    const auto state = capture.snapshot();
    if (!state.enabled) return result {status::unsupported};
    if (request.action == operation::select && !switching_available) return result {status::switch_unavailable};
    if (!state.active) return result {status::not_ready};
    const auto describe_now = [&](status state, display_id affected = 0) {
      return describe(state, affected, platform, authorized, switching_available, stream_mode);
    };
    const auto describe_created = [&](display_id id) {
      auto reply = describe_now(status::ok, id);
      const auto target = std::ranges::find(reply.displays, id, &display::id);
      if (reply.state == status::ok && (target == reply.displays.end() || !target->ready)) {
        reply.state = status::not_ready;
      }
      return reply;
    };

    switch (request.action) {
      case operation::list:
        return describe_now(status::ok);

      case operation::create: {
        if (!state.inventory_safe && !switching_available) return describe_now(status::not_ready);
        const auto topology = describe_now(status::ok);
        if (topology.state != status::ok) return topology;
        const auto mode = request.mode.width ? std::optional {request.mode} : default_mode(topology, stream_mode.fps);
        if (!mode) return describe_now(status::mode_unavailable);
        const auto same = std::ranges::find(owned, *mode, &owned_display::mode);
        if (same != owned.end()) {
          if (same->retiring) return describe_now(status::cleanup_unconfirmed, same->id);
          return same->native->ready() ? describe_created(same->id) : describe_now(status::not_ready, same->id);
        }
        if (state.pending) return describe_now(status::busy);
        if (owned.size() >= max_leases || removed.size() >= max_retired) return describe_now(status::capacity);
        if (!topology.can_create) return describe_now(status::unavailable);
        if (!authorized()) return result {status::denied};
        status failure = status::unavailable;
        auto created = platform.create(*mode, failure);
        if (!created) return describe_now(failure);
        const auto id = created->id();
        if (!authorized() || (!switching_available && !capture.snapshot().inventory_safe) ||
            !id || removed.contains(id) || !created->ready() ||
            std::ranges::find(owned, id, &owned_display::id) != owned.end()) {
          const bool closed = created->close();
          return describe_now(closed ? status::not_ready : status::cleanup_unconfirmed);
        }
        owned.push_back({id, *mode, std::shared_ptr<lease> {std::move(created)}});
        return describe_created(id);
      }

      case operation::select: {
        if (state.pending) {
          return describe_now(state.pending->id == request.id ? status::pending : status::busy, state.pending->id);
        }
        const auto topology = describe_now(status::ok);
        if (topology.state != status::ok) return topology;
        const auto target = std::ranges::find(topology.displays, request.id, &display::id);
        if (target == topology.displays.end()) return describe_now(status::not_found);
        if (!target->ready) return describe_now(status::not_ready);
        if (target->id != state.active && !target->selectable) return describe_now(status::incompatible_aspect);
        if (!authorized()) return result {status::denied};
        switch (capture.submit(request.id)) {
          case submit_result::confirmed: return describe_now(status::ok, request.id);
          case submit_result::pending: return describe_now(status::pending, request.id);
          case submit_result::busy: return describe_now(status::busy);
          case submit_result::unavailable: return describe_now(status::not_ready);
        }
        break;
      }

      case operation::remove: {
        const auto owner = std::ranges::find(owned, request.id, &owned_display::id);
        if (owner == owned.end()) {
          return describe_now(removed.contains(request.id) ? status::ok : status::not_found, request.id);
        }
        // The old target remains active for the purposes of deletion until the worker confirms retirement.
        if (state.active == request.id || (state.pending && state.pending->id == request.id)) {
          return describe_now(status::in_use, request.id);
        }
        if (state.pending) return describe_now(status::busy, request.id);
        if (!switching_available && !capture.snapshot().inventory_safe) return describe_now(status::not_ready, request.id);
        if (!authorized()) return result {status::denied};
        owner->retiring = true;
        if (!owner->native->close()) return describe_now(status::cleanup_unconfirmed, request.id);
        removed.insert(request.id);
        owned.erase(owner);
        return describe_now(status::ok, request.id);
      }
    }
    return result {status::invalid_request};
  }

  bool controller::exclusive() const {
    return !owned.empty() || capture.owns_override();
  }

  bool controller::lost_target() const {
    const auto active = capture.snapshot().active;
    const auto owner = std::ranges::find(owned, active, &owned_display::id);
    return owner != owned.end() && (owner->retiring || !owner->native->ready());
  }

  std::shared_ptr<const lease> controller::lease_for(display_id id) const {
    const auto owner = std::ranges::find(owned, id, &owned_display::id);
    if (owner == owned.end() || owner->retiring) return {};
    return owner->native;
  }

  bool controller::cleanup() {
    bool closed = true;
    for (const auto &owner : owned) {
      if (!owner.native->close()) closed = false;
    }
    owned.clear();
    return closed;
  }
}
