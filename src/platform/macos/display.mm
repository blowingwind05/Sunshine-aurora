/**
 * @file src/platform/macos/display.mm
 * @brief Definitions for display capture on macOS.
 */

// standard includes
#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

// local includes
#include "src/config.h"
#include "src/display_device.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/platform/macos/av_img_t.h"
#include "src/platform/macos/av_video.h"
#include "src/platform/macos/misc.h"
#include "src/platform/macos/nv12_zero_device.h"
#ifdef AURORA_SUNSHINE_HOST
  #include "src/platform/macos/aurora_capture_geometry.h"
  #include "src/platform/macos/aurora_capture_target.h"
  #include "src/platform/macos/aurora_virtual_monitor.h"
#endif
#ifdef AURORA_MACOS_PRIVACY_PREVIEW
  #include "src/platform/macos/aurora_privacy.h"
#endif

// Avoid conflict between AVFoundation and libavutil both defining AVMediaType
/**
 * @def AVMediaType
 * @brief Macro for AV media type.
 */
#define AVMediaType AVMediaType_FFmpeg
#include "src/video.h"
#undef AVMediaType

namespace platf {
  using namespace std::literals;

  namespace {
    std::optional<CGDirectDisplayID> parse_display_id(std::string_view display_name) {
      if (display_name.empty()) {
        return std::nullopt;
      }

      CGDirectDisplayID display_id {};
      const auto *const begin {display_name.data()};
      const auto *const end {display_name.data() + display_name.size()};
      const auto [ptr, ec] {std::from_chars(begin, end, display_id)};
      if (ec != std::errc {} || ptr != end) {
        return std::nullopt;
      }

      return display_id;
    }

    OSType videotoolbox_pixel_format(const video::config_t &config) {
      const auto colorspace {video::colorspace_from_client_config(config, false)};
      return colorspace.bit_depth == 10 ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    }
  }  // namespace

#ifdef AURORA_SUNSHINE_HOST
  namespace {
    constexpr auto exact_capture_name = "aurora:exact-display"sv;  ///< Capture entry distinct from the encoder probe name.

    /** @brief One immutable target revision plus its guarded capture readiness. */
    struct exact_target_t {
      CGDirectDisplayID id;
      mac_capture::geometry_t geometry;
      bool ready = false;
      bool failed = false;
      bool input_enabled = false;
    };

    std::mutex exact_mutex;  ///< Protects the exclusive selection and readiness.
    const aurora_capture_target_t *exact_owner = nullptr;  ///< Session guard that owns the pin.
    std::shared_ptr<exact_target_t> exact_target;  ///< Never cleared implicitly on capture failure.

    /** @brief Read the actual active display geometry without a primary-display fallback. */
    std::optional<mac_capture::geometry_t> exact_geometry(CGDirectDisplayID id) {
      if (!id || !CGDisplayIsOnline(id) || !CGDisplayIsActive(id) ||
          CGDisplayIsAsleep(id) || CGDisplayIsInMirrorSet(id) || CGDisplayIsBuiltin(id)) {
        return std::nullopt;
      }
      const auto mode = CGDisplayCopyDisplayMode(id);
      if (!mode) {
        return std::nullopt;
      }
      const auto width = CGDisplayModeGetPixelWidth(mode);
      const auto height = CGDisplayModeGetPixelHeight(mode);
      CGDisplayModeRelease(mode);
      const auto bounds = CGDisplayBounds(id);
      return mac_capture::make_geometry(bounds.origin.x, bounds.origin.y, bounds.size.width, bounds.size.height, width, height);
    }

    /** @brief Verify the requested ID against the Host-owned virtual monitor registry. */
    std::shared_ptr<exact_target_t> prepare_exact_target(CGDirectDisplayID id) {
      if (!id || config::aurora_privacy_screensaver || !is_screen_capture_allowed()) {
        return {};
      }
      const auto owned = display_device::aurora_virtual_monitor_t::enumerate_owned();
      if (!owned || std::none_of(owned->begin(), owned->end(), [id](const auto &display) { return display.id == id; })) {
        return {};
      }
      const auto geometry = exact_geometry(id);
      return geometry ? std::make_shared<exact_target_t>(exact_target_t {id, *geometry}) : nullptr;
    }

