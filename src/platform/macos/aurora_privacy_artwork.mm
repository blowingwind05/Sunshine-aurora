/**
 * @file src/platform/macos/aurora_privacy_artwork.mm
 * @brief Decode and paint the Aurora privacy curtain artwork.
 */
#include "aurora_privacy_artwork.h"

#include <algorithm>
#import <ImageIO/ImageIO.h>

/** @brief Opaque curtain subview that paints the artwork without letterboxing. */
@interface AuroraPrivacyArtworkView: NSView
@property (nonatomic, strong) NSImage *artwork;  ///< Decoded artwork for this curtain.
@end

namespace {
  /**
   * @brief Center an aspect-preserving rectangle large enough to cover the view.
   * @param bounds View bounds.
   * @param image Artwork dimensions.
   * @return Destination rectangle before clipping.
   */
  NSRect artwork_destination(NSRect bounds, NSSize image) {
    const auto scale = std::max(NSWidth(bounds) / image.width, NSHeight(bounds) / image.height);
    const auto width = image.width * scale;
    const auto height = image.height * scale;
    return NSMakeRect(NSMidX(bounds) - width / 2, NSMidY(bounds) - height / 2, width, height);
  }
}  // namespace

@implementation AuroraPrivacyArtworkView

/** @brief Declare that every pixel in the view is drawn over solid black. */
- (BOOL)isOpaque {
  return YES;
}

/** @brief Paint the dirty region, clipping the centered artwork to the view bounds. */
- (void)drawRect:(NSRect)dirtyRect {
  [NSColor.blackColor setFill];
  NSRectFill(dirtyRect);
  NSImage *image = self.artwork;
  if (!image) {
    return;
  }
  [NSGraphicsContext saveGraphicsState];
  NSRectClip(self.bounds);
  [image drawInRect:artwork_destination(self.bounds, image.size)];
  [NSGraphicsContext restoreGraphicsState];
}

@end

namespace aurora_privacy {
  CGImageRef load_artwork(const std::filesystem::path &profile) {
    if (!profile.is_absolute()) {
      return nullptr;
    }
    const auto path = profile / "privacy-screensaver.png";
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
      return nullptr;
    }

    NSURL *url = [NSURL fileURLWithFileSystemRepresentation:path.c_str() isDirectory:NO relativeToURL:nil];
    if (!url) {
      return nullptr;
    }
    const auto source = CGImageSourceCreateWithURL((__bridge CFURLRef) url, nullptr);
    if (!source) {
      return nullptr;
    }

    CGImageRef image = nullptr;
    const auto type = CGImageSourceGetType(source);
    if (type && CFEqual(type, CFSTR("public.png")) && CGImageSourceGetCount(source) == 1) {
      NSDictionary *options = @{(__bridge NSString *) kCGImageSourceShouldCacheImmediately: @YES};
      image = CGImageSourceCreateImageAtIndex(source, 0, (__bridge CFDictionaryRef) options);
      if (image) {
        const auto alpha = CGImageGetAlphaInfo(image);
        const bool opaque = alpha == kCGImageAlphaNone || alpha == kCGImageAlphaNoneSkipFirst || alpha == kCGImageAlphaNoneSkipLast;
        if (!opaque || CGImageSourceGetStatusAtIndex(source, 0) != kCGImageStatusComplete) {
          CGImageRelease(image);
          image = nullptr;
        }
      }
    }
    CFRelease(source);
    return image;
  }

  NSView *create_artwork_view(NSRect frame, CGImageRef image) {
    auto view = [[AuroraPrivacyArtworkView alloc] initWithFrame:frame];
    view.artwork = [[NSImage alloc] initWithCGImage:image size:NSMakeSize(CGImageGetWidth(image), CGImageGetHeight(image))];
    view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    return view;
  }
}  // namespace aurora_privacy
