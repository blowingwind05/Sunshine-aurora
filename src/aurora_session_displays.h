/**
 * @file src/aurora_session_displays.h
 * @brief Session-owned display control and virtual-monitor lease policy.
 */
#pragma once

#include "aurora_session_display_capture.h"

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace aurora::session_display {
  /** @brief Supported authenticated operations. */
  enum class operation { list, create, select, remove };

  /** @brief Request for an exact extended virtual-display mode. */
  struct display_mode {
    unsigned width = 0, height = 0, fps = 0;
    bool operator==(const display_mode &) const = default;
  };

  /** @brief Validated HTTPS command. */
  struct command {
    operation action = operation::list;
    display_id id = 0;
    display_mode mode {};
  };

  /** @brief Result observed by the caller. */
  enum class status {
    ok, pending, unsupported, denied, invalid_request, not_ready, busy,
    not_found, in_use, capacity, unavailable, cleanup_unconfirmed, mode_unavailable, switch_unavailable, incompatible_aspect
  };

  /** @brief A display and its observed, rather than requested, properties. */
  struct display {
    display_id id;
    std::string name;
    bool owned_virtual;
    bool ready;
    bool removable;
    unsigned width, height;
    double refresh_rate;
    bool selectable = false;  ///< Ready exact owned target with a compatible pixel aspect ratio.
  };

  /** @brief HTTPS response snapshot. */
  struct result {
    explicit result(status state = status::unavailable): state {state} {}
    status state;
    display_id active = 0;
    display_id pending = 0;
    display_id failed = 0;
    display_id affected = 0;
    std::uint32_t generation = 0;
    std::optional<std::uint32_t> first_frame;
    bool can_create = false;
    bool can_select = false;  ///< True only in integrations that confirm a client-visible video generation.
    std::vector<display> displays;
  };

  /** @brief Parse the fixed empty-body command query without accepting duplicate or unknown fields. */
  std::optional<command> parse(std::string_view query);

  /** @brief Owned native virtual-monitor handle. */
  class lease {
  public:
    virtual ~lease() = default;
    virtual display_id id() const = 0;
    virtual bool ready() const = 0;
    /** @brief Release this lease and confirm that its target has gone offline. */
    virtual bool close() = 0;
  };

  /** @brief Native topology and owned-monitor operations. */
  class backend {
  public:
    virtual ~backend() = default;
    virtual bool available() const = 0;
    virtual std::optional<std::vector<display>> topology() const = 0;
    virtual std::unique_ptr<lease> create(display_mode mode, status &failure) const = 0;
  };

  /** @brief The managed macOS implementation. */
  const backend &mac_backend();

  /**
   * @brief Serialize lease decisions under the RTSP session registry lock.
   * @note The capture channel is independently synchronized for capture and input workers.
   */
  class controller {
  public:
    capture_target capture;  ///< Per-stream capture/input handoff.

    /** @brief Act while both the session and its certificate remain current. */
    result apply(const command &request, const backend &platform, const std::function<bool()> &authorized,
      bool switching_available, display_mode stream_mode = {});

    /** @brief Whether admitting a parallel stream would share a controlled display. */
    bool exclusive() const;

    /** @brief Whether a confirmed Aurora-owned capture target disappeared from Quartz. */
    bool lost_target() const;

    /** @brief Retain the live lease used to authorize an exact virtual target during reinit. */
    std::shared_ptr<const lease> lease_for(display_id id) const;

    /** @brief Release all leases after video and input have stopped. */
    bool cleanup();

  private:
    struct owned_display {
      display_id id;
      display_mode mode;
      std::shared_ptr<lease> native;
      bool retiring = false;
    };

    /** @brief Scope topology to the session and recheck authorization before publishing it. */
    result describe(status state, display_id affected, const backend &platform,
      const std::function<bool()> &authorized, bool switching_available, display_mode stream_mode) const;

    std::vector<owned_display> owned;
    std::set<display_id> removed;
  };
}