    /** @brief Read the current pinned revision for enumeration or capture open. */
    std::shared_ptr<exact_target_t> current_exact_target() {
      std::lock_guard lock {exact_mutex};
      return exact_target;
    }

    /** @brief Require the same active display, logical bounds and source mode. */
    bool target_geometry_matches(const exact_target_t &target) {
      const auto current = exact_geometry(target.id);
      return current && mac_capture::same_geometry(*current, target.geometry);
    }

    /** @brief Mark the old revision unavailable without touching a newer selection. */
    void mark_exact_failed(const std::shared_ptr<exact_target_t> &target) {
      std::lock_guard lock {exact_mutex};
      target->failed = true;
      target->ready = false;
      target->input_enabled = false;
    }
  }

  std::unique_ptr<aurora_capture_target_t> aurora_select_capture_target(CGDirectDisplayID id) {
    auto requested = prepare_exact_target(id);
    if (!requested) {
      return {};
    }
    std::unique_ptr<aurora_capture_target_t> guard {new aurora_capture_target_t};
    std::lock_guard lock {exact_mutex};
    if (exact_owner) {
      return {};
    }
    exact_owner = guard.get();
    exact_target = std::move(requested);
    return guard;
  }

  bool aurora_capture_target_t::switch_to(CGDirectDisplayID id) {
    auto requested = prepare_exact_target(id);
    if (!requested) {
      return false;
    }
    std::lock_guard lock {exact_mutex};
    if (exact_owner != this) {
      return false;
    }
    exact_target = std::move(requested);
    return true;
  }

  bool aurora_capture_target_t::enable_absolute_input(CGDirectDisplayID id) {
    std::lock_guard lock {exact_mutex};
    if (exact_owner != this || exact_target->id != id || !exact_target->ready ||
        exact_target->failed || !target_geometry_matches(*exact_target) || !CGPreflightPostEventAccess()) {
      return false;
    }
    exact_target->input_enabled = true;
    return true;
  }

  void aurora_capture_target_t::disable_absolute_input() {
    std::lock_guard lock {exact_mutex};
    if (exact_owner == this) {
      exact_target->input_enabled = false;
    }
  }

  aurora_capture_target_t::~aurora_capture_target_t() {
    std::lock_guard lock {exact_mutex};
    if (exact_owner == this) {
      exact_target.reset();
      exact_owner = nullptr;
    }
  }

  bool aurora_capture_target_active() {
    return static_cast<bool>(current_exact_target());
  }

  CGDirectDisplayID aurora_capture_target_ready() {
    std::lock_guard lock {exact_mutex};
    return exact_target && exact_target->ready && !exact_target->failed && target_geometry_matches(*exact_target) ?
             exact_target->id : kCGNullDirectDisplay;
  }

  bool aurora_capture_absolute_mouse(const touch_port_t &port, float x, float y) {
    std::lock_guard lock {exact_mutex};
    if (!exact_target) {
      return false;
    }
    if (!exact_target->input_enabled || !exact_target->ready || exact_target->failed || !target_geometry_matches(*exact_target)) {
      return true;
    }
    const auto point = mac_capture::absolute_point(exact_target->geometry, port.offset_x, port.offset_y,
                                                   port.logical_width, port.logical_height, x, y);
    if (point && CGPreflightPostEventAccess()) {
      const auto location = CGPointMake(point->x, point->y);
      if (const auto event = CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved, location, kCGMouseButtonLeft)) {
        const auto local = CGPointMake(point->x - exact_target->geometry.x, point->y - exact_target->geometry.y);
        if (CGDisplayMoveCursorToPoint(exact_target->id, local) == kCGErrorSuccess) {
          CGEventPost(kCGHIDEventTap, event);
        }
        CFRelease(event);
      }
    }
    return true;
  }
