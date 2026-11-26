/**
 * @file src/platform/macos/aurora_privacy_artwork.h
 * @brief Optional artwork for the existing Aurora privacy curtain window.
 */
#pragma once

#import <AppKit/AppKit.h>
#include <filesystem>

namespace aurora_privacy {
  /**
   * @brief Decode the locally stored, opaque privacy artwork.
   * @param profile Absolute managed host profile.
   * @return Retained image for the caller to release, or null to keep the black curtain.
   */
  CGImageRef load_artwork(const std::filesystem::path &profile);

  /**
   * @brief Create an opaque, center-cropped artwork subview on the AppKit thread.
   * @param frame Initial subview rectangle in the existing curtain content view.
   * @param image Decoded artwork.
   * @return A view that retains the artwork and fills its bounds.
   */
  NSView *create_artwork_view(NSRect frame, CGImageRef image);
}  // namespace aurora_privacy
