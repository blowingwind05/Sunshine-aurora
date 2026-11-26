/**
 * @file src/platform/macos/aurora_privacy.mm
 * @brief Local AppKit curtain and ScreenCaptureKit exclusion for Aurora.
 */
#include "aurora_privacy.h"
#include "aurora_privacy_artwork.h"
#include "sc_capture.h"

#import <AppKit/AppKit.h>
#import <dispatch/dispatch.h>

#include <atomic>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <unistd.h>

#include "src/logging.h"
#include "src/platform/common.h"
#include "src/utility.h"

namespace aurora_privacy {
  using namespace std::chrono_literals;

  struct session_t {
    std::atomic<bool> valid {true};
    std::atomic<bool> armed {false};
    uint64_t generation {};
    CGDirectDisplayID display {};
    CGWindowID window {};
    SCCapture *capture {};
    CMSampleBufferRef first_frame {};

    ~session_t() {
      if (first_frame) {
        CFRelease(first_frame);
      }
    }
  };

  namespace {
    std::mutex session_mutex;
    std::weak_ptr<session_t> reserved_session;
    uint64_t next_generation {};

    // Read and mutate these only on the AppKit main thread.
    NSWindow *curtain;
    std::weak_ptr<session_t> curtain_session;
    uint64_t curtain_generation {};
    bool observing_display_changes = false;

    void display_reconfigured(CGDirectDisplayID, CGDisplayChangeSummaryFlags, void *) {
      std::lock_guard lock {session_mutex};
      if (auto session = reserved_session.lock()) {
        session->valid = false;
      }
    }

    bool has_single_builtin_display(CGDirectDisplayID display) {
      CGDirectDisplayID displays[2] {};
      uint32_t count {};
      return display != kCGNullDirectDisplay &&
             CGGetOnlineDisplayList(2, displays, &count) == kCGErrorSuccess && count == 1 &&
             displays[0] == display && CGDisplayIsBuiltin(display) && CGDisplayIsActive(display) &&
             !CGDisplayIsAsleep(display) && !CGDisplayIsInMirrorSet(display);
    }

    void invalidate_curtain() {
      if (const auto session = curtain_session.lock()) {
        session->valid = false;
      }
    }

    void remove_curtain(uint64_t generation) {
      if (curtain_generation == generation) {
        [curtain orderOut:nil];
        curtain = nil;
        curtain_session.reset();
        curtain_generation = 0;
      }
    }

