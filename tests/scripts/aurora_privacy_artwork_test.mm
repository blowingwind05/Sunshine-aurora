/**
 * @file tests/scripts/aurora_privacy_artwork_test.mm
 * @brief Offline decoder and bitmap drawing fixture for the macOS privacy artwork.
 *
 * From the repository root on macOS:
 * xcrun clang++ -std=c++17 -fobjc-arc -I. tests/scripts/aurora_privacy_artwork_test.mm \
 *   src/platform/macos/aurora_privacy_artwork.mm -framework AppKit -framework CoreGraphics \
 *   -framework ImageIO -o /tmp/aurora-privacy-artwork-test
 * /tmp/aurora-privacy-artwork-test
 */
#include "src/platform/macos/aurora_privacy_artwork.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#import <ImageIO/ImageIO.h>

namespace {
  /** @brief Stop on a failed fixture check and name the failed expectation. */
  void require(bool condition, const char *message) {
    if (!condition) {
      std::fprintf(stderr, "%s\n", message);
      std::exit(1);
    }
  }

  /** @brief Write a three-stripe fixture with a selectable image type and alpha. */
  void write_fixture(NSURL *url, CFStringRef type, bool transparent) {
    const auto space = CGColorSpaceCreateDeviceRGB();
    const auto alpha = transparent ? kCGImageAlphaPremultipliedLast : kCGImageAlphaNoneSkipLast;
    const auto bitmap = CGBitmapContextCreate(nullptr, 12, 4, 8, 0, space, kCGBitmapByteOrder32Big | alpha);
    CGColorSpaceRelease(space);
    require(bitmap != nullptr, "Could not create fixture bitmap");
    CGContextSetRGBFillColor(bitmap, 1, 0, 0, 1);
    CGContextFillRect(bitmap, CGRectMake(0, 0, 3, 4));
    CGContextSetRGBFillColor(bitmap, 0, 1, 0, transparent ? 0.5 : 1.0);
    CGContextFillRect(bitmap, CGRectMake(3, 0, 6, 4));
    CGContextSetRGBFillColor(bitmap, 0, 0, 1, 1);
    CGContextFillRect(bitmap, CGRectMake(9, 0, 3, 4));
    const auto image = CGBitmapContextCreateImage(bitmap);
    CGContextRelease(bitmap);
    require(image != nullptr, "Could not make fixture image");
    const auto destination = CGImageDestinationCreateWithURL((__bridge CFURLRef) url, type, 1, nullptr);
    require(destination != nullptr, "Could not open fixture destination");
    CGImageDestinationAddImage(destination, image, nullptr);
    CGImageRelease(image);
    require(CGImageDestinationFinalize(destination), "Could not encode fixture");
    CFRelease(destination);
  }

  /** @brief Draw a 4x4 view in memory and verify that the center stripe fills it. */
  void check_aspect_fill(CGImageRef image) {
    NSView *view = aurora_privacy::create_artwork_view(NSMakeRect(0, 0, 4, 4), image);
    require(view.isOpaque, "Artwork subview must be opaque");
    std::array<unsigned char, 4 * 4 * 4> pixels {};
    const auto space = CGColorSpaceCreateDeviceRGB();
    const auto bitmap = CGBitmapContextCreate(pixels.data(), 4, 4, 8, 16, space, kCGBitmapByteOrder32Big | kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(space);
    require(bitmap != nullptr, "Could not create offscreen view bitmap");
    [NSGraphicsContext saveGraphicsState];
    NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithCGContext:bitmap flipped:NO];
    [view drawRect:view.bounds];
    [NSGraphicsContext restoreGraphicsState];
    CGContextRelease(bitmap);
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
      require(pixels[i] < 16 && pixels[i + 1] > 239 && pixels[i + 2] < 16 && pixels[i + 3] == 255, "Landscape artwork should crop to the opaque center stripe without bars");
    }
  }
}  // namespace

/** @brief Exercise the artwork code with temporary files and offscreen drawing only. */
int main() {
  @autoreleasepool {
    NSURL *directory = [[NSURL fileURLWithPath:NSTemporaryDirectory() isDirectory:YES]
      URLByAppendingPathComponent:NSUUID.UUID.UUIDString
                      isDirectory:YES];
    NSFileManager *files = NSFileManager.defaultManager;
    require([files createDirectoryAtURL:directory withIntermediateDirectories:NO attributes:nil error:nullptr], "Could not create temporary fixture profile");
    const std::filesystem::path profile {directory.fileSystemRepresentation};
    NSURL *file = [directory URLByAppendingPathComponent:@"privacy-screensaver.png"];
    NSURL *other = [directory URLByAppendingPathComponent:@"other.png"];
    write_fixture(other, CFSTR("public.png"), false);
    require(!aurora_privacy::load_artwork({}), "An empty profile must select the black fallback");
    require(!aurora_privacy::load_artwork("relative"), "A relative profile must select the black fallback");
    require(!aurora_privacy::load_artwork(profile), "Only the fixed artwork name should be read");

    require([@"broken PNG" writeToURL:file atomically:YES encoding:NSUTF8StringEncoding error:nullptr], "Could not write malformed fixture");
    require(!aurora_privacy::load_artwork(profile), "Corrupt artwork must select the black fallback");

    require([files removeItemAtURL:file error:nullptr], "Could not remove malformed fixture");
    write_fixture(file, CFSTR("public.jpeg"), false);
    require(!aurora_privacy::load_artwork(profile), "A JPEG renamed to PNG must select the black fallback");

    require([files removeItemAtURL:file error:nullptr], "Could not remove JPEG fixture");
    write_fixture(file, CFSTR("public.png"), true);
    require(!aurora_privacy::load_artwork(profile), "Transparent PNG must select the black fallback");

    require([files removeItemAtURL:file error:nullptr], "Could not remove transparent fixture");
    write_fixture(file, CFSTR("public.png"), false);
    const auto image = aurora_privacy::load_artwork(profile);
    require(image != nullptr, "The normalized opaque PNG should be accepted");
    check_aspect_fill(image);
    CGImageRelease(image);
    require([files removeItemAtURL:directory error:nullptr], "Could not clean temporary fixtures");
  }
  std::puts("PASS: macOS privacy artwork decode and opaque aspect fill");
}
