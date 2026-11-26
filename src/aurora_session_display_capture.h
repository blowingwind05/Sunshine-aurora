/**
 * @file src/aurora_session_display_capture.h
 * @brief Per-stream request and acknowledgement contract for an exact capture target.
 */
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>

namespace aurora::session_display {
  using display_id = std::uint32_t;  ///< Exact native Quartz display ID, zero is reserved.

  /** @brief A request may be acknowledged only by its assigned generation and exact target. */
  struct switch_request {
    std::uint32_t generation;  ///< Nonzero, never reused within this stream.
    display_id id;  ///< Requested native target.
    bool operator==(const switch_request &) const = default;
  };

  /** @brief A coherent snapshot of the capture worker's last confirmation and pending work. */
  struct capture_state {
    bool enabled = false;  ///< The stream can participate in this contract.
    bool inventory_safe = false;  ///< Ordinary capture excludes all staged session leases.
    bool source_lost = false;  ///< The capture worker reported a changed or unknown source.
    display_id active = 0;  ///< Host-confirmed target, independent of the client presentation state.
    std::optional<switch_request> pending;  ///< Queued or in progress.
    std::optional<std::uint32_t> first_frame;  ///< First successfully transmitted real target IDR.
    display_id failed = 0;  ///< Last rejected target, cleared by a new command.
  };

  /** @brief Result of asking the current stream to switch to a specific display. */
  enum class submit_result { confirmed, pending, busy, unavailable };

  /** @brief Track which packets came from a converted real sample in one encoder instance. */
  class encoded_display_source {
  public:
    /** @brief Mark the first successful conversion and whether its capture excludes staged displays. */
    bool observe(display_id id, std::int64_t pts, bool ordinary, std::uint32_t generation = 0) {
      const bool first = id && !source;
      if (first) {
        source = id;
        first_real_pts = pts;
        inventory_safe = ordinary;
        source_generation = generation;
      }
      if (source && id == source && generation == source_generation) {
        real_pts[next++ % real_pts.size()] = pts;
      }
      return first;
    }

    /** @brief Attribute packets at or after the first real input PTS, including encoded duplicates. */
    display_id for_packet(std::int64_t pts) const {
      return source && pts >= first_real_pts ? source : 0;
    }

    /** @brief Whether an attributed packet is from capture that excludes staged displays. */
    bool allows_inventory() const {
      return inventory_safe;
    }

    /** @brief Attribute the packet to its encoder generation only when backed by the new real source. */
    std::uint32_t generation_for_packet(std::int64_t pts) const {
      return for_packet(pts) ? source_generation : 0;
    }

    /** @brief Distinguish actual converted samples from dummy and repeated encoder input. */
    bool is_real_sample(std::int64_t pts) const {
      return std::find(real_pts.begin(), real_pts.begin() + std::min(next, real_pts.size()), pts) !=
             real_pts.begin() + std::min(next, real_pts.size());
    }

  private:
    display_id source = 0;
    std::int64_t first_real_pts = 0;
    bool inventory_safe = false;
    std::uint32_t source_generation = 0;
    std::array<std::int64_t, 64> real_pts {};
    std::size_t next = 0;
  };

  /** @brief Owned by one stream and shared only with its capture/input paths. */
  class capture_target {
  public:
    /** @brief Arm the contract during eligible session allocation, before workers start. */
    void enable() {
      const std::lock_guard lock {mutex};
      state.enabled = true;
    }

    /** @brief Bind the first real converted source, and reject a later change before it can be encoded. */
    bool observe_source(display_id id, bool ordinary, std::uint32_t *observed_generation = nullptr) {
      const std::lock_guard lock {mutex};
      if (!state.enabled || state.source_lost) return false;
      if (state.pending && switching && id == state.pending->id && !ordinary) {
        target_observed = true;
        if (observed_generation) *observed_generation = state.pending->generation;
        return true;
      }
      if (!id || (source && (source != id || ordinary_source != ordinary))) {
        state.active = 0;
        state.inventory_safe = false;
        state.source_lost = true;
        return false;
      }
      source = id;
      ordinary_source = ordinary;
      if (observed_generation) *observed_generation = active_generation;
      return true;
    }

    /** @brief Publish a host-sent real target that does not guarantee staged display exclusion. */
    bool report_initial(display_id id) {
      const std::lock_guard lock {mutex};
      if (!id || !state.enabled || state.source_lost || source != id || state.active) return false;
      state.active = id;
      return true;
    }

    /**
     * @brief Report the ordinary source after the host sends a packet with the verified ID.
     */
    bool report_inventory_ready(display_id id) {
      const std::lock_guard lock {mutex};
      if (!id || !state.enabled || state.source_lost || source != id || !ordinary_source ||
          exclusive || state.pending || state.inventory_safe) return false;
      state.active = id;
      state.inventory_safe = true;
      return true;
    }