#endif

  /**
   * @brief macOS display capture source and image buffers.
   */
  struct av_display_t: public display_t {
    AVVideo *av_capture {};  ///< AV capture.
    CGDirectDisplayID display_id {};  ///< Display ID.
    std::unique_ptr<display_device::DisplayPowerGuardInterface> display_power_guard;  ///< Display power guard.

    ~av_display_t() override {
      [av_capture release];
    }

    capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
      auto signal = [av_capture capture:^(CMSampleBufferRef sampleBuffer) {
#ifdef AURORA_SUNSHINE_HOST
        if (!sampleBuffer || !CMSampleBufferIsValid(sampleBuffer) || !CMSampleBufferDataIsReady(sampleBuffer) ||
            !CMSampleBufferGetImageBuffer(sampleBuffer)) {
          return true;
        }
#endif
        auto new_sample_buffer = std::make_shared<av_sample_buf_t>(sampleBuffer);
        auto new_pixel_buffer = std::make_shared<av_pixel_buf_t>(new_sample_buffer->buf);

        std::shared_ptr<img_t> img_out;
        if (!pull_free_image_cb(img_out)) {
          // got interrupt signal
          // returning false here stops capture backend
          return false;
        }
        auto av_img = std::static_pointer_cast<av_img_t>(img_out);
#ifdef AURORA_SUNSHINE_HOST
        av_img->source_display_id = display_id;
        av_img->ordinary_display_source = true;
#endif

        auto old_data_retainer = std::make_shared<temp_retain_av_img_t>(
          av_img->sample_buffer,
          av_img->pixel_buffer,
          img_out->data
        );

        av_img->sample_buffer = new_sample_buffer;
        av_img->pixel_buffer = new_pixel_buffer;
        img_out->data = new_pixel_buffer->data();

        img_out->width = (int) CVPixelBufferGetWidth(new_pixel_buffer->buf);
        img_out->height = (int) CVPixelBufferGetHeight(new_pixel_buffer->buf);
        img_out->row_pitch = (int) CVPixelBufferGetBytesPerRow(new_pixel_buffer->buf);
        img_out->pixel_pitch = img_out->row_pitch / img_out->width;

        old_data_retainer = nullptr;

        if (!push_captured_image_cb(std::move(img_out), true)) {
          // got interrupt signal
          // returning false here stops capture backend
          return false;
        }

        return true;
      }];

      if (!signal) {
        return capture_e::error;
      }
      // FIXME: We should time out if an image isn't returned for a while
      dispatch_semaphore_wait(signal, DISPATCH_TIME_FOREVER);

      return capture_e::ok;
    }

    /**
     * @brief Allocate an image buffer compatible with this display backend.
     *
     * @return Allocated img object, or null when unavailable.
     */
    std::shared_ptr<img_t> alloc_img() override {
      return std::make_shared<av_img_t>();
    }

    /**
     * @brief Create AVCodec encode device.
     *
     * @param pix_fmt Sunshine pixel format to convert or allocate for.
     * @return Constructed AVCodec encode device object.
     */
    std::unique_ptr<avcodec_encode_device_t> make_avcodec_encode_device(pix_fmt_e pix_fmt) override {
      if (pix_fmt == pix_fmt_e::yuv420p) {
        av_capture.pixelFormat = kCVPixelFormatType_32BGRA;

        return std::make_unique<avcodec_encode_device_t>();
      } else if (pix_fmt == pix_fmt_e::nv12 || pix_fmt == pix_fmt_e::p010) {
        auto device = std::make_unique<nv12_zero_device>();

        device->init(static_cast<void *>(av_capture), pix_fmt, setResolution, setPixelFormat);

        return device;
      } else {
        BOOST_LOG(error) << "Unsupported Pixel Format."sv;
        return nullptr;
      }
    }

    /**
     * @brief Populate a fallback image when real capture data is unavailable.
     *
     * @param img Image or frame object to read from or populate.
     * @return Capture status reported to the streaming pipeline.
     */
    int dummy_img(img_t *img) override {
      if (!platf::is_screen_capture_allowed()) {
        // If we don't have the screen capture permission, this function will hang
        // indefinitely without doing anything useful. Exit instead to avoid this.
        // A non-zero return value indicates failure to the calling function.
        return 1;
      }

      auto signal = [av_capture capture:^(CMSampleBufferRef sampleBuffer) {
#ifdef AURORA_SUNSHINE_HOST
        if (!sampleBuffer || !CMSampleBufferIsValid(sampleBuffer) || !CMSampleBufferDataIsReady(sampleBuffer) ||
            !CMSampleBufferGetImageBuffer(sampleBuffer)) {
          return true;
        }
#endif
        auto new_sample_buffer = std::make_shared<av_sample_buf_t>(sampleBuffer);
        auto new_pixel_buffer = std::make_shared<av_pixel_buf_t>(new_sample_buffer->buf);

        auto av_img = (av_img_t *) img;
#ifdef AURORA_SUNSHINE_HOST
        av_img->source_display_id = 0;
        av_img->ordinary_display_source = false;
#endif

        auto old_data_retainer = std::make_shared<temp_retain_av_img_t>(
          av_img->sample_buffer,
          av_img->pixel_buffer,
          img->data
        );

        av_img->sample_buffer = new_sample_buffer;
        av_img->pixel_buffer = new_pixel_buffer;
        img->data = new_pixel_buffer->data();

        img->width = (int) CVPixelBufferGetWidth(new_pixel_buffer->buf);
        img->height = (int) CVPixelBufferGetHeight(new_pixel_buffer->buf);
        img->row_pitch = (int) CVPixelBufferGetBytesPerRow(new_pixel_buffer->buf);
        img->pixel_pitch = img->row_pitch / img->width;

        old_data_retainer = nullptr;

        // returning false here stops capture backend
        return false;
      }];

      if (!signal) {
        return -1;
      }
      dispatch_semaphore_wait(signal, DISPATCH_TIME_FOREVER);

      return 0;
    }

    /**
     * A bridge from the pure C++ code of the hwdevice_t class to the pure Objective C code.
     *
     * display --> an opaque pointer to an object of this class
     * width --> the intended capture width
     * height --> the intended capture height
     * @param display Display object or identifier associated with the operation.
     * @param width Frame or display width in pixels.
     * @param height Frame or display height in pixels.
     */
    static void setResolution(void *display, int width, int height) {
      [static_cast<AVVideo *>(display) setFrameWidth:width frameHeight:height];
    }

    /**
     * @brief Set pixel format.
     *
     * @param display Display object or identifier associated with the operation.
     * @param pixelFormat Pixel format.
     */
    static void setPixelFormat(void *display, OSType pixelFormat) {
      static_cast<AVVideo *>(display).pixelFormat = pixelFormat;
    }
  };

