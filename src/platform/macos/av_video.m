/**
 * @file src/platform/macos/av_video.m
 * @brief Definitions for video capture on macOS.
 */
// local includes
#import "av_video.h"

/** @brief Exclude the cursor from this screen input and verify the resulting setting. */
static BOOL excludeScreenCursor(AVCaptureScreenInput *input) {
  if (![input respondsToSelector:@selector(setCapturesCursor:)] ||
      ![input respondsToSelector:@selector(capturesCursor)]) {
    return NO;
  }
  [input setCapturesCursor:NO];
  return ![input capturesCursor];
}

/** @brief AVFoundation delegate for persistent, non-blocking frame delivery. */
@interface AVBufferedSampleSink: NSObject <AVCaptureVideoDataOutputSampleBufferDelegate> {
  FrameSinkBlock _sink;  ///< Copied callback, released after the output queue drains.
}
- (instancetype)initWithSink:(FrameSinkBlock)sink;
@end

@implementation AVBufferedSampleSink

- (instancetype)initWithSink:(FrameSinkBlock)sink {
  if ((self = [super init])) {
    _sink = [sink copy];
  }
  return self;
}

- (void)dealloc {
  [_sink release];
  [super dealloc];
}

- (void)captureOutput:(AVCaptureOutput *)output didOutputSampleBuffer:(CMSampleBufferRef)sample
        fromConnection:(AVCaptureConnection *)connection {
  _sink(sample);
}

@end

@implementation AVVideo {
  AVCaptureVideoDataOutput *_bufferedOutput;  ///< Persistent output for live display capture.
  AVBufferedSampleSink *_bufferedSink;  ///< Retained delegate for the persistent output.
  dispatch_queue_t _bufferedQueue;  ///< Serial delegate queue, drained before releasing the sink.
}

+ (BOOL)canExcludeCursorForDisplay:(CGDirectDisplayID)displayID {
  AVCaptureScreenInput *input = [[AVCaptureScreenInput alloc] initWithDisplayID:displayID];
  BOOL supported = excludeScreenCursor(input);
  [input release];
  return supported;
}

- (id)initWithDisplay:(CGDirectDisplayID)displayID frameRate:(int)frameRate excludeCursor:(BOOL)excludeCursor {
  self = [super init];

  CGDisplayModeRef mode = CGDisplayCopyDisplayMode(displayID);
  if (!mode) {
    [self release];
    return nil;
  }

  self.displayID = displayID;
  self.pixelFormat = kCVPixelFormatType_32BGRA;
  self.frameWidth = (int) CGDisplayModeGetPixelWidth(mode);
  self.frameHeight = (int) CGDisplayModeGetPixelHeight(mode);
  self.minFrameDuration = CMTimeMake(1, frameRate);
  self.session = [[AVCaptureSession alloc] init];

  CFRelease(mode);

  AVCaptureScreenInput *screenInput = [[AVCaptureScreenInput alloc] initWithDisplayID:self.displayID];
  if (!screenInput || (excludeCursor && !excludeScreenCursor(screenInput))) {
    [screenInput release];
    [self release];
    return nil;
  }
  [screenInput setMinFrameDuration:self.minFrameDuration];

  if ([self.session canAddInput:screenInput]) {
    [self.session addInput:screenInput];
  } else {
    [screenInput release];
    [self release];
    return nil;
  }
  [screenInput release];

  [self.session startRunning];

  return self;
}

- (void)dealloc {
  [self stopBufferedCapture];
  [self.session stopRunning];
  [self.session release];
  [super dealloc];
}

- (void)setFrameWidth:(int)frameWidth frameHeight:(int)frameHeight {
  self.frameWidth = frameWidth;
  self.frameHeight = frameHeight;
}

