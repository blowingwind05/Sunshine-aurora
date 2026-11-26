/**
 * @file src/platform/macos/av_video.h
 * @brief Declarations for video capture on macOS.
 */
#pragma once

// platform includes
#import <AVFoundation/AVFoundation.h>
#import <CoreGraphics/CoreGraphics.h>

/**
 * @brief AVFoundation video capture controller used by the macOS backend.
 */
@interface AVVideo: NSObject

/**
 * @brief Display ID property.
 */
@property (nonatomic, assign) CGDirectDisplayID displayID;
/**
 * @brief Min frame duration property.
 */
@property (nonatomic, assign) CMTime minFrameDuration;
/**
 * @brief Pixel format property.
 */
@property (nonatomic, assign) OSType pixelFormat;
/**
 * @brief Frame width property.
 */
@property (nonatomic, assign) int frameWidth;
/**
 * @brief Frame height property.
 */
@property (nonatomic, assign) int frameHeight;

/**
 * @brief Objective-C block invoked for each captured sample buffer.
 */
typedef bool (^FrameCallbackBlock)(CMSampleBufferRef);

/** @brief Listener for a persistent capture output owned by AVVideo. */
typedef void (^FrameSinkBlock)(CMSampleBufferRef);

/**
 * @brief Capture session that owns the active AVFoundation inputs and outputs.
 */
@property (nonatomic, assign) AVCaptureSession *session;

/**
 * @brief Initialize AVFoundation capture for a display and frame rate.
 *
 * @param displayID Display ID.
 * @param frameRate Frame rate.
 * @param excludeCursor Whether to exclude the cursor from captured video.
 * @return Initialized AVVideo instance, or nil on failure.
 */
- (id)initWithDisplay:(CGDirectDisplayID)displayID frameRate:(int)frameRate excludeCursor:(BOOL)excludeCursor;

/** @brief Verify cursor exclusion on a screen input without starting capture. */
+ (BOOL)canExcludeCursorForDisplay:(CGDirectDisplayID)displayID;

/**
 * @brief Set frame width frame height.
 *
 * @param frameWidth Frame width.
 * @param frameHeight Frame height.
 */
- (void)setFrameWidth:(int)frameWidth frameHeight:(int)frameHeight;
/**
 * @brief Probe capture until the callback accepts a sample and returns false.
 * @param frameCallback Receives samples for the encoder probe.
 * @return Whether the probe completed, with its delegate drained before returning.
 */
- (BOOL)probeCapture:(FrameCallbackBlock)frameCallback;

/** @brief Attach a persistent sample output. The sink must not call back into AVVideo. */
- (BOOL)startBufferedCapture:(FrameSinkBlock)sink;

/** @brief Detach and drain the buffered delegate on the capture owner thread. Safe to repeat. */
- (void)stopBufferedCapture;

@end