#ifdef AURORA_SUNSHINE_HOST
  namespace {
    /** @brief Latest sample from an AVFoundation delegate with a bounded consumer wait. */
    struct exact_samples_t {
      std::mutex mutex;
      std::condition_variable changed;
      std::shared_ptr<av_sample_buf_t> latest;

      void put(CMSampleBufferRef sample) {
        if (!sample || !CMSampleBufferIsValid(sample) || !CMSampleBufferDataIsReady(sample) ||
            !CMSampleBufferGetImageBuffer(sample)) {
          return;
        }
        {
          std::lock_guard lock {mutex};
          latest = std::make_shared<av_sample_buf_t>(sample);
        }
        changed.notify_one();
      }

      std::shared_ptr<av_sample_buf_t> take() {
        std::unique_lock lock {mutex};
        changed.wait_for(lock, 50ms, [&] { return static_cast<bool>(latest); });
        return std::exchange(latest, nullptr);
      }
    };

    /** @brief VideoToolbox capture bound to one exact Mac display selection. */
    struct exact_av_display_t: av_display_t {
      std::shared_ptr<exact_target_t> target;
      std::shared_ptr<exact_samples_t> samples = std::make_shared<exact_samples_t>();
      std::shared_ptr<av_sample_buf_t> first_frame;
      OSType format {};
      int output_width {}, output_height {};

      explicit exact_av_display_t(std::shared_ptr<exact_target_t> requested):
          target {std::move(requested)} {}

      ~exact_av_display_t() override {
        std::lock_guard lock {exact_mutex};
        target->ready = false;
        target->input_enabled = false;
      }

      /** @brief Report intentional retargeting separately from failure or guard teardown. */
      capture_e status() const {
        std::lock_guard lock {exact_mutex};
        if (exact_target != target) {
          return exact_target ? capture_e::reinit : capture_e::error;
        }
        if (target->failed || !target_geometry_matches(*target)) {
          target->ready = false;
          target->failed = true;
          return capture_e::error;
        }
        return capture_e::ok;
      }

      /** @brief Verify this sample matches the selected AVCapture output. */
      bool valid_frame(const std::shared_ptr<av_sample_buf_t> &sample) const {
        const auto image = sample ? CMSampleBufferGetImageBuffer(sample->buf) : nullptr;
        return image && CVPixelBufferGetWidth(image) == static_cast<size_t>(output_width) &&
               CVPixelBufferGetHeight(image) == static_cast<size_t>(output_height) &&
               CVPixelBufferGetPixelFormatType(image) == format;
      }

      /** @brief Bind AVCapture and wait for a real, matching first frame before publication. */
      bool open(const video::config_t &config) {
        if (status() != capture_e::ok || config.framerate <= 0 ||
            !mac_capture::compatible_output(target->geometry, config.width, config.height)) {
          return false;
        }
        const auto &geometry = target->geometry;
        display_id = target->id;
        offset_x = geometry.x;
        offset_y = geometry.y;
        width = env_width = geometry.pixel_width;
        height = env_height = geometry.pixel_height;
        logical_width = env_logical_width = geometry.width;
        logical_height = env_logical_height = geometry.height;
        output_width = config.width;
        output_height = config.height;
        format = videotoolbox_pixel_format(config);
        display_power_guard = display_device::keep_display_awake("Aurora exact display capture");

        @autoreleasepool {
          @try {
            av_capture = [[AVVideo alloc] initWithDisplay:display_id frameRate:config.framerate excludeCursor:config.aurora_client_cursor];
            if (!av_capture) {
              return false;
            }
            [av_capture setFrameWidth:output_width frameHeight:output_height];
            av_capture.pixelFormat = format;
            const auto inbox = samples;
            if (![av_capture startBufferedCapture:^(CMSampleBufferRef sample) { inbox->put(sample); }]) {
              return false;
            }
          } @catch (NSException *) {
            return false;
          }
        }
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline && status() == capture_e::ok) {
          if (auto sample = samples->take()) {
            if (!valid_frame(sample)) {
              return false;
            }
            std::lock_guard lock {exact_mutex};
            if (exact_target != target || target->failed || !target_geometry_matches(*target)) {
              return false;
            }
            first_frame = std::move(sample);
            target->ready = true;
            return true;
          }
        }
        return false;
      }

      /** @brief Transfer one verified CoreVideo image into Sunshine's encoder buffer. */
      void use_frame(std::shared_ptr<av_sample_buf_t> sample, img_t &image) const {
        auto &out = static_cast<av_img_t &>(image);
        auto pixel = std::make_shared<av_pixel_buf_t>(sample->buf);
        out.pixel_buffer.reset();
        out.sample_buffer = std::move(sample);
        out.pixel_buffer = std::move(pixel);
        out.data = out.pixel_buffer->data();
        out.width = output_width;
        out.height = output_height;
        out.row_pitch = static_cast<int>(CVPixelBufferGetBytesPerRowOfPlane(out.pixel_buffer->buf, 0));
        out.pixel_pitch = format == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange ? 2 : 1;
      }

      int dummy_img(img_t *image) override {
        if (status() != capture_e::ok || !first_frame) {
          return -1;
        }
        use_frame(first_frame, *image);
        static_cast<av_img_t &>(*image).source_display_id = 0;
        static_cast<av_img_t &>(*image).ordinary_display_source = false;
        return 0;
      }

      std::unique_ptr<avcodec_encode_device_t> make_avcodec_encode_device(pix_fmt_e pix_fmt) override {
        const auto required = format == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange ? pix_fmt_e::p010 : pix_fmt_e::nv12;
        if (pix_fmt != required) {
          return nullptr;
        }
        auto device = std::make_unique<nv12_zero_device>();
        device->init(this, pix_fmt, [](void *, int, int) {}, [](void *, int) {});
        return device;
      }

      capture_e capture(const push_captured_image_cb_t &push, const pull_free_image_cb_t &pull, bool *) override {
        auto stop = util::fail_guard([&] { mark_exact_failed(target); });
        auto sample = first_frame;
        auto deadline = std::chrono::steady_clock::now() + 5s;
        while (true) {
          const auto current = status();
          if (current != capture_e::ok) {
            return current;
          }
          if (!sample) {
            sample = samples->take();
            if (!sample) {
              if (std::chrono::steady_clock::now() >= deadline) {
                return capture_e::error;
              }
              continue;
            }
          }
          if (!valid_frame(sample)) {
            return capture_e::error;
          }
          std::shared_ptr<img_t> image;
          if (!pull(image)) {
            return capture_e::ok;
          }
          const auto before_push = status();
          if (before_push != capture_e::ok) {
            return before_push;
          }
          use_frame(std::move(sample), *image);
          static_cast<av_img_t &>(*image).source_display_id = display_id;
          static_cast<av_img_t &>(*image).ordinary_display_source = false;
          deadline = std::chrono::steady_clock::now() + 5s;
          if (!push(std::move(image), true)) {
            // The existing switch mailbox may have asked for a rebuild of this revision.
            stop.disable();
            return capture_e::ok;
          }
        }
      }
    };
  }
