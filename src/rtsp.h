/**
 * @file src/rtsp.h
 * @brief Declarations for RTSP streaming.
 */
#pragma once

// standard includes
#include <atomic>
#include <functional>

// local includes
#include "aurora_client_cursor.h"
#include "crypto.h"
#include "thread_safe.h"
#if defined(__APPLE__) && defined(AURORA_SUNSHINE_HOST)
  #include "aurora_session_displays.h"
#endif
#if defined(_WIN32) && defined(AURORA_SUNSHINE_HOST)
  #include "aurora_video_bitrate.h"
#endif

namespace rtsp_stream {
#if defined(__APPLE__) && defined(AURORA_SUNSHINE_HOST)
  /**
   * @brief Dispatch an HTTPS command to the sole running session owned by the exact TLS certificate.
   * @param cert Verified peer certificate in PEM form.
   * @param request Parsed display command.
   * @param authorize Live certificate authorization check for use under the session registry lock.
   * @return Confirmed status or a rejection without topology from other sessions.
   */
  aurora::session_display::result aurora_session_displays(std::string_view cert,
    const aurora::session_display::command &request, const std::function<bool()> &authorize);
#endif
#if defined(AURORA_SUNSHINE_HOST) && (defined(_WIN32) || (defined(__APPLE__) && defined(AURORA_MACOS_PRIVACY_PREVIEW)))
  /**
   * @brief Run the profile operation only while the matching certificate owns the sole running session.
   * @param cert Exact PEM for the verified TLS peer.
   * @param operation Bounded synchronous operation. Must recheck paired authorization before touching the profile.
   * @return Whether the session authorized entry to the operation.
   */
  bool aurora_with_privacy_appearance_owner(std::string_view cert, const std::function<void()> &operation);
#endif
#if defined(_WIN32) && defined(AURORA_SUNSHINE_HOST)
  enum class aurora_privacy_action { status, on, off, display_off };
  std::string_view aurora_session_privacy(std::string_view cert, aurora_privacy_action action);

  /** @brief Handle a bitrate request for the verified certificate's active session. */
  std::string aurora_session_bitrate(std::string_view cert, const aurora::video_bitrate::request_t &request);
#endif

  constexpr auto RTSP_SETUP_PORT = 21;  ///< GameStream base-port offset used for the RTSP setup listener.

  /**
   * @brief RTSP launch session state shared with stream setup.
   */
  struct launch_session_t {
    uint32_t id;  ///< RTSP launch-session identifier assigned before stream startup.

    crypto::aes_t gcm_key;  ///< AES-GCM key negotiated for encrypted RTSP messages.
    crypto::aes_t iv;  ///< Initial RTSP AES-GCM IV supplied by the client.

    std::string av_ping_payload;  ///< AV ping payload.
    uint32_t control_connect_data;  ///< Client-provided token used when connecting the control channel.

    bool aurora_client_cursor = false;  ///< Exclude the cursor from captured video after successful negotiation.
#if defined(__APPLE__) && defined(AURORA_SUNSHINE_HOST)
    bool aurora_display_switch = false;  ///< Paired client opted into the presentation barrier on this launch or resume.
#endif
    bool host_audio;  ///< Whether host audio should be played locally.
    std::string unique_id;  ///< Moonlight client unique identifier for this launch request.
    int width;  ///< Frame or display width in pixels.
    int height;  ///< Frame or display height in pixels.
    int fps;  ///< Requested video frame rate.
    int gcmap;  ///< Game controller mapping requested by the client.
    int appid;  ///< Application ID requested for launch or resume.
    int surround_info;  ///< Encoded GameStream surround-sound capability flags.
    std::string surround_params;  ///< Client-provided surround-sound layout parameters.
    bool continuous_audio;  ///< Whether audio packets continue during silence.
    bool enable_hdr;  ///< Whether HDR streaming is requested.
    bool enable_sops;  ///< Whether sequence output protection is requested.
    std::string client_name;  ///< Friendly client name from initial pairing.

    std::optional<crypto::cipher::gcm_t> rtsp_cipher;  ///< AES-GCM cipher used once encrypted RTSP is negotiated.
    std::string rtsp_url_scheme;  ///< URL scheme selected by the RTSP SETUP flow.
    uint32_t rtsp_iv_counter;  ///< Counter value mixed into encrypted RTSP IVs.
    std::string client_cert;  ///< PEM certificate for the paired Moonlight client.
  };

  /**
   * @brief Queue a launch session until the RTSP client connects.
   *
   * @param launch_session Session state prepared by the GameStream launch handler.
   * @return The cursor mode granted to this launch, or a pending cursor handshake conflict.
   */
  aurora::client_cursor::launch_result launch_session_raise(std::shared_ptr<launch_session_t> launch_session);

  /** @brief Whether a confirmed cursor launch already owns the pending slot. */
  bool cursor_launch_pending();

  /**
   * @brief Clear state for the specified launch session.
   * @param launch_session_id The ID of the session to clear.
   */
  void launch_session_clear(uint32_t launch_session_id);

  /**
   * @brief Get the number of active sessions.
   * @return Count of active sessions.
   */
  int session_count();

  /**
   * @brief Terminates all running streaming sessions.
   */
  void terminate_sessions();
  /**
   * @brief Terminate active sessions associated with a client certificate.
   *
   * @param cert Certificate data or object used by the operation.
   */
  void terminate_sessions_by_cert(std::string_view cert);

  /**
   * @brief Runs the RTSP server loop.
   */
  void start();
}  // namespace rtsp_stream
