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

#include "aurora_privacy_coverage.h"
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
    std::atomic<coverage::clock::time_point> last_coverage {coverage::clock::time_point {}};  ///< Start time of the latest verified WindowServer snapshot.
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

    /** @brief Recheck the identity, opacity, layer and bounds of the existing AppKit window. */
    bool curtain_matches(const session_ptr &session) {
      if (!session->valid || curtain_generation != session->generation || !curtain.isVisible || !curtain.isOpaque ||
          curtain.alphaValue != 1 || curtain.windowNumber != session->window ||
          curtain.level != CGWindowLevelForKey(kCGMaximumWindowLevelKey) || !has_single_builtin_display(session->display)) {
        return false;
      }
      NSArray<NSScreen *> *screens = NSScreen.screens;
      NSScreen *screen = screens.firstObject;
      return screens.count == 1 && [screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue] == session->display &&
             NSEqualRects(curtain.frame, screen.frame);
    }

    /** @brief Verify the current curtain and any intersecting windows in front of it. */
    coverage::result inspect_curtain(const session_ptr &session) {
      if (!curtain_matches(session)) {
        return coverage::result::unconfirmed;
      }

      const auto options = static_cast<CGWindowListOption>(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements);
      NSArray<NSDictionary *> *windows = CFBridgingRelease(CGWindowListCopyWindowInfo(options, kCGNullWindowID));
      std::vector<coverage::window_t> observed;
      for (NSDictionary *info in windows) {
        NSNumber *id_value = info[(__bridge NSString *) kCGWindowNumber];
        NSNumber *pid = info[(__bridge NSString *) kCGWindowOwnerPID];
        NSNumber *layer = info[(__bridge NSString *) kCGWindowLayer];
        NSNumber *alpha = info[(__bridge NSString *) kCGWindowAlpha];
        NSDictionary *rect = info[(__bridge NSString *) kCGWindowBounds];
        CGRect bounds {};
        if (!id_value || !pid || !layer || !alpha || !rect ||
            !CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef) rect, &bounds)) {
          return coverage::result::unconfirmed;
        }
        observed.push_back({
          id_value.unsignedIntValue, pid.longLongValue, layer.longLongValue, alpha.doubleValue,
          {bounds.origin.x, bounds.origin.y, bounds.size.width, bounds.size.height}
        });
        if (observed.back().id == session->window) {
          break;
        }
      }
      const auto bounds = CGDisplayBounds(session->display);
      return coverage::inspect(observed, session->window, getpid(), CGWindowLevelForKey(kCGMaximumWindowLevelKey),
                               {bounds.origin.x, bounds.origin.y, bounds.size.width, bounds.size.height});
    }

    /** @brief Reorder the same window once on a known same-level conflict and refresh the confirmation. */
    bool confirm_curtain(const session_ptr &session) {
      @autoreleasepool {
        const auto started = coverage::clock::now();
        const bool covered = coverage::confirm(
          [&] { return inspect_curtain(session); },
          [&] {
            if (session->valid) {
              BOOST_LOG(debug) << "Aurora privacy reordering the same curtain window " << session->window;
              [curtain orderFrontRegardless];
            }
          }
        );
        if (!covered || !session->valid || !coverage::fresh(started, coverage::clock::now())) {
          return false;
        }
        session->last_coverage = started;
        return true;
      }
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
      window.level = CGWindowLevelForKey(kCGMaximumWindowLevelKey);
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
            window.isOnScreen && window.windowLayer == CGWindowLevelForKey(kCGMaximumWindowLevelKey) &&
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
        if (const auto session = curtain_session.lock(); session && session->valid) {
          const auto last = session->last_coverage.load();
          const auto now = coverage::clock::now();
          if (!curtain_matches(session) ||
              (last != coverage::clock::time_point {} &&
               (!coverage::fresh(last, now) || (now - last >= coverage::snapshot_interval && !confirm_curtain(session))))) {
            BOOST_LOG(warning) << "Aurora privacy could not confirm local curtain coverage";
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

      // The excluded ID must also be the opaque, unobstructed curtain seen by WindowServer.
      auto confirmation = std::make_shared<std::promise<bool>>();
      auto confirmed = confirmation->get_future();
      dispatch_async(dispatch_get_main_queue(), ^{
        confirmation->set_value(confirm_curtain(session));
      });
      if (confirmed.wait_for(1s) != std::future_status::ready || !confirmed.get() || !session->valid) {
        BOOST_LOG(warning) << "Aurora privacy could not verify the excluded curtain before capture";
        return {};
      }
      BOOST_LOG(debug) << "Aurora privacy verified excluded curtain window " << session->window;

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
    if (!session->valid) {
      return false;
    }
    const auto last = session->last_coverage.load();
    if (!coverage::fresh(last, coverage::clock::now())) {
      if (session->valid.exchange(false)) {
        BOOST_LOG(warning) << "Aurora privacy curtain coverage confirmation expired";
      }
      return false;
    }
    return ![session->capture failed] && session->valid;
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