#endif

#ifdef AURORA_MACOS_PRIVACY_PREVIEW
  struct privacy_display_t: public display_t {
    aurora_privacy::session_ptr privacy;
    std::unique_ptr<display_device::DisplayPowerGuardInterface> display_power_guard;

    int use_sample(CMSampleBufferRef sample, img_t &out) const {
      auto image = sample ? CMSampleBufferGetImageBuffer(sample) : nullptr;
      if (!image || CVPixelBufferGetWidth(image) != static_cast<size_t>(width) ||
          CVPixelBufferGetHeight(image) != static_cast<size_t>(height) ||
          CVPixelBufferGetPixelFormatType(image) != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) {
        return -1;
      }
      auto &av_image = static_cast<av_img_t &>(out);
      auto pixel_buffer = std::make_shared<av_pixel_buf_t>(sample);
      av_image.pixel_buffer.reset();
      av_image.sample_buffer = std::make_shared<av_sample_buf_t>(sample);
      av_image.pixel_buffer = std::move(pixel_buffer);
      out.width = width;
      out.height = height;
      out.row_pitch = static_cast<int>(CVPixelBufferGetBytesPerRowOfPlane(image, 0));
      out.pixel_pitch = 1;
      out.data = static_cast<uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(image, 0));
      return 0;
    }

    capture_e capture(const push_captured_image_cb_t &push_image, const pull_free_image_cb_t &pull_image, bool *) override {
      while (aurora_privacy::healthy(privacy)) {
        auto sample = aurora_privacy::copy_frame(privacy, 50ms);
        auto release_sample = util::fail_guard([sample]() {
          if (sample) CFRelease(sample);
        });
        if (!aurora_privacy::healthy(privacy)) {
          break;
        }
        std::shared_ptr<img_t> image;
        if (!pull_image(image)) {
          return capture_e::ok;
        }
        if (!aurora_privacy::healthy(privacy) || (sample && use_sample(sample, *image) != 0)) {
          break;
        }
        if (!push_image(std::move(image), sample != nullptr)) {
          return capture_e::ok;
        }
      }
      return capture_e::error;
    }

    std::shared_ptr<img_t> alloc_img() override {
      return std::make_shared<av_img_t>();
    }

    int dummy_img(img_t *image) override {
      auto sample = aurora_privacy::copy_first_frame(privacy);
      if (!sample) return -1;
      auto release_sample = util::fail_guard([sample]() { CFRelease(sample); });
      return use_sample(sample, *image);
    }

    std::unique_ptr<avcodec_encode_device_t> make_avcodec_encode_device(pix_fmt_e format) override {
      if (format != pix_fmt_e::nv12) return nullptr;
      auto device = std::make_unique<nv12_zero_device>();
      // The capture stream is already running at the negotiated dimensions and format.
      device->init(this, format, [](void *, int, int) {}, [](void *, int) {});
      return device;
    }
  };
