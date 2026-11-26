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
  AVCaptureVideoDataOutput *_bufferedOutput;  ///< Persistent output for exact-display capture.
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
  self.videoOutputs = [[NSMapTable alloc] init];
  self.captureCallbacks = [[NSMapTable alloc] init];
  self.captureSignals = [[NSMapTable alloc] init];

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
  if (_bufferedOutput) {
    [_bufferedOutput setSampleBufferDelegate:nil queue:NULL];
  }
  [self.session stopRunning];
  if (_bufferedQueue) {
    dispatch_sync(_bufferedQueue, ^{});
    dispatch_release(_bufferedQueue);
  }
  [_bufferedOutput release];
  [_bufferedSink release];
  [self.videoOutputs release];
  [self.captureCallbacks release];
  [self.captureSignals release];
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

- (BOOL)startBufferedCapture:(FrameSinkBlock)sink {
  if (_bufferedOutput || !sink) {
    return NO;
  }
  @try {
    _bufferedOutput = [[AVCaptureVideoDataOutput alloc] init];
    _bufferedSink = [[AVBufferedSampleSink alloc] initWithSink:sink];
    _bufferedQueue = dispatch_queue_create("aurora.exact-display.capture", DISPATCH_QUEUE_SERIAL);
    _bufferedOutput.alwaysDiscardsLateVideoFrames = YES;
    [_bufferedOutput setVideoSettings:[self outputSettings]];
    [_bufferedOutput setSampleBufferDelegate:_bufferedSink queue:_bufferedQueue];
    [self.session stopRunning];
    if (![self.session canAddOutput:_bufferedOutput]) {
      return NO;
    }
    [self.session addOutput:_bufferedOutput];
    [self.session startRunning];
    return self.session.isRunning;
  } @catch (NSException *) {
    return NO;
  }
}

- (dispatch_semaphore_t)capture:(FrameCallbackBlock)frameCallback {
  @synchronized(self) {
    AVCaptureVideoDataOutput *videoOutput = [[AVCaptureVideoDataOutput alloc] init];
    [videoOutput setVideoSettings:[self outputSettings]];

    dispatch_queue_attr_t qos = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, DISPATCH_QUEUE_PRIORITY_HIGH);
    dispatch_queue_t recordingQueue = dispatch_queue_create("videoCaptureQueue", qos);
    [videoOutput setSampleBufferDelegate:self queue:recordingQueue];

    [self.session stopRunning];

    if ([self.session canAddOutput:videoOutput]) {
      [self.session addOutput:videoOutput];
    } else {
      [videoOutput release];
      return nil;
    }

    AVCaptureConnection *videoConnection = [videoOutput connectionWithMediaType:AVMediaTypeVideo];
    dispatch_semaphore_t signal = dispatch_semaphore_create(0);

    [self.videoOutputs setObject:videoOutput forKey:videoConnection];
    [self.captureCallbacks setObject:frameCallback forKey:videoConnection];
    [self.captureSignals setObject:signal forKey:videoConnection];

    [self.session startRunning];

    return signal;
  }
}

- (void)captureOutput:(AVCaptureOutput *)captureOutput
  didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
         fromConnection:(AVCaptureConnection *)connection {
  FrameCallbackBlock callback = [self.captureCallbacks objectForKey:connection];

  if (callback != nil) {
    if (!callback(sampleBuffer)) {
      @synchronized(self) {
        [self.session stopRunning];
        [self.captureCallbacks removeObjectForKey:connection];
        [self.session removeOutput:[self.videoOutputs objectForKey:connection]];
        [self.videoOutputs removeObjectForKey:connection];
        dispatch_semaphore_signal([self.captureSignals objectForKey:connection]);
        [self.captureSignals removeObjectForKey:connection];
        [self.session startRunning];
      }
    }
  }
}

@end