    /**
     * @brief Load optional artwork off the AppKit thread and install it on the owning curtain.
     * @param generation The curtain generation that requested the artwork.
     */
    void load_curtain_artwork(uint64_t generation) {
      dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        @autoreleasepool {
          const auto image = load_artwork(platf::aurora_profile());
          if (!image) {
            return;
          }
          dispatch_async(dispatch_get_main_queue(), ^{
            if (const auto session = curtain_session.lock();
                curtain_generation == generation && session && session->valid && curtain.isVisible) {
              NSView *content = curtain.contentView;
              [content addSubview:create_artwork_view(content.bounds, image)];
            }
            CGImageRelease(image);
          });
        }
      });
    }

    bool show_curtain(const session_ptr &session, const std::string &configured_display) {
      if (!session->valid || curtain || !observing_display_changes || !CGPreflightScreenCaptureAccess()) {
        return false;
      }
      NSArray<NSScreen *> *screens = NSScreen.screens;
      const auto display = CGMainDisplayID();
      if (screens.count != 1 || !has_single_builtin_display(display) ||
          (!configured_display.empty() && configured_display != std::to_string(display))) {
        return false;
      }

      NSScreen *screen = screens.firstObject;
      if ([screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue] != display ||
          NSIsEmptyRect(screen.frame)) {
        return false;
      }

      NSWindow *window = [[NSWindow alloc] initWithContentRect:screen.frame
                                                   styleMask:NSWindowStyleMaskBorderless
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
      window.releasedWhenClosed = NO;
      window.opaque = YES;
      window.backgroundColor = NSColor.blackColor;
      window.hasShadow = NO;
      window.ignoresMouseEvents = YES;
      window.hidesOnDeactivate = NO;
      window.level = CGWindowLevelForKey(kCGScreenSaverWindowLevel);
      window.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                  NSWindowCollectionBehaviorFullScreenAuxiliary |
                                  NSWindowCollectionBehaviorStationary |
                                  NSWindowCollectionBehaviorIgnoresCycle;
      [window orderFrontRegardless];

      if (!session->valid || !window.isVisible || window.windowNumber <= 0 || !NSEqualRects(window.frame, screen.frame)) {
        [window orderOut:nil];
        return false;
      }
      session->display = display;
      session->window = static_cast<CGWindowID>(window.windowNumber);
      curtain = window;
      curtain_generation = session->generation;
      curtain_session = session;
      load_curtain_artwork(session->generation);
      return true;
    }

    SCContentFilter *create_filter(const session_ptr &session) {
      dispatch_semaphore_t ready = dispatch_semaphore_create(0);
      __block SCShareableContent *shareable;
      [SCShareableContent getShareableContentExcludingDesktopWindows:NO
                                                onScreenWindowsOnly:YES
                                                  completionHandler:^(SCShareableContent *content, NSError *error) {
        if (!error) {
          shareable = content;
        }
        dispatch_semaphore_signal(ready);
      }];
      if (dispatch_semaphore_wait(ready, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC)) != 0 ||
          !session->valid || shareable.displays.count != 1) {
        return nil;
      }

      SCDisplay *display = shareable.displays.firstObject;
      if (display.displayID != session->display) {
        return nil;
      }
      for (SCWindow *window in shareable.windows) {
        if (window.windowID == session->window && window.owningApplication.processID == getpid() &&
            window.isOnScreen && window.windowLayer >= CGWindowLevelForKey(kCGScreenSaverWindowLevel) &&
            CGRectContainsRect(window.frame, display.frame)) {
          return [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[window]];
        }
      }
      // The first shareable snapshot may predate WindowServer committing the curtain.
      return nil;
    }
  }

  void run_main_loop(const std::function<bool()> &quitting) {
    @autoreleasepool {
      [NSApplication sharedApplication];
      [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
      observing_display_changes = CGDisplayRegisterReconfigurationCallback(display_reconfigured, nullptr) == kCGErrorSuccess;
      NSNotificationCenter *app_notifications = NSNotificationCenter.defaultCenter;
      NSNotificationCenter *workspace_notifications = NSWorkspace.sharedWorkspace.notificationCenter;
      auto invalidate = ^(NSNotification *) { invalidate_curtain(); };
      auto display_observer = [app_notifications addObserverForName:NSApplicationDidChangeScreenParametersNotification
                                                            object:nil queue:NSOperationQueue.mainQueue usingBlock:invalidate];
      auto space_observer = [workspace_notifications addObserverForName:NSWorkspaceActiveSpaceDidChangeNotification
                                                                object:nil queue:NSOperationQueue.mainQueue usingBlock:invalidate];
      auto session_observer = [workspace_notifications addObserverForName:NSWorkspaceSessionDidResignActiveNotification
                                                                  object:nil queue:NSOperationQueue.mainQueue usingBlock:invalidate];
      auto sleep_observer = [workspace_notifications addObserverForName:NSWorkspaceWillSleepNotification
                                                                object:nil queue:NSOperationQueue.mainQueue usingBlock:invalidate];
      NSTimer *timer = [NSTimer timerWithTimeInterval:0.05 repeats:YES block:^(NSTimer *) {
        if (const auto session = curtain_session.lock()) {
          if (!curtain.isVisible || !has_single_builtin_display(session->display)) {
            session->valid = false;
          }
        }
        if (quitting()) {
          invalidate_curtain();
          [NSApp stop:nil];
          [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                             location:NSZeroPoint modifierFlags:0 timestamp:0
                                         windowNumber:0 context:nil subtype:0 data1:0 data2:0]
                   atStart:NO];
        }
      }];
      [NSRunLoop.mainRunLoop addTimer:timer forMode:NSRunLoopCommonModes];
      if (!quitting()) {
        [NSApp run];
      }
      [timer invalidate];
      if (observing_display_changes) {
        CGDisplayRemoveReconfigurationCallback(display_reconfigured, nullptr);
        observing_display_changes = false;
      }
      [app_notifications removeObserver:display_observer];
      [workspace_notifications removeObserver:space_observer];
      [workspace_notifications removeObserver:session_observer];
      [workspace_notifications removeObserver:sleep_observer];
      // Keep the curtain ordered until workers have stopped. WindowServer removes
      // the window when the host exits, including on forced termination.
    }
  }

  session_ptr begin(std::string_view selected_display, int width, int height, int fps) {
    @autoreleasepool {
      if (width <= 0 || height <= 0 || width % 2 || height % 2 || fps <= 0 ||
          fps > INT32_MAX / 10) {
        return {};
      }

      auto session = std::make_shared<session_t>();
      {
        std::lock_guard lock {session_mutex};
        if (!reserved_session.expired()) {
          return {};
        }
        session->generation = ++next_generation;
        reserved_session = session;
      }
      auto cleanup = util::fail_guard([&]() { end(session); });
      auto result = std::make_shared<std::promise<bool>>();
      auto shown = result->get_future();
      const std::string configured_display {selected_display};
      dispatch_async(dispatch_get_main_queue(), ^{
        result->set_value(show_curtain(session, configured_display));
      });
      if (shown.wait_for(2s) != std::future_status::ready || !shown.get()) {
        return {};
      }

      SCContentFilter *filter = create_filter(session);
      if (!filter && session->valid) {
        std::this_thread::sleep_for(50ms);
        filter = create_filter(session);
      }
      if (!filter || !session->valid) {
        return {};
      }
      session->capture = [[SCCapture alloc] initWithFilter:filter width:width height:height frameRate:fps];
      if (![session->capture start]) {
        return {};
      }
      session->first_frame = [session->capture copyLatestSampleBufferWaiting:NSEC_PER_SEC];
      const auto image = session->first_frame ? CMSampleBufferGetImageBuffer(session->first_frame) : nullptr;
      if (!healthy(session) || !image || CVPixelBufferGetWidth(image) != static_cast<size_t>(width) ||
          CVPixelBufferGetHeight(image) != static_cast<size_t>(height) ||
          CVPixelBufferGetPixelFormatType(image) != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) {
        return {};
      }

      session->armed = true;
      cleanup.disable();
      return session;
    }
  }

  void end(const session_ptr &session) {
    if (!session) {
      return;
    }
    session->valid = false;
    [session->capture stop];
    const auto generation = session->generation;
    dispatch_async(dispatch_get_main_queue(), ^{
      remove_curtain(generation);
    });
  }

  session_ptr current() {
    std::lock_guard lock {session_mutex};
    auto session = reserved_session.lock();
    return session && session->armed ? session : nullptr;
  }

  bool healthy(const session_ptr &session) {
    return session->valid && ![session->capture failed];
  }

  CGDirectDisplayID display_id(const session_ptr &session) {
    return session->display;
  }

  CMSampleBufferRef copy_first_frame(const session_ptr &session) {
    return healthy(session) && session->first_frame ?
      (CMSampleBufferRef) CFRetain(session->first_frame) : nullptr;
  }

  CMSampleBufferRef copy_frame(const session_ptr &session, std::chrono::nanoseconds timeout) {
    return [session->capture copyLatestSampleBufferWaiting:timeout.count()];
  }
}
