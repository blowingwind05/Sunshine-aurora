/**
 * @file src/aurora_session_display_capture.h
 * @brief Per-stream request and acknowledgement contract for an exact capture target.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>

namespace aurora::session_display {
  using display_id = std::uint32_t;  ///< Exact native Quartz display ID, zero is reserved.

  /** @brief A request may be acknowledged only by its assigned generation and exact target. */
  struct switch_request {
    std::uint64_t generation;  ///< Monotonic within this stream.
    display_id id;  ///< Requested native target.
    bool operator==(const switch_request &) const = default;
  };

  /** @brief A coherent snapshot of the capture worker's last confirmation and pending work. */
  struct capture_state {
    bool enabled = false;  ///< The stream can participate in this contract.
    bool inventory_safe = false;  ///< Ordinary capture excludes all staged session leases.
    display_id active = 0;  ///< Last target confirmed by the capture and input paths.
    std::optional<switch_request> pending;  ///< Queued or in progress.
    display_id failed = 0;  ///< Last rejected target, cleared by a new command.
  };

  /** @brief Result of asking the current stream to switch to a specific display. */
  enum class submit_result { confirmed, pending, busy, unavailable };

  /** @brief Owned by one stream and shared only with its capture/input paths. */
  class capture_target {
  public:
    /** @brief Arm the contract during eligible session allocation, before workers start. */
    void enable() {
      const std::lock_guard lock {mutex};
      state.enabled = true;
    }

    /** @brief Publish the initial target after a first frame and the matching input mapping are available. */
    bool report_initial(display_id id) {
      const std::lock_guard lock {mutex};
      if (!id || !state.enabled || state.active) return false;
      state.active = id;
      return true;
    }

    /**
     * @brief Report the observed ordinary capture source after a real frame.
     * @note Use only when normal capture and fallback exclude the session's staged leases.
     */
    bool report_inventory_ready(display_id id) {
      const std::lock_guard lock {mutex};
      if (!id || !state.enabled || exclusive || state.pending) return false;
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

    /** @brief Confirm only after the exact new target has produced a frame, input is mapped and old capture is retired. */
    bool confirm(const switch_request &request) {
      const std::lock_guard lock {mutex};
      if (!taken || state.pending != request || std::chrono::steady_clock::now() >= deadline) return false;
      state.active = request.id;
      state.pending.reset();
      taken = false;
      return true;
    }

    /** @brief Reject only if the prior confirmed capture and input target are still valid. Otherwise stop the stream. */
    bool reject(const switch_request &request) {
      const std::lock_guard lock {mutex};
      if (!taken || state.pending != request) return false;
      state.pending.reset();
      state.failed = request.id;
      taken = false;
      return true;
    }

    /** @brief Queue a target, collapsing retries for the pending or confirmed target. */
    submit_result submit(display_id id) {
      const std::lock_guard lock {mutex};
      if (!id || !state.enabled || !state.active) return submit_result::unavailable;
      if (state.pending) return state.pending->id == id ? submit_result::pending : submit_result::busy;
      if (state.active == id) return submit_result::confirmed;
      state.failed = 0;
      state.pending = switch_request {++generation, id};
      deadline = std::chrono::steady_clock::now() + std::chrono::seconds {8};
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
      taken = false;
    }

  private:
    mutable std::mutex mutex;
    capture_state state;
    std::chrono::steady_clock::time_point deadline {};
    std::uint64_t generation = 0;
    bool taken = false;
    bool exclusive = false;  ///< Once switched, retain exclusive capture until workers join.
  };
}