#endif

#ifdef AURORA_SUNSHINE_HOST
  bool aurora_can_exclude_client_cursor(const std::string &display_name) {
    if (config::aurora_privacy_screensaver) {
      return false;
    }
  #ifdef AURORA_MACOS_PRIVACY_PREVIEW
    if (aurora_privacy::current()) {
      return false;
    }
  #endif
    @autoreleasepool {
      if (const auto pinned = current_exact_target()) {
        return target_geometry_matches(*pinned) && [AVVideo canExcludeCursorForDisplay:pinned->id];
      }
      return [AVVideo canExcludeCursorForDisplay:parse_display_id(display_name).value_or(CGMainDisplayID())];
    }
  }
#endif

  std::shared_ptr<display_t> display(platf::mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    if (hwdevice_type != platf::mem_type_e::system && hwdevice_type != platf::mem_type_e::videotoolbox) {
      BOOST_LOG(error) << "Could not initialize display with the given hw device type."sv;
      return nullptr;
    }

#ifdef AURORA_SUNSHINE_HOST
    if (display_name == exact_capture_name) {
      const auto pinned = current_exact_target();
      if (!pinned) {
        return nullptr;
      }
      if (config::aurora_privacy_screensaver || hwdevice_type != mem_type_e::videotoolbox) {
        mark_exact_failed(pinned);
        return nullptr;
      }
      auto exact = std::make_shared<exact_av_display_t>(pinned);
      if (!exact->open(config)) {
        mark_exact_failed(pinned);
        BOOST_LOG(warning) << "Aurora exact display did not yield a matching capture frame for CGDirectDisplayID " << pinned->id;
        return nullptr;
      }
      return exact;
    }
#endif

#ifdef AURORA_MACOS_PRIVACY_PREVIEW
    if (auto privacy = aurora_privacy::current()) {
      if (config.aurora_client_cursor) {
        BOOST_LOG(warning) << "The negotiated Aurora cursor mode is unavailable in privacy preview capture";
        return nullptr;
      }
      if (hwdevice_type != mem_type_e::videotoolbox || !aurora_privacy::healthy(privacy)) {
        return nullptr;
      }
      const auto display_id = aurora_privacy::display_id(privacy);
      if (!display_name.empty() && display_name != std::to_string(display_id)) {
        return nullptr;
      }
      auto display = std::make_shared<privacy_display_t>();
      display->privacy = std::move(privacy);
      display->display_power_guard = display_device::keep_display_awake("Aurora protected capture");
      display->width = config.width;
      display->height = config.height;
      const auto bounds = CGDisplayBounds(display_id);
      display->env_width = display->logical_width = static_cast<int>(bounds.size.width);
      display->env_height = display->logical_height = static_cast<int>(bounds.size.height);
      display->env_logical_width = display->logical_width;
      display->env_logical_height = display->logical_height;
      return display;
    }
#endif

    auto display = std::make_shared<av_display_t>();

    BOOST_LOG(debug) << "Waking display for capture selector ["sv << display_name << ']';
    if (!display_device::wake_display(display_name, 1s)) {
      BOOST_LOG(debug) << "Display wake attempt did not expose the requested display ["sv << display_name << ']';
    }

    display->display_power_guard = display_device::keep_display_awake("Sunshine display capture");
    if (display->display_power_guard) {
      BOOST_LOG(debug) << "Keeping display awake for capture"sv;
    } else {
      BOOST_LOG(debug) << "Unable to create display sleep prevention assertion"sv;
    }

    // Default to main display
    display->display_id = CGMainDisplayID();

    if (const auto configured_display_id {parse_display_id(display_name)}) {
      display->display_id = *configured_display_id;
    } else if (!display_name.empty()) {
      BOOST_LOG(warning) << "Configured display ["sv << display_name
                         << "] is not a valid macOS capture display id. Falling back to main display ["sv
                         << display->display_id << "]."sv;
    }

    // Print all displays available with their names and ids
    BOOST_LOG(debug) << "Detecting displays"sv;
    for (const auto &device : display_device::enumerate_devices()) {
      if (device.m_display_name.empty()) {
        continue;
      }

      BOOST_LOG(debug) << "Detected display: "sv << device.m_friendly_name
                       << " (id: "sv << device.m_display_name << ") connected: true"sv;
    }

    BOOST_LOG(info) << "Configuring selected display ("sv << display->display_id << ") to stream"sv;

#ifdef AURORA_SUNSHINE_HOST
    if (display_device::aurora_virtual_monitor_t::owns_capture_id(display->display_id)) {
      return nullptr;
    }
#endif
    display->av_capture = [[AVVideo alloc] initWithDisplay:display->display_id frameRate:config.framerate excludeCursor:config.aurora_client_cursor];

    if (!display->av_capture) {
      BOOST_LOG(error) << "Video setup failed."sv;
      return nullptr;
    }

    display->width = display->av_capture.frameWidth;
    display->height = display->av_capture.frameHeight;
    // We also need set env_width and env_height for absolute mouse coordinates
    display->env_width = display->width;
    display->env_height = display->height;

    if (hwdevice_type == platf::mem_type_e::videotoolbox) {
      const auto pixel_format {videotoolbox_pixel_format(config)};
      [display->av_capture setFrameWidth:config.width frameHeight:config.height];
      display->av_capture.pixelFormat = pixel_format;
    }

    return display;
  }

  std::vector<std::string> display_names(mem_type_e hwdevice_type) {
    std::vector<std::string> display_names;
    if (hwdevice_type != platf::mem_type_e::system && hwdevice_type != platf::mem_type_e::videotoolbox) {
      return display_names;
    }

#ifdef AURORA_SUNSHINE_HOST
    if (current_exact_target()) {
      // refresh_displays feeds this entry to display(), distinct from the legacy encoder probe name.
      // Keep the entry if the exact ID disappears so reinit cannot fall back.
      return {std::string {exact_capture_name}};
    }
#endif

    const auto devices {display_device::enumerate_devices()};
    display_names.reserve(devices.size());
    for (const auto &device : devices) {
      if (!device.m_display_name.empty()) {
#ifdef AURORA_SUNSHINE_HOST
        const auto id = parse_display_id(device.m_display_name);
        if (id && display_device::aurora_virtual_monitor_t::owns_capture_id(*id)) {
          continue;
        }
#endif
        display_names.emplace_back(device.m_display_name);
      }
    }

    return display_names;
  }