    /** @brief Suspend staging if the ordinary source or its lease exclusion can no longer be verified. */
    void suspend_inventory() {
      const std::lock_guard lock {mutex};
      state.inventory_safe = false;
      state.active = 0;
    }

    /** @brief Claim the next target once. The worker must confirm, reject, or stop the stream. */
    std::optional<switch_request> take_pending() {
      const std::lock_guard lock {mutex};
      if (taken || !state.pending || std::chrono::steady_clock::now() >= deadline) return {};
      taken = true;
      return state.pending;
    }

    /** @brief Allow the capture worker to observe the target only after the input queue has been frozen. */
    bool begin(const switch_request &request) {
      const std::lock_guard lock {mutex};
      if (!taken || switching || state.source_lost || state.pending != request || std::chrono::steady_clock::now() >= deadline) return false;
      switching = true;
      return true;
    }

    /** @brief Reject stale or dummy video as the capture worker and sender cross the generation boundary. */
    bool video_allowed(display_id id, std::uint32_t generation, bool real_idr) const {
      const std::lock_guard lock {mutex};
      if (state.source_lost) return false;
      if (state.pending && switching) {
        return target_observed && id == state.pending->id && generation == state.pending->generation && (state.first_frame || real_idr);
      }
      return active_generation == 0 || (generation == active_generation && id == state.active);
    }

    /** @brief Publish the first frame and record exact frame membership only after all frame datagrams were sent. */
    void video_sent(display_id id, std::uint32_t generation, std::uint32_t frame, bool real_idr) {
      const std::lock_guard lock {mutex};
      if (!switching || !target_observed || !state.pending || state.pending->id != id || state.pending->generation != generation ||
          std::chrono::steady_clock::now() >= deadline) return;
      if (!state.first_frame) {
        if (!real_idr) return;
        state.first_frame = frame;
      }
      sent_frames[next_frame++ % sent_frames.size()] = frame;
    }

    /** @brief Validate an encrypted presentation barrier before reopening the exact target's input. */
    bool presented(std::uint32_t generation, std::uint32_t frame, const std::function<bool(display_id)> &enable_input) {
      const std::lock_guard lock {mutex};
      if (!generation || !switching || state.source_lost || !state.pending || state.pending->generation != generation ||
          !state.first_frame || std::chrono::steady_clock::now() >= deadline) return false;
      const auto end = sent_frames.begin() + std::min(next_frame, sent_frames.size());
      if (std::find(sent_frames.begin(), end, frame) == end || !enable_input(state.pending->id)) return false;
      state.active = source = state.pending->id;
      ordinary_source = false;
      active_generation = generation;
      state.pending.reset();
      state.first_frame.reset();
      taken = switching = target_observed = false;
      next_frame = 0;
      return true;
    }

    /** @brief Queue a target, collapsing retries for the pending or confirmed target. */
    submit_result submit(display_id id) {
      const std::lock_guard lock {mutex};
      if (!id || !state.enabled || !state.active || state.source_lost) return submit_result::unavailable;
      if (state.pending) return state.pending->id == id ? submit_result::pending : submit_result::busy;
      if (state.active == id) return submit_result::confirmed;
      if (generation == std::numeric_limits<std::uint32_t>::max()) return submit_result::unavailable;
      state.failed = 0;
      state.pending = switch_request {++generation, id};
      deadline = std::chrono::steady_clock::now() + std::chrono::seconds {15};
      exclusive = true;
      return submit_result::pending;
    }

    /** @brief Read the latest public capture state without entering the stream registry. */
    capture_state snapshot() const {
      const std::lock_guard lock {mutex};
      return state;
    }

    /** @brief Keep other sessions off this capture while a remote override owns the stream. */
    bool owns_override() const {
      const std::lock_guard lock {mutex};
      return exclusive;
    }

    /** @brief Detect a switch without confirmation before it can remain pending indefinitely. */
    bool expired(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const {
      const std::lock_guard lock {mutex};
      return state.pending && now >= deadline;
    }

    /** @brief Revoke queued work and ignore later acknowledgements during teardown. */
    void close() {
      const std::lock_guard lock {mutex};
      state = {};
      source = 0;
      ordinary_source = false;
      taken = switching = target_observed = false;
      next_frame = 0;
    }

  private:
    mutable std::mutex mutex;
    capture_state state;
    display_id source = 0;  ///< Exact source observed by the encoder before sender confirmation.
    bool ordinary_source = false;  ///< Whether the source came from the ordinary, stage-excluding path.
    std::chrono::steady_clock::time_point deadline {};
    std::uint32_t generation = 0;
    std::uint32_t active_generation = 0;
    std::array<std::uint32_t, 4096> sent_frames {};  ///< Bounds membership even across the RTP frame-number wrap.
    std::size_t next_frame = 0;
    bool taken = false;
    bool switching = false;
    bool target_observed = false;
    bool exclusive = false;  ///< Once switched, retain exclusive capture until workers join.
  };
}
