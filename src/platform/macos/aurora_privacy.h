/**
 * @file src/platform/macos/aurora_privacy.h
 * @brief Lifetime and shared capture object for a protected Aurora session.
 */
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string_view>

#include <CoreGraphics/CoreGraphics.h>
#include <CoreMedia/CoreMedia.h>

namespace aurora_privacy {
  struct session_t;
  using session_ptr = std::shared_ptr<session_t>;

  void run_main_loop(const std::function<bool()> &quitting);
  session_ptr begin(std::string_view selected_display, int width, int height, int fps);
  void end(const session_ptr &session);
  session_ptr current();
  bool healthy(const session_ptr &session);
  CGDirectDisplayID display_id(const session_ptr &session);
  CMSampleBufferRef copy_first_frame(const session_ptr &session);
  CMSampleBufferRef copy_frame(const session_ptr &session, std::chrono::nanoseconds timeout);
}