#ifdef AURORA_SUNSHINE_HOST
  std::optional<std::pair<std::size_t, std::size_t>> aurora_capture_size() {
    {
      std::lock_guard lock {exact_mutex};
      if (exact_target) {
        if (!exact_target->ready || exact_target->failed || !target_geometry_matches(*exact_target)) {
          return std::nullopt;
        }
        return std::pair {static_cast<std::size_t>(exact_target->geometry.pixel_width),
                          static_cast<std::size_t>(exact_target->geometry.pixel_height)};
      }
    }

    // Match video::refresh_displays when starting a new capture. An explicitly
    // configured output may be woken later, so omit geometry while it is unknown.
    const auto outputs = display_names(mem_type_e::videotoolbox);
    if (outputs.empty()) {
      return std::nullopt;
    }

    const auto configured = display_device::map_output_name(config::video.output_name);
    const auto selected = config::video.output_name.empty() ?
      outputs.begin() : std::find(outputs.begin(), outputs.end(), configured);
    if (selected == outputs.end()) {
      return std::nullopt;
    }

    const auto id = parse_display_id(*selected);
    if (!id || !CGDisplayIsActive(*id)) {
      return std::nullopt;
    }

    const auto mode = CGDisplayCopyDisplayMode(*id);
    if (!mode) {
      return std::nullopt;
    }

    const auto width = CGDisplayModeGetPixelWidth(mode);
    const auto height = CGDisplayModeGetPixelHeight(mode);
    CFRelease(mode);
    if (!width || !height) {
      return std::nullopt;
    }
    return std::pair {width, height};
  }
#endif

  /**
   * @brief Report whether encoder backends should be probed again before streaming.
   *
   * @return Always `true` because macOS GPU changes are not tracked by this backend.
   */
  bool needs_encoder_reenumeration() {
    // We don't track GPU state, so we will always reenumerate. Fortunately, it is fast on macOS.
    return true;
  }
}  // namespace platf
