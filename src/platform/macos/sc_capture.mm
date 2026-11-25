/**
 * @file src/platform/macos/sc_capture.mm
 * @brief SCStream setup, latest-wins buffer and stop delegate adapted from
 *        Sunshine PR #5511 (d48326113000210a8e301261167a60f76583118d, GPL-3.0).
 */
#import "sc_capture.h"

#include "src/logging.h"

static BOOL isUsableImageSampleBuffer(CMSampleBufferRef sampleBuffer) {
  return sampleBuffer && CMSampleBufferIsValid(sampleBuffer) && CMSampleBufferGetImageBuffer(sampleBuffer);
}

@implementation SCCapture {
  SCStream *_stream;
  dispatch_semaphore_t _frameSignal;
  CMSampleBufferRef _latestSampleBuffer;
  BOOL _stopping;
  BOOL _streamFailed;
}

- (instancetype)initWithFilter:(SCContentFilter *)filter
                         width:(int)width
                        height:(int)height
                     frameRate:(int)frameRate {
  self = [super init];
  if (self) {
    SCStreamConfiguration *config = [[SCStreamConfiguration alloc] init];
    config.width = width;
    config.height = height;
    config.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    config.showsCursor = YES;
    config.captureResolution = SCCaptureResolutionBest;
    config.preservesAspectRatio = YES;
    // Upstream #5511 avoids phase locking with WindowServer and gives the
    // latest-wins slot and VideoToolbox enough surfaces to make progress.
    config.minimumFrameInterval = CMTimeMake(9, frameRate * 10);
    config.queueDepth = 8;
    _frameSignal = dispatch_semaphore_create(0);
    _stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:self];
  }
  return self;
}

- (BOOL)start {
  SCStream *stream = _stream;
  if (!stream || ![stream addStreamOutput:self type:SCStreamOutputTypeScreen sampleHandlerQueue:nil error:nullptr]) {
    return NO;
  }

  dispatch_semaphore_t started = dispatch_semaphore_create(0);
  __block BOOL succeeded = NO;
  [stream startCaptureWithCompletionHandler:^(NSError *error) {
    BOOL stopped;
    @synchronized(self) {
      stopped = self->_stopping;
      succeeded = !error && !stopped;
    }
    if (!error && stopped) {
      [stream stopCaptureWithCompletionHandler:^(NSError *) {}];
    }
    dispatch_semaphore_signal(started);
  }];
  if (dispatch_semaphore_wait(started, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC)) != 0) {
    [self stop];
    return NO;
  }
  return succeeded;
}

- (void)clearLatestSampleBuffer {
  if (_latestSampleBuffer) {
    CFRelease(_latestSampleBuffer);
    _latestSampleBuffer = nullptr;
  }
}

- (void)storeSampleBuffer:(CMSampleBufferRef)sampleBuffer {
  @synchronized(self) {
    if (_stopping || _streamFailed) {
      return;
    }

    const BOOL shouldSignal = _latestSampleBuffer == nullptr;
    [self clearLatestSampleBuffer];
    _latestSampleBuffer = (CMSampleBufferRef) CFRetain(sampleBuffer);

    if (shouldSignal) {
      dispatch_semaphore_signal(_frameSignal);
    }
  }
}

- (CMSampleBufferRef)copyLatestSampleBufferWaiting:(int64_t)timeout_ns {
  dispatch_semaphore_wait(_frameSignal, dispatch_time(DISPATCH_TIME_NOW, timeout_ns));
  @synchronized(self) {
    CMSampleBufferRef sampleBuffer = _latestSampleBuffer;
    _latestSampleBuffer = nullptr;
    return sampleBuffer;
  }
}

- (BOOL)failed {
  @synchronized(self) {
    return _streamFailed || _stopping;
  }
}

- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer ofType:(SCStreamOutputType)type {
  if (type == SCStreamOutputTypeScreen && isUsableImageSampleBuffer(sampleBuffer)) {
    [self storeSampleBuffer:sampleBuffer];
  }
}

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)stop_error {
  @synchronized(self) {
    if (!_stopping) {
      BOOST_LOG(error) << "Aurora ScreenCaptureKit stopped";
      _streamFailed = YES;
      [self clearLatestSampleBuffer];
      dispatch_semaphore_signal(_frameSignal);
    }
  }
}

- (void)stop {
  SCStream *stream;
  @synchronized(self) {
    _stopping = YES;
    stream = _stream;
    _stream = nil;
    [self clearLatestSampleBuffer];
    dispatch_semaphore_signal(_frameSignal);
  }
  if (stream) {
    [stream removeStreamOutput:self type:SCStreamOutputTypeScreen error:nullptr];
    [stream stopCaptureWithCompletionHandler:^(NSError *) {}];
  }
}

- (void)dealloc {
  [self clearLatestSampleBuffer];
}
@end