/** @brief Build an uncompressed AVFoundation output with the selected size and pixel format. */
- (NSDictionary *)outputSettings {
  return @{
      (NSString *) kCVPixelBufferPixelFormatTypeKey: [NSNumber numberWithUnsignedInt:self.pixelFormat],
      (NSString *) kCVPixelBufferWidthKey: [NSNumber numberWithInt:self.frameWidth],
      (NSString *) kCVPixelBufferHeightKey: [NSNumber numberWithInt:self.frameHeight],
      (NSString *) AVVideoScalingModeKey: AVVideoScalingModeResizeAspect,
  };
}

/** @brief Attach and start one output without changing the selected display input. */
- (BOOL)attachOutput:(AVCaptureVideoDataOutput *)output sink:(AVBufferedSampleSink *)sink queue:(dispatch_queue_t)queue {
  [output setVideoSettings:[self outputSettings]];
  [output setSampleBufferDelegate:sink queue:queue];
  @synchronized(self) {
    [self.session stopRunning];
    const BOOL attached = [self.session canAddOutput:output];
    if (attached) {
      [self.session addOutput:output];
    }
    if (self.session.outputs.count) {
      [self.session startRunning];
    }
    return attached && self.session.isRunning;
  }
}

/** @brief Stop delivery, preserve other outputs, and drain before releasing the sink. */
- (void)detachOutput:(AVCaptureVideoDataOutput *)output queue:(dispatch_queue_t)queue {
  if (output) {
    [output setSampleBufferDelegate:nil queue:NULL];
    @synchronized(self) {
      if ([self.session.outputs containsObject:output]) {
        [self.session stopRunning];
        [self.session removeOutput:output];
        if (self.session.outputs.count) {
          [self.session startRunning];
        }
      }
    }
  }
  if (queue) {
    dispatch_sync(queue, ^{});
  }
}

- (BOOL)startBufferedCapture:(FrameSinkBlock)sink {
  if (_bufferedOutput || !sink) {
    return NO;
  }
  BOOL running = NO;
  @try {
    _bufferedOutput = [[AVCaptureVideoDataOutput alloc] init];
    _bufferedSink = [[AVBufferedSampleSink alloc] initWithSink:sink];
    _bufferedQueue = dispatch_queue_create("sunshine.video.capture", DISPATCH_QUEUE_SERIAL);
    _bufferedOutput.alwaysDiscardsLateVideoFrames = YES;
    running = [self attachOutput:_bufferedOutput sink:_bufferedSink queue:_bufferedQueue];
  } @catch (NSException *) {
    running = NO;
  }
  if (!running) {
    [self stopBufferedCapture];
  }
  return running;
}

- (void)stopBufferedCapture {
  [self detachOutput:_bufferedOutput queue:_bufferedQueue];
  if (_bufferedQueue) {
    dispatch_release(_bufferedQueue);
    _bufferedQueue = NULL;
  }
  [_bufferedOutput release];
  _bufferedOutput = nil;
  [_bufferedSink release];
  _bufferedSink = nil;
}

- (BOOL)probeCapture:(FrameCallbackBlock)frameCallback {
  if (!frameCallback) {
    return NO;
  }
  AVCaptureVideoDataOutput *output = [[AVCaptureVideoDataOutput alloc] init];
  dispatch_semaphore_t done = dispatch_semaphore_create(0);
  dispatch_queue_attr_t qos = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, DISPATCH_QUEUE_PRIORITY_HIGH);
  dispatch_queue_t queue = dispatch_queue_create("sunshine.video.probe", qos);
  __block BOOL completed = NO;
  AVBufferedSampleSink *sink = [[AVBufferedSampleSink alloc] initWithSink:^(CMSampleBufferRef sample) {
    if (!completed && !frameCallback(sample)) {
      completed = YES;
      dispatch_semaphore_signal(done);
    }
  }];
  BOOL running = NO;
  @try {
    running = [self attachOutput:output sink:sink queue:queue];
    if (running) {
      dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    }
  } @catch (NSException *) {
    running = NO;
  }
  [self detachOutput:output queue:queue];
  [sink release];
  [output release];
  dispatch_release(queue);
  dispatch_release(done);
  return running;
}

@end
