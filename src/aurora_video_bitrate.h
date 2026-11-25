/**
 * @file src/aurora_video_bitrate.h
 * @brief Bounded request and response state for Aurora's per-stream bitrate control.
 */
#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace aurora::video_bitrate {
  constexpr int min_requested_kbps = 500;  ///< Minimum Aurora client bitrate setting.
  constexpr int max_requested_kbps = 150000;  ///< Maximum Aurora client bitrate setting.
  constexpr auto request_timeout = std::chrono::milliseconds(750);  ///< Queue and response deadline.

  /** @brief Validated request. An empty token denotes a status query. */
  struct request_t {
    std::string_view token;  ///< The session token supplied by the caller.
    int kbps = 0;  ///< Requested client budget for a set command.
    std::uint32_t revision = 0;  ///< Revision returned by status.
  };

  /** @brief Parse exactly one supported query with an empty body. */
  inline std::optional<request_t> parse(std::string_view query, bool empty_body) {
    constexpr std::string_view set_prefix = "action=set&session=";
    constexpr std::string_view revision_prefix = "&revision=";
    constexpr std::string_view kbps_prefix = "&kbps=";
    if (!empty_body) {
      return std::nullopt;
    }
    if (query == "action=status") {
      return request_t {};
    }
    if (!query.starts_with(set_prefix) || query.size() < set_prefix.size() + 32) {
      return std::nullopt;
    }
    query.remove_prefix(set_prefix.size());
    const auto token = query.substr(0, 32);
    if (token.find_first_not_of("0123456789abcdef") != std::string_view::npos) {
      return std::nullopt;
    }
    query.remove_prefix(32);
    if (!query.starts_with(revision_prefix)) {
      return std::nullopt;
    }
    query.remove_prefix(revision_prefix.size());
    const auto kbps_start = query.find(kbps_prefix);
    if (kbps_start == std::string_view::npos || kbps_start == 0 || kbps_start > 10 ||
        (kbps_start > 1 && query.front() == '0')) {
      return std::nullopt;
    }
    std::uint32_t revision;
    const auto revision_end = query.data() + kbps_start;
    const auto parsed_revision = std::from_chars(query.data(), revision_end, revision);
    if (parsed_revision.ec != std::errc {} || parsed_revision.ptr != revision_end) {
      return std::nullopt;
    }

    query.remove_prefix(kbps_start + kbps_prefix.size());
    if (query.empty() || query.size() > 10 || query.front() < '1' || query.front() > '9') {
      return std::nullopt;
    }
    int kbps;
    const auto end = query.data() + query.size();
    const auto parsed_kbps = std::from_chars(query.data(), end, kbps);
    if (parsed_kbps.ec != std::errc {} || parsed_kbps.ptr != end) {
      return std::nullopt;
    }
    return request_t {token, kbps, revision};
  }

  /** @brief Combine fresh cryptographic randomness with a process-unique sequence. */
  inline std::string make_token(const std::array<unsigned char, 12> &random, std::uint32_t sequence) {
    constexpr std::string_view hex = "0123456789abcdef";
    std::string token;
    token.reserve(32);
    for (const auto byte : random) {
      token.push_back(hex[byte >> 4]);
      token.push_back(hex[byte & 15]);
    }
    for (int shift = 28; shift >= 0; shift -= 4) {
      token.push_back(hex[(sequence >> shift) & 15]);
    }
    return token;
  }

  /** @brief Apply the RTSP FEC, audio and packet overhead allowances in kbps. */
  inline std::int64_t adjusted_encoder_kbps(std::int64_t configured_kbps, int fec_percentage, int audio_kbps) {
    // Keep the upstream float division and truncation to the configured bitrate type.
    if (fec_percentage <= 80) {
      configured_kbps /= 100.f / (100 - fec_percentage);
    }
    configured_kbps -= std::min(std::int64_t {audio_kbps}, configured_kbps / 5);
    configured_kbps -= std::min(std::int64_t {500}, configured_kbps / 10);
    return configured_kbps;
  }

  /** @brief Session budget captured from RTSP before Sunshine deducts overhead. */
  struct budget_t {
    int negotiated_kbps;  ///< Largest client budget announced for this stream.
    int fec_percentage;  ///< FEC percentage selected at negotiation.
    int audio_kbps;  ///< Audio reserve before applying the proportional cap.
    int host_max_kbps;  ///< Positive host encoder cap, or zero for no cap.

    /** @brief Compute the live encoder target with the optional host ceiling. */
    int encoder_kbps(int requested) const {
      const auto target = static_cast<int>(adjusted_encoder_kbps(requested, fec_percentage, audio_kbps));
      return host_max_kbps > 0 ? std::min(target, host_max_kbps) : target;
    }
  };

  /**
   * @brief A single pending bitrate change shared by HTTPS and the owning encoder thread.
   *
   * The encoder calls attach, apply and detach. The streaming session calls close
   * when stopping. Neither the session registry nor this mutex is held across a driver call.
   */
  class control_t {
  public:
    /** @brief Initialize one session with its private token and immutable budget. */
    control_t(std::string token, budget_t budget):
        token_ {std::move(token)}, budget_ {budget}, requested_kbps_ {budget.negotiated_kbps} {}

    /** @brief Report the capabilities of the current encoder. Zero means no live support. */
    void attach(int encoder_kbps) {
      std::lock_guard lock {mutex_};
      if (encoder_ == encoder_state::closed) {
        return;
      }
      if (encoder_kbps <= 0) {
        encoder_ = encoder_state::unsupported;
      } else if (!encoder_kbps_ || encoder_kbps_ == encoder_kbps) {
        encoder_kbps_ = encoder_kbps;
        encoder_ = encoder_state::ready;
      }
    }

    /** @brief Return the last confirmed encoder target for an internal capture rebuild. */
    int resume_target() {
      std::lock_guard lock {mutex_};
      return encoder_kbps_;
    }

    /** @brief Pause control for encoder teardown while retaining the confirmed target. */
    void detach() {
      std::lock_guard lock {mutex_};
      if (encoder_ != encoder_state::closed) {
        encoder_ = encoder_state::waiting;
        interrupt();
      }
    }

    /** @brief Revoke control for the rest of this streaming session. */
    void close() {
      std::lock_guard lock {mutex_};
      encoder_ = encoder_state::closed;
      interrupt();
    }

    /** @brief Publish a usable revision only after the preceding operation has settled. */
    std::string status() {
      std::lock_guard lock {mutex_};
      if (const auto blocked = unavailable_reason(); !blocked.empty()) {
        return std::string {blocked};
      }
      return pending_ ? "busy" : describe("status");
    }

    /** @brief Atomically validate and consume the revision on accepting a change. */
    std::string_view submit(const request_t &request) {
      std::lock_guard lock {mutex_};
      if (request.token != token_) {
        return "stale";
      }
      if (const auto blocked = unavailable_reason(); !blocked.empty()) {
        return blocked;
      }
      if (pending_) {
        return "busy";
      }
      if (request.revision != revision_) {
        return "stale";
      }
      if (request.kbps < min_requested_kbps || request.kbps > budget_.negotiated_kbps) {
        return "rejected";
      }
      ++revision_;
      pending_.emplace(change_t {request.kbps, clock_t::now() + request_timeout});
      return {};
    }

    /** @brief Await an accepted change without holding a session registry lock. */
    std::string await() {
      std::unique_lock lock {mutex_};
      if (!pending_) {
        return "unavailable";
      }
      if (!ready_.wait_until(lock, pending_->deadline, [this] { return pending_->state == change_state::finished; })) {
        if (pending_->state == change_state::applying) {
          pending_->state = change_state::waiting_for_driver;
          return "pending";
        }
        pending_.reset();
        return "expired";
      }
      const auto result = pending_->result;
      pending_.reset();
      return result == "applied" ? describe(result) : std::string {result};
    }

    /** @brief Apply at most one unexpired request before the next frame on the encoder thread. */
    template<class Apply>
    void apply(Apply reconfigure) {
      int requested;
      std::uint32_t accepted_revision;
      {
        std::lock_guard lock {mutex_};
        if (encoder_ != encoder_state::ready || !pending_ || pending_->state != change_state::queued ||
            clock_t::now() >= pending_->deadline) {
          return;
        }
        pending_->state = change_state::applying;
        requested = pending_->kbps;
        accepted_revision = revision_;
      }

      const int target = budget_.encoder_kbps(requested);
      const int confirmed = reconfigure(target);
      const bool applied = confirmed > 0 && confirmed == target;
      std::lock_guard lock {mutex_};
      if (!pending_ || revision_ != accepted_revision || encoder_ != encoder_state::ready ||
          pending_->state == change_state::finished) {
        return;
      }
      if (applied) {
        requested_kbps_ = requested;
        encoder_kbps_ = confirmed;
      }
      if (pending_->state == change_state::waiting_for_driver) {
        pending_.reset();
      } else {
        finish(applied ? "applied" : "failed");
      }
    }

  private:
    using clock_t = std::chrono::steady_clock;
    enum class encoder_state { waiting, ready, unsupported, closed };
    enum class change_state { queued, applying, waiting_for_driver, finished };

    struct change_t {
      int kbps;
      clock_t::time_point deadline;
      change_state state = change_state::queued;
      std::string_view result;
    };

    /** @brief Return why new operations cannot proceed. Requires mutex_. */
    std::string_view unavailable_reason() const {
      if (encoder_ == encoder_state::unsupported || revision_ == (std::numeric_limits<std::uint32_t>::max)()) {
        return "unsupported";
      }
      return encoder_ == encoder_state::ready ? std::string_view {} : "unavailable";
    }

    /** @brief Cancel the old encoder's pending operation. Requires mutex_. */
    void interrupt() {
      if (!pending_ || pending_->state == change_state::finished) {
        return;
      }
      if (pending_->state == change_state::waiting_for_driver) {
        pending_.reset();
      } else {
        finish("unavailable");
      }
    }

    void finish(std::string_view result) {
      pending_->state = change_state::finished;
      pending_->result = result;
      ready_.notify_one();
    }

    std::string describe(std::string_view type) const {
      return std::string {type} + ' ' + token_ + ' ' + std::to_string(requested_kbps_) +
             ' ' + std::to_string(encoder_kbps_) + ' ' + std::to_string(budget_.negotiated_kbps) +
             ' ' + std::to_string(revision_);
    }

    const std::string token_;
    const budget_t budget_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::optional<change_t> pending_;
    int requested_kbps_;
    int encoder_kbps_ = 0;
    std::uint32_t revision_ = 0;
    encoder_state encoder_ = encoder_state::waiting;
  };
}  // namespace aurora::video_bitrate
