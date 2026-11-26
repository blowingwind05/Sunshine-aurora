/**
 * @file src/platform/macos/aurora_capture_target.h
 * @brief Exact display selection and pointer routing for one managed macOS capture session.
 */
#pragma once

#include <CoreGraphics/CoreGraphics.h>
#include <functional>
#include <memory>

namespace platf {
  struct touch_port_t;
  using aurora_capture_lease_check = std::function<bool(CGDirectDisplayID)>;  ///< Proves the current session owns the exact virtual ID.

  /** @brief Exclusive target selection held for the lifetime of the shared capture. */
  class aurora_capture_target_t {
  public:
    /**
     * @brief Select an authorized physical output or a live Aurora virtual lease.
     * @param display_id Quartz ID from the session's authorized inventory.
     * @param owns_virtual Empty for a physical output, otherwise verifies the session's live virtual lease.
     * @return True when a new selection revision was published, including a retry of the same ID.
     */
    [[nodiscard]] bool switch_to(CGDirectDisplayID display_id, const aurora_capture_lease_check &owns_virtual = {});

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
    friend std::unique_ptr<aurora_capture_target_t> aurora_select_capture_target(CGDirectDisplayID, const aurora_capture_lease_check &);
  };

  /**
   * @brief Pin an exact authorized output for an exclusive stream.
   * @param display_id Quartz ID from the session's authorized inventory.
   * @param owns_virtual Empty for a physical output, otherwise verifies the session's live virtual lease.
   * @return Selection guard, or null if unavailable or another selection is active.
   */
  [[nodiscard]] std::unique_ptr<aurora_capture_target_t> aurora_select_capture_target(
    CGDirectDisplayID display_id,
    const aurora_capture_lease_check &owns_virtual = {}
  );

  /** @brief Whether an exact target is selected, including pending and failed selections. */
  [[nodiscard]] bool aurora_capture_target_active();

  /** @brief Current exact ID after the backend verified its first real sample, or zero. */
  [[nodiscard]] CGDirectDisplayID aurora_capture_target_ready();

  /**
   * @brief Route a converted absolute mouse move without falling back to the primary-display backend.
   * @param port Display geometry from the active encoder touch port.
   * @param x Quartz global horizontal position from the common input conversion.
   * @param y Quartz global vertical position from the common input conversion.
   * @param delivered Optional output set when the move reached the selected display.
   * @return True if exact mode consumed the move, including when it dropped stale input.
   */
  bool aurora_capture_absolute_mouse(const touch_port_t &port, float x, float y, bool *delivered = nullptr);

  /** @brief Consume a client button at the last valid exact position while a selection exists. */
  bool aurora_capture_button_mouse(int button, bool release);

  /** @brief Consume signed Moonlight scroll deltas at the last valid exact position. */
  bool aurora_capture_scroll(int vertical, int horizontal);

  /** @brief Release exact mouse buttons at their old anchor and disarm injection before switching. */
  void aurora_capture_pause_pointer();
}  // namespace platf
