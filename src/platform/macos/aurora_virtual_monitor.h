/**
 * @file src/platform/macos/aurora_virtual_monitor.h
 * @brief Ownership of one session-scoped Aurora virtual monitor on macOS.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace display_device {
  /** @brief Current Quartz target and mode for an Aurora-owned monitor. */
  struct aurora_virtual_target_t {
    std::uint32_t id;  ///< CGDirectDisplayID, valid while the owned target is live.
    double x, y, logical_width, logical_height;  ///< Global Quartz bounds in points.
    int width, height;  ///< Actual display mode in pixels.
    double refresh_rate;  ///< Actual refresh rate in Hz.
  };

  /** @brief Reason that a requested virtual monitor could not be published. */
  enum class aurora_virtual_monitor_error {
    none,
    invalid_mode,
    unavailable,
    no_active_desktop,
    capacity,
    native_create,
    topology_changed,
    configure,
    not_ready,
    cleanup_unconfirmed
  };

  /** @brief Noncopyable lease for a display owned by this Host process. */
  class aurora_virtual_monitor_t {
  public:
    /** @brief Probe the native class and selector surface without creating a monitor. */
    [[nodiscard]] static bool driver_available();

    /**
     * @brief Create and verify an extended display from a Host worker.
     * @param width Requested even pixel width, 640–7680.
     * @param height Requested even pixel height, 480–4320.
     * @param refresh_rate Requested whole Hz, 30–240.
     * @param error Optional reason for failure.
     * @return Owner of the exact new display once online, placed and mode-verified.
     */
    [[nodiscard]] static std::unique_ptr<aurora_virtual_monitor_t> create(
      int width,
      int height,
      int refresh_rate,
      aurora_virtual_monitor_error *error = nullptr
    );

    /** @brief Release the owned monitor, with bounded removal confirmation. */
    ~aurora_virtual_monitor_t();

    aurora_virtual_monitor_t(const aurora_virtual_monitor_t &) = delete;
    aurora_virtual_monitor_t &operator=(const aurora_virtual_monitor_t &) = delete;

    /** @brief Return the current ID of this lease, or zero after release. */
    [[nodiscard]] std::uint32_t display_id() const;

    /** @brief Match an online, active and extended owned target by its exact Quartz ID. */
    [[nodiscard]] bool matches(std::uint32_t display_id) const;

    /** @brief Enumerate ready, live Aurora-owned targets, or report a failed snapshot. */
    [[nodiscard]] static std::optional<std::vector<aurora_virtual_target_t>> enumerate_owned();

    /**
     * @brief Release this lease after its capture stops. Safe to call again.
     * @return True when this lease is released and its owned ID is absent from an online snapshot.
     */
    [[nodiscard]] bool close();

  private:
    struct state_t;  ///< ARC-owned CoreGraphics object and its registry slot.
    explicit aurora_virtual_monitor_t();
    bool close_locked();
    std::unique_ptr<state_t> state_;  ///< Native lifetime state.
  };
}  // namespace display_device
