/**
 * @file src/platform/macos/aurora_capture_target.h
 * @brief Exact virtual-display selection for one managed macOS capture session.
 */
#pragma once

#include <CoreGraphics/CoreGraphics.h>
#include <memory>

namespace platf {
  struct touch_port_t;

  /** @brief Exclusive target selection held for the lifetime of the shared capture. */
  class aurora_capture_target_t {
  public:
    /**
     * @brief Select another live Aurora-owned ID without releasing this session's selection.
     * @param display_id Quartz ID of the live virtual monitor.
     * @return True when a new selection revision was published, including a retry of the same ID.
     */
    [[nodiscard]] bool switch_to(CGDirectDisplayID display_id);

    /**
     * @brief Arm absolute moves after the controller has acknowledged the new video and old input was drained.
     * @param display_id ID acknowledged by the session.
     * @return True when this selection is still ready and owns that ID.
     */
    [[nodiscard]] bool enable_absolute_input(CGDirectDisplayID display_id);

    /** @brief Suspend absolute input before changing selection or stopping the session. */
    void disable_absolute_input();

    /** @brief Clear selection after its video and input workers have stopped. */
    ~aurora_capture_target_t();

    aurora_capture_target_t(const aurora_capture_target_t &) = delete;
    aurora_capture_target_t &operator=(const aurora_capture_target_t &) = delete;

  private:
    aurora_capture_target_t() = default;
    friend std::unique_ptr<aurora_capture_target_t> aurora_select_capture_target(CGDirectDisplayID);
  };

  /**
   * @brief Pin an exact live Aurora-owned ID before starting an exclusive stream.
   * @param display_id Quartz ID from the session's retained virtual-monitor lease.
   * @return Selection guard, or null if unavailable or another selection is active.
   */
  [[nodiscard]] std::unique_ptr<aurora_capture_target_t> aurora_select_capture_target(CGDirectDisplayID display_id);

  /** @brief Whether an exact target is selected, including pending and failed selections. */
  [[nodiscard]] bool aurora_capture_target_active();

  /** @brief Current exact ID after the backend verified its first real sample, or zero. */
  [[nodiscard]] CGDirectDisplayID aurora_capture_target_ready();

  /**
   * @brief Route a converted absolute mouse move without falling back to the primary-display backend.
   * @param port Display geometry from the active encoder touch port.
   * @param x Quartz global horizontal position from the common input conversion.
   * @param y Quartz global vertical position from the common input conversion.
   * @return True if exact mode consumed the move, including when it dropped stale input.
   */
  bool aurora_capture_absolute_mouse(const touch_port_t &port, float x, float y);
}  // namespace platf
