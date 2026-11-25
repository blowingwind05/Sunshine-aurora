/**
 * @file src/platform/macos/sc_capture.h
 * @brief Narrow ScreenCaptureKit backport from Sunshine PR #5511, by martona
 *        and contributors (GPL-3.0). Aurora supplies the verified content filter.
 */
#pragma once

#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

@interface SCCapture: NSObject <SCStreamDelegate, SCStreamOutput>
- (instancetype)initWithFilter:(SCContentFilter *)filter
                         width:(int)width
                        height:(int)height
                     frameRate:(int)frameRate;
- (BOOL)start;
- (void)stop;
- (BOOL)failed;
- (CMSampleBufferRef)copyLatestSampleBufferWaiting:(int64_t)timeout_ns CF_RETURNS_RETAINED;
@end
