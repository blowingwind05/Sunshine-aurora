/**
 * @file src/aurora_parent_lifetime.h
 * @brief Parent process shutdown signals for the managed Sunshine host.
 */
#pragma once

#include "platform/common.h"

#ifdef _WIN32
  #include <Windows.h>
#elif defined(__APPLE__)
  #include <cerrno>
  #include <fcntl.h>
  #include <poll.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

namespace aurora {
  class parent_lifetime {
  public:
    parent_lifetime() {
#ifdef _WIN32
      wchar_t stop_name[128];
      const auto length = GetEnvironmentVariableW(L"AURORA_SUNSHINE_STOP_EVENT", stop_name, _countof(stop_name));
      if (platf::aurora_profile().empty() || length == 0 || length >= _countof(stop_name)) {
        return;
      }
      stop_event_ = OpenEventW(SYNCHRONIZE, FALSE, stop_name);
      if (stop_event_) {
        SetEnvironmentVariableW(L"AURORA_SUNSHINE_PROFILE", nullptr);
        SetEnvironmentVariableW(L"AURORA_SUNSHINE_STOP_EVENT", nullptr);
        ready_ = true;
      }
#elif defined(__APPLE__)
      struct stat pipe {};
      if (platf::appdata().empty() || fstat(STDIN_FILENO, &pipe) != 0 || !S_ISFIFO(pipe.st_mode)) {
        return;
      }
      unsetenv("AURORA_SUNSHINE_PROFILE");
      // Keep the parent pipe out of programs launched by Sunshine.
      ready_ = fcntl(STDIN_FILENO, F_SETFD, FD_CLOEXEC) != -1;
#endif
    }

    parent_lifetime(const parent_lifetime &) = delete;
    parent_lifetime &operator=(const parent_lifetime &) = delete;

#ifdef _WIN32
    ~parent_lifetime() {
      if (stop_event_) {
        CloseHandle(stop_event_);
      }
    }
#endif

    explicit operator bool() const {
      return ready_;
    }

    bool wait_for_stop() const {
#ifdef _WIN32
      return WaitForSingleObject(stop_event_, 200) != WAIT_TIMEOUT;
#elif defined(__APPLE__)
      // Closing the pipe also covers an unexpected exit of Aurora.
      pollfd descriptor {STDIN_FILENO, 0, 0};
      const auto ready = poll(&descriptor, 1, 200);
      return (ready > 0 && (descriptor.revents & (POLLHUP | POLLERR | POLLNVAL))) ||
             (ready < 0 && errno != EINTR);
#else
      return true;
#endif
    }

  private:
    bool ready_ = false;
#ifdef _WIN32
    HANDLE stop_event_ = nullptr;
#endif
  };
}  // namespace aurora
