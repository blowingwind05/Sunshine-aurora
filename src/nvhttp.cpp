/**
 * @file src/nvhttp.cpp
 * @brief Definitions for the nvhttp (GameStream) server.
 */
// macros
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <utility>

// lib includes
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/context_base.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <Simple-Web-Server/server_http.hpp>

#if defined(AURORA_SUNSHINE_HOST) && defined(_WIN32)
  #include <windows.h>
#elif defined(AURORA_SUNSHINE_HOST) && defined(__APPLE__)
  #include <CoreServices/CoreServices.h>
#endif

// local includes
#include "config.h"
#if defined(AURORA_SUNSHINE_HOST) && (defined(_WIN32) || defined(__APPLE__))
  #include "aurora_capture_geometry.h"
#endif
#include "display_device.h"
#include "file_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "network.h"
#include "nvhttp.h"
#ifdef AURORA_SUNSHINE_HOST
  #include "aurora_client_proof.h"
#endif
#include "platform/common.h"
#include "process.h"
#include "rtsp.h"
#include "system_tray.h"
#include "utility.h"
#include "uuid.h"
#include "video.h"

using namespace std::literals;

namespace nvhttp {

  static constexpr std::string_view EMPTY_PROPERTY_TREE_ERROR_MSG = "Property tree is empty. Probably, control flow got interrupted by an unexpected C++ exception. This is a bug in Sunshine. Moonlight-qt will report Malformed XML (missing root element)."sv;

  namespace fs = std::filesystem;
  namespace pt = boost::property_tree;

  crypto::cert_chain_t cert_chain;  ///< Enabled paired-client certificates accepted by Sunshine's GameStream HTTPS server.
#ifdef AURORA_SUNSHINE_HOST
  static std::optional<Aurora::ClientProof::CertificateDigest> tailnet_host_fingerprint;  ///< TLS leaf digest under the paired-client mutex.
#endif

  /**
   * @brief Get the mutex protecting paired-client authorization state.
   *
   * @return Mutex serializing paired-client state and certificate authorization changes.
   */
  std::mutex &client_auth_mutex() {
    static std::mutex mutex;
    return mutex;
  }

  /**
   * @brief HTTPS server backend that adds Sunshine's client-certificate verification.
   */
  class SunshineHTTPSServer: public SimpleWeb::ServerBase<SunshineHTTPS> {
  public:
    /**
     * @brief Initialize the HTTPS server with Sunshine's certificate and key files.
     *
     * @param certification_file Path to the server certificate file.
     * @param private_key_file Path to the matching private key file.
     */
    SunshineHTTPSServer(const std::string &certification_file, const std::string &private_key_file):
        ServerBase<SunshineHTTPS>::ServerBase(443),
        context(boost::asio::ssl::context::tls_server) {
      // Disabling TLS 1.0 and 1.1 (see RFC 8996)
      context.set_options(boost::asio::ssl::context::no_tlsv1);
      context.set_options(boost::asio::ssl::context::no_tlsv1_1);
      context.use_certificate_chain_file(certification_file);
      context.use_private_key_file(private_key_file, boost::asio::ssl::context::pem);
    }

#ifdef AURORA_SUNSHINE_HOST
    crypto::x509_t peer_certificate(const Request &request) {
      // Simple-Web-Server does not expose the TLS socket through Request. Match
      // both ends of its live TCP connection under the server's connection lock.
      const auto local = request.local_endpoint();
      const auto remote = request.remote_endpoint();
      if (local.port() == 0 || remote.port() == 0) return {};
      SimpleWeb::LockGuard lock(connections->mutex);
      for (const auto *connection : connections->set) {
        const auto &socket = connection->socket->lowest_layer();
        SimpleWeb::error_code ec;
        if (socket.remote_endpoint(ec) != remote || ec || socket.local_endpoint(ec) != local || ec) continue;
        return crypto::x509_t {
  #if OPENSSL_VERSION_MAJOR >= 3
          SSL_get1_peer_certificate(connection->socket->native_handle())
  #else
          SSL_get_peer_certificate(connection->socket->native_handle())
  #endif
        };
      }
      return {};
    }
#endif

    std::function<int(SSL *)> verify;  ///< Callback that validates a client's TLS certificate after handshake.
    std::function<void(std::shared_ptr<Response>, std::shared_ptr<Request>)> on_verify_failed;  ///< Handler used to return the pairing challenge when client verification fails.

  protected:
    boost::asio::ssl::context context;  ///< TLS server context configured with Sunshine's certificate and protocol policy.

    /**
     * @brief Enable client-certificate verification after the listening socket is bound.
     */
    void after_bind() override {
      if (verify) {
        context.set_verify_mode(boost::asio::ssl::verify_peer | boost::asio::ssl::verify_fail_if_no_peer_cert | boost::asio::ssl::verify_client_once);
        context.set_verify_callback([](int verified, boost::asio::ssl::verify_context &ctx) {
          // To respond with an error message, a connection must be established
          return 1;
        });
      }
    }

    // This is Server<HTTPS>::accept() with SSL validation support added
    /**
     * @brief Accept a pending connection and arm the server for the next client.
     */
    void accept() override {
      auto connection = create_connection(*io_service, context);

      acceptor->async_accept(connection->socket->lowest_layer(), [this, connection](const SimpleWeb::error_code &ec) {
        auto lock = connection->handler_runner->continue_lock();
        if (!lock) {
          return;
        }

        if (ec != SimpleWeb::error::operation_aborted) {
          this->accept();
        }

        auto session = std::make_shared<Session>(config.max_request_streambuf_size, connection);

        if (!ec) {
          boost::asio::ip::tcp::no_delay option(true);
          SimpleWeb::error_code ec;
          session->connection->socket->lowest_layer().set_option(option, ec);

          session->connection->set_timeout(config.timeout_request);
          session->connection->socket->async_handshake(boost::asio::ssl::stream_base::server, [this, session](const SimpleWeb::error_code &ec) {
            session->connection->cancel_timeout();
            auto lock = session->connection->handler_runner->continue_lock();
            if (!lock) {
              return;
            }
            if (!ec) {
              if (verify && !verify(session->connection->socket->native_handle())) {
                this->write(session, on_verify_failed);
              } else {
                this->read(session);
              }
            } else if (this->on_error) {
              this->on_error(session->request, ec);
            }
          });
        } else if (this->on_error) {
          this->on_error(session->request, ec);
        }
      });
    }
  };

  /**
   * @brief HTTPS server type used for GameStream endpoints requiring TLS.
   */
  using https_server_t = SunshineHTTPSServer;
  /**
   * @brief Plain HTTP server type used for GameStream endpoints without TLS.
   */
  using http_server_t = SimpleWeb::Server<SimpleWeb::HTTP>;

  /**
   * @brief Internal HTTPS credential paths for the configuration server.
   */
  struct conf_intern_t {
    std::string servercert;  ///< Server certificate PEM string.
    std::string pkey;  ///< Private key PEM string or path.
  } conf_intern;  ///< TLS credential paths loaded from Sunshine's runtime configuration.

  /**
   * @brief Certificate entry associated with a client name and UUID.
   */
  struct named_cert_t {
    std::string name;  ///< Human-readable name for this item.
    std::string uuid;  ///< Persistent Moonlight client UUID associated with the certificate.
    std::string cert;  ///< Certificate PEM string or path.
    bool enabled = true;  ///< Whether this persisted client entry may connect.
    std::string tailnet_scope;  ///< Stable authorization scope for Aurora-managed credentials.
    bool unpaired = false;  ///< Retained revocation record hidden from the active client list.
    std::chrono::steady_clock::time_point tailnet_until {};  ///< In-memory expiry cleared on restart.

    /** @brief Check whether the stored client is currently allowed to authenticate. */
    bool authorized() const {
      return enabled && !unpaired &&
        (tailnet_scope.empty() || std::chrono::steady_clock::now() < tailnet_until);
    }
  };

  /**
   * @brief Persisted pairing data for one Moonlight client.
   */
  struct client_t {
    std::vector<named_cert_t> named_devices;  ///< Persisted Moonlight clients allowed to pair or reconnect.
  };

  // uniqueID, session
  std::unordered_map<std::string, pair_session_t> map_id_sess;  ///< Pairing sessions keyed by temporary unique ID.

  /**
   * @brief Get the mutex protecting pairing-session state.
   *
   * @return Mutex serializing pairing-session storage and lifecycle transitions.
   */
  std::mutex &map_id_sess_mutex() {
    static std::mutex mutex;
    return mutex;
  }

  client_t client_root;  ///< In-memory representation of the paired-client database.
  std::atomic<uint32_t> session_id_counter;  ///< Monotonic counter used to allocate GameStream session IDs.

  // Set by TLS verify callback, read by launch/resume handler (single-threaded HTTPS server)
  std::string last_verified_client_cert;  ///< Last client certificate accepted by the TLS verify callback.  // NOSONAR(cpp:S5421): intentionally mutable global
  std::string last_verified_client_name;  ///< Friendly name of last client certificate accepted by the TLS verify callback. // NOSONAR(cpp:S5421): intentionally mutable global

  /**
   * @brief Case-insensitive map used for HTTP headers and query parameters.
   */
  using args_t = SimpleWeb::CaseInsensitiveMultimap;
  /**
   * @brief Shared HTTPS response object passed to GameStream handlers.
   */
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Response>;
  /**
   * @brief Shared HTTPS request object received by GameStream handlers.
   */
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Request>;
  /**
   * @brief Shared HTTP response object passed to redirect and discovery handlers.
   */
  using resp_http_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Response>;
  /**
   * @brief Shared HTTP request object received by redirect and discovery handlers.
   */
  using req_http_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Request>;

  /**
   * @brief Certificate operations supported by the pairing API.
   */
  enum class op_e {
    ADD,  ///< Add certificate
    REMOVE  ///< Remove certificate
  };

  /**
   * @brief Read a named query argument from the HTTP request map.
   *
   * @param args Parsed query-string argument map.
   * @param name Query parameter name to read.
   * @param default_value Value returned when the parameter is absent.
   * @return Query parameter value, default value, or an empty string.
   */
  std::string get_arg(const args_t &args, const char *name, const char *default_value = nullptr) {
    auto it = args.find(name);
    if (it == std::end(args)) {
      if (default_value != nullptr) {
        return std::string(default_value);
      }

      throw std::out_of_range(name);
    }
    return it->second;
  }

  /**
   * @brief Persist the current state to its backing store.
   * @return True when the updated state was written.
   */
  bool save_state() {
    pt::ptree root;

    std::error_code status_error;
    const bool has_existing_state = fs::exists(config::nvhttp.file_state, status_error);
    if (status_error) {
      BOOST_LOG(error) << "Couldn't access "sv << config::nvhttp.file_state << ": "sv << status_error.message();
      return false;
    }
    if (has_existing_state) {
      try {
        pt::read_json(config::nvhttp.file_state, root);
      } catch (std::exception &e) {
        BOOST_LOG(error) << "Couldn't read "sv << config::nvhttp.file_state << ": "sv << e.what();
        return false;
      }
    }

    root.erase("root"s);

    root.put("root.uniqueid", http::unique_id);
    pt::ptree named_cert_nodes;
    for (const auto &named_cert : client_root.named_devices) {
      pt::ptree named_cert_node;
      named_cert_node.put("name"s, named_cert.name);
      named_cert_node.put("cert"s, named_cert.cert);
      named_cert_node.put("uuid"s, named_cert.uuid);
      named_cert_node.put("enabled"s, named_cert.enabled);
      if (!named_cert.tailnet_scope.empty()) {
        named_cert_node.put("aurora_tailnet_scope", named_cert.tailnet_scope);
        named_cert_node.put("aurora_unpaired", named_cert.unpaired);
      }
      named_cert_nodes.push_back(std::make_pair(""s, named_cert_node));
    }
    root.add_child("root.named_devices"s, named_cert_nodes);

    fs::path temporary;
    try {
#ifdef AURORA_SUNSHINE_HOST
      const fs::path target {config::nvhttp.file_state};
      temporary = target;
      temporary += "." + uuid_util::uuid_t::generate().string() + ".tmp";
      std::ofstream output(temporary, std::ios::out | std::ios::trunc);
      output.exceptions(std::ios::badbit | std::ios::failbit);
  #ifndef _WIN32
      fs::permissions(temporary, fs::perms::owner_read | fs::perms::owner_write);
  #endif
      pt::write_json(output, root);
      output.close();
  #ifdef _WIN32
      if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
      }
  #else
      fs::rename(temporary, target);
  #endif
#else
      pt::write_json(config::nvhttp.file_state, root);
#endif
    } catch (std::exception &e) {
      BOOST_LOG(error) << "Couldn't write "sv << config::nvhttp.file_state << ": "sv << e.what();
      if (!temporary.empty()) {
        std::error_code error;
        fs::remove(temporary, error);
      }
      return false;
    }
    return true;
  }

  /**
   * @brief Convert a PEM certificate to Sunshine's canonical OpenSSL serialization.
   *
   * @param cert_pem PEM-encoded certificate to canonicalize.
   * @return Canonical PEM text, or an empty string when the certificate is invalid.
   */
  std::string canonical_certificate_pem(const std::string_view cert_pem) {
    auto certificate = crypto::x509(cert_pem);
    return certificate ? crypto::pem(certificate) : std::string {};
  }

  /**
   * @brief Rebuild the GameStream trust stores from enabled paired-client records.
   *
   * @note The caller must hold `client_auth_mutex()`.
   */
  void rebuild_client_cert_chain() {
    cert_chain.clear();
    for (const auto &named_cert : client_root.named_devices) {
      if (!named_cert.enabled || named_cert.unpaired) {
        continue;
      }

      auto certificate = crypto::x509(named_cert.cert);
      if (!certificate) {
        BOOST_LOG(warning) << "Ignoring invalid paired-client certificate"sv;
        continue;
      }

      cert_chain.add(std::move(certificate));
    }
  }

  /**
   * @brief Check whether a certificate exactly matches an enabled paired-client record.
   *
   * @param certificate Parsed client certificate to compare by canonical X.509 identity.
   * @return `true` only when exactly one matching paired-client record is enabled.
   * @note The caller must hold `client_auth_mutex()`.
   */
  bool is_client_enabled(const X509 *certificate) {
    bool matched = false;
    for (const auto &named_cert : client_root.named_devices) {
      if (auto stored_certificate = crypto::x509(named_cert.cert); !stored_certificate || X509_cmp(stored_certificate.get(), certificate) != 0) {
        continue;
      }

      if (matched || !named_cert.authorized()) {
        return false;
      }
      matched = true;
    }
    return matched;
  }

  /**
   * @brief Verify a client certificate against the exact enabled paired identity.
   *
   * @param certificate Parsed client certificate presented during the TLS handshake.
   * @return `nullptr` when authorized, otherwise a non-sensitive error string.
   * @note The caller must hold `client_auth_mutex()`.
   */
  const char *verify_client_certificate(X509 *certificate) {
    if (const auto verification_error = cert_chain.verify(certificate); verification_error) {
      return verification_error;
    }
    return is_client_enabled(certificate) ? nullptr : "Client certificate identity is not enabled";
  }

  /**
   * @brief Load state from its backing store.
   */
  void load_state() {
    if (!fs::exists(config::nvhttp.file_state)) {
      BOOST_LOG(info) << "File "sv << config::nvhttp.file_state << " doesn't exist"sv;
      http::unique_id = uuid_util::uuid_t::generate().string();
      return;
    }

    pt::ptree tree;
    try {
      pt::read_json(config::nvhttp.file_state, tree);
    } catch (std::exception &e) {
      BOOST_LOG(error) << "Couldn't read "sv << config::nvhttp.file_state << ": "sv << e.what();

      return;
    }

    auto unique_id_p = tree.get_optional<std::string>("root.uniqueid");
    if (!unique_id_p) {
      // This file doesn't contain moonlight credentials
      http::unique_id = uuid_util::uuid_t::generate().string();
      return;
    }
    http::unique_id = std::move(*unique_id_p);

    auto root = tree.get_child("root");
    client_t client;

    // Import from old format
    if (root.get_child_optional("devices")) {
      auto device_nodes = root.get_child("devices");
      for (auto &[_, device_node] : device_nodes) {
        auto uniqID = device_node.get<std::string>("uniqueid");

        if (device_node.count("certs")) {
          for (auto &[_, el] : device_node.get_child("certs")) {
            named_cert_t named_cert;
            named_cert.name = ""s;
            named_cert.cert = el.get_value<std::string>();
            named_cert.uuid = uuid_util::uuid_t::generate().string();
            client.named_devices.emplace_back(named_cert);
          }
        }
      }
    }

    if (root.count("named_devices")) {
      for (auto &[_, el] : root.get_child("named_devices")) {
        named_cert_t named_cert;
        named_cert.name = el.get_child("name").get_value<std::string>();
        named_cert.cert = el.get_child("cert").get_value<std::string>();
        named_cert.uuid = el.get_child("uuid").get_value<std::string>();
        named_cert.enabled = el.get<bool>("enabled", true);
        named_cert.tailnet_scope = el.get<std::string>("aurora_tailnet_scope", "");
        named_cert.unpaired = el.get<bool>("aurora_unpaired", false);
        client.named_devices.emplace_back(named_cert);
      }
    }

    for (auto &named_cert : client.named_devices) {
      auto canonical_certificate = canonical_certificate_pem(named_cert.cert);
      if (!canonical_certificate.empty()) {
        named_cert.cert = std::move(canonical_certificate);
      }
    }

    std::lock_guard lock {client_auth_mutex()};
    client_root = std::move(client);
    rebuild_client_cert_chain();
  }

  /**
   * @brief Add authorized client data.
   *
   * @param name Human-readable name to assign.
   * @param cert Certificate data or object used by the operation.
   * @return Persistent UUID for the added client, or an empty string when the certificate is invalid.
   */
  std::string add_authorized_client(const std::string &name, std::string &&cert) {
    auto canonical_certificate = canonical_certificate_pem(cert);
    if (canonical_certificate.empty()) {
      return {};
    }

    named_cert_t named_cert;
    named_cert.name = name;
    named_cert.cert = std::move(canonical_certificate);
    named_cert.uuid = uuid_util::uuid_t::generate().string();

    std::lock_guard lock {client_auth_mutex()};
    if (std::ranges::any_of(client_root.named_devices, [&named_cert](const auto &entry) {
          return !entry.tailnet_scope.empty() && entry.cert == named_cert.cert;
        })) {
      return {};
    }
    client_root.named_devices.emplace_back(std::move(named_cert));
    rebuild_client_cert_chain();

    if (!config::sunshine.flags[config::flag::FRESH_STATE]) {
      save_state();
    }
    return client_root.named_devices.back().uuid;
  }

#ifdef AURORA_SUNSHINE_HOST
  /**
   * @brief Check canonical lowercase hex and the decoded field bound.
   * @param hex Encoded field supplied by the local supervisor.
   * @param max_bytes Largest decoded value allowed for the field.
   * @return True when the field can be decoded within its limit.
   */
  static bool valid_proof_hex(std::string_view hex, std::size_t max_bytes) {
    return !hex.empty() && hex.size() % 2 == 0 && hex.size() / 2 <= max_bytes &&
      std::ranges::all_of(hex, [](char byte) {
        return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
      });
  }

  tailnet_grant_result grant_tailnet_client(std::string_view name,
    std::string_view challenge_hex, std::string_view certificate_hex, std::string_view signature_hex) {
    using namespace Aurora::ClientProof;
    using result = tailnet_grant_result;
    if (!is_valid_pairing_name(name) || challenge_hex.size() != ChallengeSize * 2 ||
        !valid_proof_hex(challenge_hex, ChallengeSize) ||
        !valid_proof_hex(certificate_hex, MaxCertificateBytes) ||
        !valid_proof_hex(signature_hex, MaxSignatureBytes)) {
      return result::invalid_proof;
    }

    const auto challenge_bytes = util::from_hex_vec(std::string {challenge_hex}, true);
    Challenge challenge {};
    std::ranges::copy(challenge_bytes, challenge.begin());
    if (std::all_of(challenge.begin(), challenge.begin() + DigestSize, [](auto byte) { return byte == 0; }) ||
        std::all_of(challenge.end() - DigestSize, challenge.end(), [](auto byte) { return byte == 0; })) {
      return result::invalid_proof;
    }

    const auto der = util::from_hex_vec(std::string {certificate_hex}, true);
    const auto *cursor = reinterpret_cast<const unsigned char *>(der.data());
    const auto *end = cursor + der.size();
    crypto::x509_t cert {d2i_X509(nullptr, &cursor, static_cast<long>(der.size()))};
    if (!cert || cursor != end ||
        X509_cmp_current_time(X509_get0_notBefore(cert.get())) >= 0 ||
        X509_cmp_current_time(X509_get0_notAfter(cert.get())) <= 0) {
      return result::invalid_proof;
    }

    const auto message = transcript(challenge, crypto::hash(der));
    const std::string_view proof {reinterpret_cast<const char *>(message.data()), message.size()};
    if (!crypto::verify256(cert, proof, util::from_hex_vec(std::string {signature_hex}, true))) {
      return result::invalid_proof;
    }
    auto pem = crypto::pem(cert);
    if (pem.empty()) {
      return result::invalid_proof;
    }
    const std::string scope {challenge_hex.substr(4 * DigestSize)};

    std::lock_guard lock {client_auth_mutex()};
    if (!tailnet_host_fingerprint || config::sunshine.flags[config::flag::FRESH_STATE]) {
      return result::unavailable;
    }
    if (!std::equal(tailnet_host_fingerprint->begin(), tailnet_host_fingerprint->end(),
        challenge.begin() + DigestSize)) {
      return result::invalid_proof;
    }

    named_cert_t *matched = nullptr;
    for (auto &entry : client_root.named_devices) {
      const bool same_certificate = entry.cert == pem;
      const bool same_scope = entry.tailnet_scope == scope;
      if (!same_certificate && !same_scope) {
        continue;
      }
      if (!entry.enabled || entry.unpaired) {
        return result::revoked;
      }
      if (matched || !same_certificate || (!entry.tailnet_scope.empty() && !same_scope)) {
        return result::conflict;
      }
      matched = &entry;
    }

    const auto until = std::chrono::steady_clock::now() + std::chrono::minutes {5};
    if (matched) {
      if (!matched->tailnet_scope.empty()) {
        matched->tailnet_until = until;
      }
      return result::accepted;
    }

    auto &clients = client_root.named_devices;
    clients.push_back({std::string {name}, uuid_util::uuid_t::generate().string(), std::move(pem), true,
      scope, false, until});
    if (!save_state()) {
      clients.pop_back();
      return result::unavailable;
    }
    rebuild_client_cert_chain();
    return result::accepted;
  }

  void expire_tailnet_client_grants() {
    std::lock_guard lock {client_auth_mutex()};
    for (auto &client : client_root.named_devices) {
      if (!client.tailnet_scope.empty()) {
        client.tailnet_until = {};
      }
    }
  }
#endif

  /**
   * @brief Create launch session.
   *
   * @param host_audio Host audio.
   * @param args Arguments forwarded to the callable or parser.
   * @return Constructed launch session object.
   */
  using paired_identity_t = std::pair<std::string, std::string>;

  std::shared_ptr<rtsp_stream::launch_session_t> make_launch_session(bool host_audio, const args_t &args, const paired_identity_t *identity) {
    auto launch_session = std::make_shared<rtsp_stream::launch_session_t>();

    launch_session->id = ++session_id_counter;

    auto rikey = util::from_hex_vec(get_arg(args, "rikey"), true);
    std::copy(rikey.cbegin(), rikey.cend(), std::back_inserter(launch_session->gcm_key));

    launch_session->host_audio = host_audio;
    std::stringstream mode = std::stringstream(get_arg(args, "mode", "0x0x0"));
    // Split mode by the char "x", to populate width/height/fps
    int x = 0;
    std::string segment;
    while (std::getline(mode, segment, 'x')) {
      if (x == 0) {
        launch_session->width = atoi(segment.c_str());
      }
      if (x == 1) {
        launch_session->height = atoi(segment.c_str());
      }
      if (x == 2) {
        launch_session->fps = atoi(segment.c_str());
      }
      x++;
    }
    launch_session->unique_id = (get_arg(args, "uniqueid", "unknown"));
    launch_session->appid = (int) util::from_view(get_arg(args, "appid", "unknown"));
    launch_session->enable_sops = util::from_view(get_arg(args, "sops", "0"));
    launch_session->surround_info = (int) util::from_view(get_arg(args, "surroundAudioInfo", "196610"));
    launch_session->surround_params = (get_arg(args, "surroundParams", ""));
    launch_session->continuous_audio = util::from_view(get_arg(args, "continuousAudio", "0"));
    launch_session->gcmap = (int) util::from_view(get_arg(args, "gcmap", "0"));
    launch_session->enable_hdr = util::from_view(get_arg(args, "hdrMode", "0"));

    // Encrypted RTSP is enabled with client reported corever >= 1
    auto corever = util::from_view(get_arg(args, "corever", "0"));
    if (corever >= 1) {
      launch_session->rtsp_cipher = crypto::cipher::gcm_t {
        launch_session->gcm_key,
        false
      };
      launch_session->rtsp_iv_counter = 0;
    }
    launch_session->rtsp_url_scheme = launch_session->rtsp_cipher ? "rtspenc://"s : "rtsp://"s;
    launch_session->client_cert = identity ? identity->first : last_verified_client_cert;
    launch_session->client_name = identity ? identity->second : last_verified_client_name;

    // Generate the unique identifiers for this connection that we will send later during RTSP handshake
    unsigned char raw_payload[8];
    RAND_bytes(raw_payload, sizeof(raw_payload));
    launch_session->av_ping_payload = util::hex_vec(raw_payload);
    RAND_bytes((unsigned char *) &launch_session->control_connect_data, sizeof(launch_session->control_connect_data));

    launch_session->iv.resize(16);
    uint32_t prepend_iv = util::endian::big<uint32_t>((int) util::from_view(get_arg(args, "rikeyid")));
    auto prepend_iv_p = (uint8_t *) &prepend_iv;
    std::copy(prepend_iv_p, prepend_iv_p + sizeof(prepend_iv), std::begin(launch_session->iv));
    return launch_session;
  }

  /**
   * @brief Write a completed response to the client waiting for PIN approval.
   *
   * @param sess Pairing session that owns the waiting response.
   * @param tree XML response body to write.
   * @return `true` when a live response was available.
   */
  bool write_pairing_response(pair_session_t &sess, const pt::ptree &tree) {
    std::ostringstream data;
    pt::write_xml(data, tree);

    auto &response = sess.async_insert_pin.response;
    if (response.has_left() && response.left()) {
      response.left()->close_connection_after_response = true;
      response.left()->write(data.str());
    } else if (response.has_right() && response.right()) {
      response.right()->close_connection_after_response = true;
      response.right()->write(data.str());
    } else {
      return false;
    }

    response = std::monostate {};
    return true;
  }

  /**
   * @brief Publish the final result of a pairing handshake.
   *
   * @param sess Pairing session whose waiter should be notified.
   * @param success Whether the client completed the authenticated handshake.
   */
  void complete_pairing(pair_session_t &sess, const bool success) {
    {
      std::lock_guard lock {sess.completion->mutex};
      if (sess.completion->result.has_value()) {
        return;
      }
      sess.completion->result = success;
    }
    sess.completion->condition.notify_all();
  }

  /**
   * @brief Expire stale pairing sessions while the session mutex is held.
   *
   * @param now Monotonic time used to evaluate session deadlines.
   */
  void expire_pair_sessions_unlocked(const std::chrono::steady_clock::time_point now) {
    for (auto it = map_id_sess.begin(); it != map_id_sess.end();) {
      if (it->second.async_insert_pin.expires_at > now) {
        ++it;
        continue;
      }

      pt::ptree tree;
      tree.put("root.paired", 0);
      tree.put("root.<xmlattr>.status_code", 408);
      tree.put("root.<xmlattr>.status_message", "Pairing session expired");
      write_pairing_response(it->second, tree);
      complete_pairing(it->second, false);
      it = map_id_sess.erase(it);
    }
  }

  pair_session_insert_e insert_pair_session(pair_session_t sess, std::string &pairing_id) {
    std::scoped_lock lock {map_id_sess_mutex()};
    const auto now = std::chrono::steady_clock::now();
    expire_pair_sessions_unlocked(now);
    pairing_id.clear();

    if (map_id_sess.contains(sess.client.uniqueID)) {
      return pair_session_insert_e::ALREADY_EXISTS;
    }
    if (map_id_sess.size() >= MAX_PENDING_PAIRING_SESSIONS) {
      return pair_session_insert_e::FULL;
    }

    do {
      pairing_id = crypto::rand_alphabet(PAIRING_ID_SIZE, "0123456789abcdef"sv);
    } while (std::ranges::any_of(map_id_sess, [&](const auto &entry) {
      return entry.second.async_insert_pin.id == pairing_id;
    }));

    sess.async_insert_pin.id = pairing_id;
    sess.async_insert_pin.expires_at = now + PAIRING_SESSION_TIMEOUT;
    map_id_sess.emplace(sess.client.uniqueID, std::move(sess));
    return pair_session_insert_e::ADDED;
  }

  bool is_valid_pairing_id(const std::string_view pairing_id) {
    return pairing_id.size() == PAIRING_ID_SIZE && std::ranges::all_of(pairing_id, [](const char character) {
             return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') || (character >= 'A' && character <= 'F');
           });
  }

  bool is_valid_pairing_pin(const std::string_view pin) {
    return pin.size() == 4 && std::ranges::all_of(pin, [](const char character) {
             return character >= '0' && character <= '9';
           });
  }

  bool is_valid_pairing_name(const std::string_view name) {
    return !name.empty() && name.size() <= MAX_PAIRING_CLIENT_NAME_SIZE;
  }

  void expire_pair_sessions(const std::chrono::steady_clock::time_point now) {
    std::scoped_lock lock {map_id_sess_mutex()};
    expire_pair_sessions_unlocked(now);
  }

  std::vector<pending_pairing_t> get_pending_pairings() {
    std::scoped_lock lock {map_id_sess_mutex()};
    expire_pair_sessions_unlocked(std::chrono::steady_clock::now());

    std::vector<const pair_session_t *> pending_sessions;
    pending_sessions.reserve(map_id_sess.size());
    for (const auto &[_, sess] : map_id_sess) {
      if (sess.last_phase == PAIR_PHASE::NONE) {
        pending_sessions.push_back(&sess);
      }
    }
    std::ranges::sort(pending_sessions, {}, [](const pair_session_t *sess) {
      return sess->async_insert_pin.expires_at;
    });

    std::vector<pending_pairing_t> result;
    result.reserve(pending_sessions.size());
    for (const auto *sess : pending_sessions) {
      result.push_back({
        .id = sess->async_insert_pin.id,
        .name = sess->async_insert_pin.device_name,
        .address = sess->async_insert_pin.address,
        .source_port = sess->async_insert_pin.source_port,
      });
    }
    return result;
  }

  bool cancel_pairing(const std::string_view pairing_id) {
    if (!is_valid_pairing_id(pairing_id)) {
      return false;
    }

    std::scoped_lock lock {map_id_sess_mutex()};
    expire_pair_sessions_unlocked(std::chrono::steady_clock::now());

    const auto sess_it = std::ranges::find_if(map_id_sess, [&](const auto &entry) {
      return entry.second.last_phase == PAIR_PHASE::NONE && entry.second.async_insert_pin.id == pairing_id;
    });
    if (sess_it == map_id_sess.end()) {
      return false;
    }

    pt::ptree tree;
    tree.put("root.paired", 0);
    tree.put("root.<xmlattr>.status_code", 400);
    tree.put("root.<xmlattr>.status_message", "Pairing request cancelled by operator");
    write_pairing_response(sess_it->second, tree);
    complete_pairing(sess_it->second, false);
    map_id_sess.erase(sess_it);
    return true;
  }

  void remove_session(const pair_session_t &sess) {
    std::scoped_lock lock {map_id_sess_mutex()};
    if (const auto sess_it = map_id_sess.find(sess.client.uniqueID); sess_it != map_id_sess.end()) {
      complete_pairing(sess_it->second, false);
      map_id_sess.erase(sess_it);
    }
  }

  /**
   * @brief Return the GameStream pairing failure response.
   *
   * @param sess Pairing session that owns the request state.
   * @param tree XML property tree used for the response body.
   * @param status_msg Status msg.
   */
  void fail_pair(pair_session_t &sess, pt::ptree &tree, const std::string status_msg) {
    tree.put("root.paired", 0);
    tree.put("root.<xmlattr>.status_code", 400);
    tree.put("root.<xmlattr>.status_message", status_msg);
    sess.failed = true;
    complete_pairing(sess, false);
  }

  /**
   * @brief Return the server certificate text for pairing responses.
   *
   * @param sess Pairing session that owns the request state.
   * @param tree XML property tree used for the response body.
   * @param pin PIN supplied by the client during pairing.
   */
  void getservercert(pair_session_t &sess, pt::ptree &tree, const std::string &pin) {
    if (sess.last_phase != PAIR_PHASE::NONE) {
      fail_pair(sess, tree, "Out of order call to getservercert");
      return;
    }
    sess.last_phase = PAIR_PHASE::GETSERVERCERT;

    if (sess.async_insert_pin.salt.size() < 32) {
      fail_pair(sess, tree, "Salt too short");
      return;
    }

    std::string_view salt_view {sess.async_insert_pin.salt.data(), 32};

    auto salt = util::from_hex<std::array<uint8_t, 16>>(salt_view, true);

    auto key = crypto::gen_aes_key(salt, pin);
    sess.cipher_key = std::make_unique<crypto::aes_t>(key);

    tree.put("root.paired", 1);
    tree.put("root.plaincert", util::hex_vec(conf_intern.servercert, true));
    tree.put("root.<xmlattr>.status_code", 200);
  }

  /**
   * @brief Handle the client-challenge phase of GameStream pairing.
   *
   * @param sess Pairing session that owns the request state.
   * @param tree XML property tree used for the response body.
   * @param challenge Client challenge bytes from the pairing request.
   */
  void clientchallenge(pair_session_t &sess, pt::ptree &tree, const std::string &challenge) {
    if (sess.last_phase != PAIR_PHASE::GETSERVERCERT) {
      fail_pair(sess, tree, "Out of order call to clientchallenge");
      return;
    }
    sess.last_phase = PAIR_PHASE::CLIENTCHALLENGE;

    if (!sess.cipher_key) {
      fail_pair(sess, tree, "Cipher key not set");
      return;
    }
    crypto::cipher::ecb_t cipher(*sess.cipher_key, false);

    std::vector<uint8_t> decrypted;
    cipher.decrypt(challenge, decrypted);

    auto x509 = crypto::x509(conf_intern.servercert);
    auto sign = crypto::signature(x509);
    auto serversecret = crypto::rand(16);

    decrypted.insert(std::end(decrypted), std::begin(sign), std::end(sign));
    decrypted.insert(std::end(decrypted), std::begin(serversecret), std::end(serversecret));

    auto hash = crypto::hash({(char *) decrypted.data(), decrypted.size()});
    auto serverchallenge = crypto::rand(16);

    std::string plaintext;
    plaintext.reserve(hash.size() + serverchallenge.size());

    plaintext.insert(std::end(plaintext), std::begin(hash), std::end(hash));
    plaintext.insert(std::end(plaintext), std::begin(serverchallenge), std::end(serverchallenge));

    std::vector<uint8_t> encrypted;
    cipher.encrypt(plaintext, encrypted);

    sess.serversecret = std::move(serversecret);
    sess.serverchallenge = std::move(serverchallenge);

    tree.put("root.paired", 1);
    tree.put("root.challengeresponse", util::hex_vec(encrypted, true));
    tree.put("root.<xmlattr>.status_code", 200);
  }

  /**
   * @brief Handle the server-challenge response phase of GameStream pairing.
   *
   * @param sess Pairing session that owns the request state.
   * @param tree XML property tree used for the response body.
   * @param encrypted_response Encrypted response.
   */
  void serverchallengeresp(pair_session_t &sess, pt::ptree &tree, const std::string &encrypted_response) {
    if (sess.last_phase != PAIR_PHASE::CLIENTCHALLENGE) {
      fail_pair(sess, tree, "Out of order call to serverchallengeresp");
      return;
    }
    sess.last_phase = PAIR_PHASE::SERVERCHALLENGERESP;

    if (!sess.cipher_key || sess.serversecret.empty()) {
      fail_pair(sess, tree, "Cipher key or serversecret not set");
      return;
    }

    std::vector<uint8_t> decrypted;
    crypto::cipher::ecb_t cipher(*sess.cipher_key, false);

    cipher.decrypt(encrypted_response, decrypted);

    sess.clienthash = std::move(decrypted);

    auto serversecret = sess.serversecret;
    auto sign = crypto::sign256(crypto::pkey(conf_intern.pkey), serversecret);

    serversecret.insert(std::end(serversecret), std::begin(sign), std::end(sign));

    tree.put("root.pairingsecret", util::hex_vec(serversecret, true));
    tree.put("root.paired", 1);
    tree.put("root.<xmlattr>.status_code", 200);
  }

  /**
   * @brief Handle the client pairing-secret phase of GameStream pairing.
   *
   * @param sess Pairing session that owns the request state.
   * @param tree XML property tree used for the response body.
   * @param client_pairing_secret Client pairing secret.
   */
  void clientpairingsecret(pair_session_t &sess, pt::ptree &tree, const std::string &client_pairing_secret) {
    if (sess.last_phase != PAIR_PHASE::SERVERCHALLENGERESP) {
      fail_pair(sess, tree, "Out of order call to clientpairingsecret");
      return;
    }
    sess.last_phase = PAIR_PHASE::CLIENTPAIRINGSECRET;

    auto &client = sess.client;

    if (client_pairing_secret.size() <= 16) {
      fail_pair(sess, tree, "Client pairing secret too short");
      return;
    }

    std::string_view secret {client_pairing_secret.data(), 16};
    std::string_view sign {client_pairing_secret.data() + secret.size(), client_pairing_secret.size() - secret.size()};

    auto x509 = crypto::x509(client.cert);
    if (!x509) {
      fail_pair(sess, tree, "Invalid client certificate");
      return;
    }
    auto x509_sign = crypto::signature(x509);

    std::string data;
    data.reserve(sess.serverchallenge.size() + x509_sign.size() + secret.size());

    data.insert(std::end(data), std::begin(sess.serverchallenge), std::end(sess.serverchallenge));
    data.insert(std::end(data), std::begin(x509_sign), std::end(x509_sign));
    data.insert(std::end(data), std::begin(secret), std::end(secret));

    auto hash = crypto::hash(data);

    // if hash not correct, probably MITM
    bool same_hash = hash.size() == sess.clienthash.size() && std::equal(hash.begin(), hash.end(), sess.clienthash.begin());
    auto verify = crypto::verify256(crypto::x509(client.cert), secret, sign);
    bool paired = false;
    if (same_hash && verify) {
      // The client is now successfully paired and will be authorized to connect
      paired = !add_authorized_client(client.name, std::move(client.cert)).empty();
    }

    tree.put("root.paired", paired ? 1 : 0);
    tree.put("root.<xmlattr>.status_code", 200);
    complete_pairing(sess, paired);
  }

  template<class T>
  struct tunnel;

  /**
   * @brief HTTPS tunnel session used for encrypted client requests.
   */
  template<>
  struct tunnel<SunshineHTTPS> {
    static auto constexpr to_string = "HTTPS"sv;  ///< To string.
  };

  /**
   * @brief Plain HTTP server wrapper used for non-TLS endpoints.
   */
  template<>
  struct tunnel<SimpleWeb::HTTP> {
    static auto constexpr to_string = "NONE"sv;  ///< To string.
  };

  /**
   * @brief Write req details to the log.
   *
   * @param request HTTP request data from the client.
   */
  template<class T>
  void print_req(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    BOOST_LOG(debug) << "TUNNEL :: "sv << tunnel<T>::to_string;

    BOOST_LOG(debug) << "METHOD :: "sv << request->method;
    BOOST_LOG(debug) << "DESTINATION :: "sv << request->path;

    for (auto &[name, val] : request->header) {
      BOOST_LOG(debug) << name << " -- " << val;
    }

    BOOST_LOG(debug) << " [--] "sv;

    for (auto &[name, val] : request->parse_query_string()) {
      BOOST_LOG(debug) << name << " -- " << val;
    }

    BOOST_LOG(debug) << " [--] "sv;
  }

  /**
   * @brief Return a GameStream HTTP not-found response.
   *
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client.
   */
  template<class T>
  void not_found(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    pt::ptree tree;
    tree.put("root.<xmlattr>.status_code", 404);

    std::ostringstream data;

    pt::write_xml(data, tree);
    response->write(data.str());

    *response
      << "HTTP/1.1 404 NOT FOUND\r\n"
      << data.str();

    response->close_connection_after_response = true;
  }

  /**
   * @brief Start a GameStream pairing session for a `getservercert` request.
   *
   * @tparam T HTTP server transport type.
   * @param response HTTP response retained while Web UI PIN approval is pending.
   * @param request HTTP request containing the client pairing parameters.
   * @param args Parsed request query parameters.
   * @param tree XML property tree used for immediate responses.
   * @param unique_id Client-provided pairing-session identifier.
   * @return `true` when the response remains pending for Web UI PIN approval.
   */
  template<class T>
  bool start_pairing_session(
    const std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> &response,
    const std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> &request,
    const args_t &args,
    pt::ptree &tree,
    const std::string &unique_id
  ) {
    pair_session_t sess;
    sess.client.uniqueID = unique_id;
    sess.client.cert = util::from_hex_vec(get_arg(args, "clientcert"), true);
    sess.async_insert_pin.salt = get_arg(args, "salt");
    sess.async_insert_pin.device_name = get_arg(args, "devicename");
    const auto source = request->remote_endpoint();
    sess.async_insert_pin.address = net::addr_to_normalized_string(source.address());
    sess.async_insert_pin.source_port = source.port();

    BOOST_LOG(debug) << sess.client.cert;
    const bool pin_stdin = config::sunshine.flags[config::flag::PIN_STDIN];
    if (!pin_stdin) {
      sess.async_insert_pin.response = response;
    }

    std::string pairing_id;
    switch (insert_pair_session(std::move(sess), pairing_id)) {
      using enum pair_session_insert_e;

      case ALREADY_EXISTS:
        tree.put("root.paired", 0);
        tree.put("root.<xmlattr>.status_code", 409);
        tree.put("root.<xmlattr>.status_message", "A pairing session with this uniqueid already exists");
        return false;
      case FULL:
        tree.put("root.paired", 0);
        tree.put("root.<xmlattr>.status_code", 503);
        tree.put("root.<xmlattr>.status_message", "Too many pending pairing sessions");
        return false;
      case ADDED:
        break;
    }

    if (!pin_stdin) {
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
      system_tray::update_tray_require_pin();
#endif
      return true;
    }

    std::string pin;
    std::cout << "Please insert pin: "sv;
    std::getline(std::cin, pin);

    std::scoped_lock lock {map_id_sess_mutex()};
    expire_pair_sessions_unlocked(std::chrono::steady_clock::now());
    const auto sess_it = map_id_sess.find(unique_id);
    if (sess_it == map_id_sess.end() || sess_it->second.async_insert_pin.id != pairing_id) {
      tree.put("root.paired", 0);
      tree.put("root.<xmlattr>.status_code", 408);
      tree.put("root.<xmlattr>.status_message", "Pairing session expired");
      return false;
    }

    getservercert(sess_it->second, tree, pin);
    if (sess_it->second.failed) {
      map_id_sess.erase(sess_it);
    }
    return false;
  }

  /**
   * @brief Dispatch the top-level GameStream pairing request by phase.
   *
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client.
   */
  template<class T>
  void pair(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    pt::ptree tree;

    auto fg = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto args = request->parse_query_string();
    if (args.find("uniqueid"s) == std::end(args)) {
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing uniqueid parameter");

      return;
    }

    auto uniqID {get_arg(args, "uniqueid")};

    const auto phrase_it = args.find("phrase");
    if (phrase_it != std::end(args) && phrase_it->second == "getservercert"sv) {
      if (start_pairing_session<T>(response, request, args, tree, uniqID)) {
        fg.disable();
      }
      return;
    }
    if (phrase_it != std::end(args) && phrase_it->second == "pairchallenge"sv) {
      tree.put("root.paired", 1);
      tree.put("root.<xmlattr>.status_code", 200);
      return;
    }

    std::scoped_lock lock {map_id_sess_mutex()};
    expire_pair_sessions_unlocked(std::chrono::steady_clock::now());
    auto sess_it = map_id_sess.find(uniqID);
    if (sess_it == std::end(map_id_sess)) {
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Invalid uniqueid");

      return;
    }

    bool pairing_complete = false;
    args_t::const_iterator it;
    if (it = args.find("clientchallenge"); it != std::end(args)) {
      auto challenge = util::from_hex_vec(it->second, true);
      clientchallenge(sess_it->second, tree, challenge);
    } else if (it = args.find("serverchallengeresp"); it != std::end(args)) {
      auto encrypted_response = util::from_hex_vec(it->second, true);
      serverchallengeresp(sess_it->second, tree, encrypted_response);
    } else if (it = args.find("clientpairingsecret"); it != std::end(args)) {
      auto pairingsecret = util::from_hex_vec(it->second, true);
      clientpairingsecret(sess_it->second, tree, pairingsecret);
      pairing_complete = true;
    } else {
      fail_pair(sess_it->second, tree, "Invalid pairing request");
    }

    if (pairing_complete || sess_it->second.failed) {
      map_id_sess.erase(sess_it);
    }
  }

  bool pin(const std::string_view pairing_id, std::string pin, std::string name) {
    if (!is_valid_pairing_id(pairing_id) || !is_valid_pairing_pin(pin) || !is_valid_pairing_name(name)) {
      return false;
    }

    std::shared_ptr<pairing_completion_t> completion;
    auto completion_deadline = std::chrono::steady_clock::time_point::min();
    {
      std::scoped_lock lock {map_id_sess_mutex()};
      const auto now = std::chrono::steady_clock::now();
      expire_pair_sessions_unlocked(now);
      const auto sess_it = std::ranges::find_if(map_id_sess, [&](const auto &entry) {
        return entry.second.last_phase == PAIR_PHASE::NONE && entry.second.async_insert_pin.id == pairing_id;
      });
      if (sess_it == map_id_sess.end()) {
        return false;
      }

      auto &sess = sess_it->second;
      pt::ptree tree;
      getservercert(sess, tree, pin);
      if (!sess.failed) {
        sess.client.name = std::move(name);
      }

      if (!write_pairing_response(sess, tree) || sess.failed) {
        complete_pairing(sess, false);
        map_id_sess.erase(sess_it);
        return false;
      }

      completion = sess.completion;
      completion_deadline = std::min(sess.async_insert_pin.expires_at, now + config::stream.ping_timeout);
    }

    {
      std::unique_lock lock {completion->mutex};
      if (completion->condition.wait_until(lock, completion_deadline, [&completion]() {
            return completion->result.has_value();
          })) {
        return *completion->result;
      }
    }

    std::scoped_lock lock {map_id_sess_mutex()};
    if (const auto sess_it = std::ranges::find_if(map_id_sess, [&](const auto &entry) {
          return entry.second.async_insert_pin.id == pairing_id && entry.second.completion == completion;
        });
        sess_it != map_id_sess.end()) {
      complete_pairing(sess_it->second, false);
      map_id_sess.erase(sess_it);
    }
    return false;
  }

  /**
   * @brief Get codec mode flags.
   *
   * @return Moonlight codec capability bitmask for the currently probed encoders.
   */
  uint32_t get_codec_mode_flags(const video::encoder_capabilities_t &encoder) {
    uint32_t codec_mode_flags = SCM_H264;
    if (encoder.yuv444[0]) {
      codec_mode_flags |= SCM_H264_HIGH8_444;
    }
    if (encoder.hevc_mode >= 2) {
      codec_mode_flags |= SCM_HEVC;
      if (encoder.yuv444[1]) {
        codec_mode_flags |= SCM_HEVC_REXT8_444;
      }
    }
    if (encoder.hevc_mode == 3 || encoder.hevc_mode == 5) {
      codec_mode_flags |= SCM_HEVC_MAIN10;
    }
    if ((encoder.hevc_mode == 4 || encoder.hevc_mode == 5) && encoder.yuv444[1]) {
      codec_mode_flags |= SCM_HEVC_REXT10_444;
    }

    if (encoder.av1_mode >= 2) {
      codec_mode_flags |= SCM_AV1_MAIN8;
      if (encoder.yuv444[2]) {
        codec_mode_flags |= SCM_AV1_HIGH8_444;
      }
    }
    if (encoder.av1_mode == 3 || encoder.av1_mode == 5) {
      codec_mode_flags |= SCM_AV1_MAIN10;
    }
    if ((encoder.av1_mode == 4 || encoder.av1_mode == 5) && encoder.yuv444[2]) {
      codec_mode_flags |= SCM_AV1_HIGH10_444;
    }
    return codec_mode_flags;
  }

  /**
   * @brief Build the GameStream server-info response.
   *
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client.
   */
  template<class T>
  void serverinfo(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    int pair_status = 0;
    if constexpr (std::is_same_v<SunshineHTTPS, T>) {
      auto args = request->parse_query_string();
      auto clientID = args.find("uniqueid"s);

      if (clientID != std::end(args)) {
        pair_status = 1;
      }
    }

    auto local_endpoint = request->local_endpoint();

    pt::ptree tree;

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put("root.hostname", config::nvhttp.sunshine_name);

    tree.put("root.appversion", VERSION);
    tree.put("root.GfeVersion", GFE_VERSION);
    tree.put("root.uniqueid", http::unique_id);
    tree.put("root.HttpsPort", net::map_port(PORT_HTTPS));
    tree.put("root.ExternalPort", net::map_port(PORT_HTTP));
    const auto encoder = video::get_encoder_capabilities();
#ifdef AURORA_SUNSHINE_HOST
    using video::encoder_probe_status;
    const auto status = encoder.status == encoder_probe_status::probing ? "probing" :
                        encoder.status == encoder_probe_status::ready ? "ready" : "failed";
    tree.put("root.AuroraEncoderStatus", status);
#endif
#if defined(AURORA_SUNSHINE_HOST) && (defined(_WIN32) || defined(__APPLE__))
    tree.put("root.AuroraPower", 1);
#endif
#if defined(_WIN32) && defined(AURORA_SUNSHINE_HOST)
    tree.put("root.AuroraSessionPrivacy", 1);
#endif
    tree.put("root.MaxLumaPixelsHEVC", encoder.hevc_mode > 1 ? "1869449984" : "0");

    // Only include the MAC address for requests sent from paired clients over HTTPS.
    // For HTTP requests, use a placeholder MAC address that Moonlight knows to ignore.
    if constexpr (std::is_same_v<SunshineHTTPS, T>) {
      tree.put("root.mac", platf::get_mac_address(net::addr_to_normalized_string(local_endpoint.address())));
    } else {
      tree.put("root.mac", "00:00:00:00:00:00");
    }

    // Moonlight clients track LAN IPv6 addresses separately from LocalIP which is expected to
    // always be an IPv4 address. If we return that same IPv6 address here, it will clobber the
    // stored LAN IPv4 address. To avoid this, we need to return an IPv4 address in this field
    // when we get a request over IPv6.
    //
    // HACK: We should return the IPv4 address of local interface here, but we don't currently
    // have that implemented. For now, we will emulate the behavior of GFE+GS-IPv6-Forwarder,
    // which returns 127.0.0.1 as LocalIP for IPv6 connections. Moonlight clients with IPv6
    // support know to ignore this bogus address.
    if (local_endpoint.address().is_v6() && !local_endpoint.address().to_v6().is_v4_mapped()) {
      tree.put("root.LocalIP", "127.0.0.1");
    } else {
      tree.put("root.LocalIP", net::addr_to_normalized_string(local_endpoint.address()));
    }

    const uint32_t codec_mode_flags = get_codec_mode_flags(encoder);
    tree.put("root.ServerCodecModeSupport", codec_mode_flags);

    if (!config::nvhttp.external_ip.empty()) {
      tree.put("root.ExternalIP", config::nvhttp.external_ip);
    }

    auto current_appid = proc::proc.running();
    tree.put("root.PairStatus", pair_status);
#if defined(AURORA_SUNSHINE_HOST) && (defined(_WIN32) || defined(__APPLE__))
    if constexpr (std::is_same_v<SunshineHTTPS, T>) {
      // An active stream can select another capture display.
      if (pair_status && rtsp_stream::session_count() == 0) {
        if (const auto size = platf::aurora_capture_size()) {
          tree.put("root.AuroraDisplayWidth", size->first);
          tree.put("root.AuroraDisplayHeight", size->second);
        }
      }
    }
#endif
    tree.put("root.currentgame", current_appid);
    tree.put("root.state", current_appid > 0 ? "SUNSHINE_SERVER_BUSY" : "SUNSHINE_SERVER_FREE");

    std::ostringstream data;

    pt::write_xml(data, tree);
    response->write(data.str());
    response->close_connection_after_response = true;
  }

  nlohmann::json get_all_clients() {
    nlohmann::json named_cert_nodes = nlohmann::json::array();
    std::lock_guard lock {client_auth_mutex()};
    for (const auto &named_cert : client_root.named_devices) {
      if (named_cert.unpaired) {
        continue;
      }
      nlohmann::json named_cert_node;
      named_cert_node["name"] = named_cert.name;
      named_cert_node["uuid"] = named_cert.uuid;
      named_cert_node["enabled"] = named_cert.enabled;
      named_cert_nodes.push_back(named_cert_node);
    }

    return named_cert_nodes;
  }

  /**
   * @brief Build the GameStream application list response.
   *
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client.
   */
  void applist(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    pt::ptree tree;

    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto &apps = tree.add_child("root", pt::ptree {});

    apps.put("<xmlattr>.status_code", 200);

    const bool hdr_supported = video::get_encoder_capabilities().hevc_mode >= 3;
    for (auto &proc : proc::proc.get_apps()) {
      pt::ptree app;

      app.put("IsHdrSupported"s, hdr_supported ? 1 : 0);
      app.put("AppTitle"s, proc.name);
      app.put("ID", proc.id);

      apps.push_back(std::make_pair("App", std::move(app)));
    }
  }

#if defined(_WIN32) && defined(AURORA_SUNSHINE_HOST)
  bool aurora_screensaver_client_authorized(std::string_view cert) {
    auto certificate = crypto::x509(cert);
    if (!certificate) return false;
    const std::lock_guard lock {client_auth_mutex()};
    return verify_client_certificate(certificate.get()) == nullptr;
  }

  bool aurora_check_display(pt::ptree &tree, bool before_switch) {
    if (before_switch && config::aurora_privacy_screensaver && !display_device::aurora_screensaver_supported()) {
      BOOST_LOG(warning) << "Aurora privacy screen requires Windows Graphics Capture and display affinity support";
      tree.put("root.<xmlattr>.status_code", 503);
      tree.put("root.<xmlattr>.status_message", "Aurora: the privacy screen requires Windows Graphics Capture");
      tree.put("root.gamesession", 0);
      tree.put("root.resume", 0);
      return false;
    }
    const auto &target = config::aurora_display_id;
    if (target.empty()) return true;
    if (before_switch) {
      const auto devices = display_device::aurora_virtual_displays();
      if (std::any_of(devices.begin(), devices.end(), [&](const auto &device) { return device.m_device_id == target; })) return true;
    } else if (display_device::aurora_display_ready()) {
      return true;
    }
    tree.put("root.<xmlattr>.status_code", 503);
    tree.put("root.<xmlattr>.status_message", before_switch ? "Aurora: the configured virtual display is unavailable" :
      "Aurora: could not prepare the selected virtual display");
    tree.put("root.gamesession", 0);
    tree.put("root.resume", 0);
    return false;
  }
#endif

  /**
   * @brief Launch the requested application for a GameStream session.
   *
   * @param host_audio Host audio.
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client.
   */
  void launch(bool &host_audio, resp_https_t response, req_https_t request, const paired_identity_t *identity = nullptr) {
    print_req<SunshineHTTPS>(request);

    pt::ptree tree;
    bool revert_display_configuration {false};
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      if (tree.empty()) {
        BOOST_LOG(error) << EMPTY_PROPERTY_TREE_ERROR_MSG;
      }

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;

      if (revert_display_configuration) {
        display_device::revert_configuration();
      }
    });

    auto args = request->parse_query_string();
    if (
      args.find("rikey"s) == std::end(args) ||
      args.find("rikeyid"s) == std::end(args) ||
      args.find("localAudioPlayMode"s) == std::end(args) ||
      args.find("appid"s) == std::end(args)
    ) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing a required launch parameter");

      return;
    }

    auto appid = util::from_view(get_arg(args, "appid"));

    auto current_appid = proc::proc.running();
    if (current_appid > 0) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "An app is already running on this host");

      return;
    }

    video::wait_for_initial_encoder_probe();

    host_audio = util::from_view(get_arg(args, "localAudioPlayMode"));
    auto launch_session = make_launch_session(host_audio, args, identity);

    if (rtsp_stream::session_count() == 0) {
#if defined(_WIN32) && defined(AURORA_SUNSHINE_HOST)
      if (!aurora_check_display(tree, true)) return;
#endif
      // The display should be restored in case something fails as there are no other sessions.
      revert_display_configuration = true;

      // We want to prepare display only if there are no active sessions at
      // the moment. This should be done before probing encoders as it could
      // change the active displays.
      display_device::configure_display(config::video, *launch_session);
#if defined(_WIN32) && defined(AURORA_SUNSHINE_HOST)
      if (!aurora_check_display(tree, false)) return;
#endif

      // Probe encoders again before streaming to ensure our chosen
      // encoder matches the active GPU (which could have changed
      // due to hotplugging, driver crash, primary monitor change,
      // or any number of other factors).
      if (video::probe_encoders()) {
        tree.put("root.<xmlattr>.status_code", 503);
        tree.put("root.<xmlattr>.status_message", "Failed to initialize video capture/encoding. Is a display connected and turned on?");
        tree.put("root.gamesession", 0);

        return;
      }
    }

    auto encryption_mode = net::encryption_mode_for_address(request->remote_endpoint().address());
    if (!launch_session->rtsp_cipher && encryption_mode == config::ENCRYPTION_MODE_MANDATORY) {
      BOOST_LOG(error) << "Rejecting client that cannot comply with mandatory encryption requirement"sv;

      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Encryption is mandatory for this host but unsupported by the client");
      tree.put("root.gamesession", 0);

      return;
    }

    if (appid > 0) {
      auto err = proc::proc.execute((int) appid, launch_session);
      if (err) {
        tree.put("root.<xmlattr>.status_code", err);
        tree.put("root.<xmlattr>.status_message", "Failed to start the specified application");
        tree.put("root.gamesession", 0);

        return;
      }
    }

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put(
      "root.sessionUrl0",
      std::format(
        "{}{}:{}",
        launch_session->rtsp_url_scheme,
        net::addr_to_url_escaped_string(request->local_endpoint().address()),
        static_cast<int>(net::map_port(rtsp_stream::RTSP_SETUP_PORT))
      )
    );
    tree.put("root.gamesession", 1);

    rtsp_stream::launch_session_raise(launch_session);

    // Stream was started successfully, we will revert the config when the app or session terminates
    revert_display_configuration = false;
  }

  /**
   * @brief Resume an existing GameStream session.
   *
   * @param host_audio Host audio.
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client.
   */
  void resume(bool &host_audio, resp_https_t response, req_https_t request, const paired_identity_t *identity = nullptr) {
    print_req<SunshineHTTPS>(request);

    pt::ptree tree;
#if defined(_WIN32) && defined(AURORA_SUNSHINE_HOST)
    bool revert_privacy_configuration = false;
    auto restore_privacy = util::fail_guard([&]() {
      if (revert_privacy_configuration) display_device::revert_configuration();
    });
#endif
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      if (tree.empty()) {
        BOOST_LOG(error) << EMPTY_PROPERTY_TREE_ERROR_MSG;
      }

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto current_appid = proc::proc.running();
    if (current_appid == 0) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 503);
      tree.put("root.<xmlattr>.status_message", "No running app to resume");

      return;
    }

    auto args = request->parse_query_string();
    if (
      args.find("rikey"s) == std::end(args) ||
      args.find("rikeyid"s) == std::end(args)
    ) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing a required resume parameter");

      return;
    }

    video::wait_for_initial_encoder_probe();

    // Newer Moonlight clients send localAudioPlayMode on /resume too,
    // so we should use it if it's present in the args and there are
    // no active sessions we could be interfering with.
    const bool no_active_sessions {rtsp_stream::session_count() == 0};
    if (no_active_sessions && args.find("localAudioPlayMode"s) != std::end(args)) {
      host_audio = util::from_view(get_arg(args, "localAudioPlayMode"));
    }
    const auto launch_session = make_launch_session(host_audio, args, identity);

    if (no_active_sessions) {
#if defined(_WIN32) && defined(AURORA_SUNSHINE_HOST)
      if (!aurora_check_display(tree, true)) return;
      revert_privacy_configuration = !config::aurora_display_id.empty();
#endif
      // We want to prepare display only if there are no active sessions at
      // the moment. This should be done before probing encoders as it could
      // change the active displays.
      display_device::configure_display(config::video, *launch_session);
#if defined(_WIN32) && defined(AURORA_SUNSHINE_HOST)
      if (!aurora_check_display(tree, false)) return;
#endif

      // Probe encoders again before streaming to ensure our chosen
      // encoder matches the active GPU (which could have changed
      // due to hotplugging, driver crash, primary monitor change,
      // or any number of other factors).
      if (video::probe_encoders()) {
        tree.put("root.resume", 0);
        tree.put("root.<xmlattr>.status_code", 503);
        tree.put("root.<xmlattr>.status_message", "Failed to initialize video capture/encoding. Is a display connected and turned on?");

        return;
      }
    }

    auto encryption_mode = net::encryption_mode_for_address(request->remote_endpoint().address());
    if (!launch_session->rtsp_cipher && encryption_mode == config::ENCRYPTION_MODE_MANDATORY) {
      BOOST_LOG(error) << "Rejecting client that cannot comply with mandatory encryption requirement"sv;

      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Encryption is mandatory for this host but unsupported by the client");
      tree.put("root.gamesession", 0);

      return;
    }

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put(
      "root.sessionUrl0",
      std::format(
        "{}{}:{}",
        launch_session->rtsp_url_scheme,
        net::addr_to_url_escaped_string(request->local_endpoint().address()),
        static_cast<int>(net::map_port(rtsp_stream::RTSP_SETUP_PORT))
      )
    );
    tree.put("root.resume", 1);

    rtsp_stream::launch_session_raise(launch_session);
#if defined(_WIN32) && defined(AURORA_SUNSHINE_HOST)
    revert_privacy_configuration = false;
#endif
  }

  /**
   * @brief Check whether cel.
   *
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client.
   */
  void cancel(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    pt::ptree tree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    tree.put("root.cancel", 1);
    tree.put("root.<xmlattr>.status_code", 200);

    rtsp_stream::terminate_sessions();

    if (proc::proc.running() > 0) {
      proc::proc.terminate();
    }

    // The config needs to be reverted regardless of whether "proc::proc.terminate()" was called or not.
    display_device::revert_configuration();
  }

  /**
   * @brief Return an application asset requested by the client.
   *
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client.
   */
  void appasset(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    auto args = request->parse_query_string();
    auto app_image = proc::proc.get_app_image((int) util::from_view(get_arg(args, "appid")));

    std::ifstream in(app_image, std::ios::binary);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "image/png");
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
    response->close_connection_after_response = true;
  }

#if defined(AURORA_SUNSHINE_HOST) && (defined(_WIN32) || defined(__APPLE__))
  enum class aurora_power_action { restart, shutdown };
  using power_status = SimpleWeb::StatusCode;

  void aurora_text_response(const resp_https_t &response, power_status status, std::string_view message, SimpleWeb::CaseInsensitiveMultimap headers = {}) {
    headers.emplace("Content-Type", "text/plain; charset=utf-8");
    headers.emplace("Cache-Control", "no-store");
    headers.emplace("Connection", "close");
    response->write(status, message, headers);
    response->close_connection_after_response = true;
  }

  struct aurora_power_result {
    power_status status;
    std::string_view message;
  };

#ifdef _WIN32
  DWORD request_os_power(aurora_power_action action) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
      return GetLastError();
    }
    const auto close_token = util::fail_guard([&] { CloseHandle(token); });

    TOKEN_PRIVILEGES requested {};
    requested.PrivilegeCount = 1;
    requested.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValueW(nullptr, L"SeShutdownPrivilege", &requested.Privileges[0].Luid)) {
      return GetLastError();
    }

    TOKEN_PRIVILEGES previous {};
    DWORD previous_size = 0;
    const BOOL adjusted = AdjustTokenPrivileges(token, FALSE, &requested, sizeof(previous), &previous, &previous_size);
    const DWORD adjust_error = GetLastError();
    if (!adjusted || adjust_error != ERROR_SUCCESS) {
      return adjust_error;
    }
    const auto restore_privilege = util::fail_guard([&] {
      if (previous.PrivilegeCount && !AdjustTokenPrivileges(token, FALSE, &previous, 0, nullptr, nullptr)) {
        BOOST_LOG(error) << "Aurora could not restore the Windows shutdown privilege: " << GetLastError();
      }
    });

    constexpr DWORD grace_seconds = 5;
    const BOOL accepted = InitiateSystemShutdownExW(nullptr, nullptr, grace_seconds, FALSE,
      action == aurora_power_action::restart, SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_FLAG_PLANNED);
    return accepted ? ERROR_SUCCESS : GetLastError();
  }

  aurora_power_result describe_os_power_error(DWORD error) {
    if (error == ERROR_ACCESS_DENIED || error == ERROR_NOT_ALL_ASSIGNED || error == ERROR_PRIVILEGE_NOT_HELD) {
      return {power_status::client_error_forbidden, "Windows shutdown permission denied"};
    }
    if (error == ERROR_SHUTDOWN_IN_PROGRESS) {
      return {power_status::client_error_conflict, "Windows shutdown already in progress"};
    }
    return {power_status::server_error_internal_server_error, "Windows rejected the power request"};
  }
#elif defined(__APPLE__)
  OSStatus request_os_power(aurora_power_action action) {
    static constexpr auto loginwindow_bundle_id = "com.apple.loginwindow"sv;
    AEAddressDesc target {typeNull, nullptr};
    AppleEvent event {typeNull, nullptr};
    AppleEvent reply {typeNull, nullptr};
    const auto dispose = util::fail_guard([&] {
      AEDisposeDesc(&reply);
      AEDisposeDesc(&event);
      AEDisposeDesc(&target);
    });

    auto result = AECreateDesc(typeApplicationBundleID, loginwindow_bundle_id.data(), loginwindow_bundle_id.size(), &target);
    if (result != noErr) {
      return result;
    }
    // These events open the system confirmation. Use the normal loginwindow flow.
    const AEEventID event_id = action == aurora_power_action::restart ? kAEShowRestartDialog : kAEShowShutdownDialog;
    result = AECreateAppleEvent(kCoreEventClass, event_id, &target, kAutoGenerateReturnID, kAnyTransactionID, &event);
    if (result != noErr) {
      return result;
    }
    constexpr auto timeout_ticks = 5 * 60;
    result = AESendMessage(&event, &reply, kAEWaitReply | kAECanInteract, timeout_ticks);
    if (result != noErr) {
      return result;
    }
    SInt32 event_error = noErr;
    result = AEGetParamPtr(&reply, keyErrorNumber, typeSInt32, nullptr, &event_error, sizeof(event_error), nullptr);
    return result == noErr ? event_error : result == errAEDescNotFound ? noErr : result;
  }

  aurora_power_result describe_os_power_error(OSStatus error) {
    if (error == errAEEventNotPermitted) {
      return {power_status::client_error_forbidden, "macOS Automation permission denied"};
    }
    if (error == userCanceledErr) {
      return {power_status::client_error_conflict, "macOS power request canceled"};
    }
    if (error == errAETimeout) {
      return {power_status::server_error_gateway_timeout, "macOS did not confirm receipt"};
    }
    return {power_status::server_error_internal_server_error, "macOS rejected the power request"};
  }
#endif

  void aurora_power(resp_https_t response, req_https_t request) {
    const bool restart = request->query_string == "action=restart"sv;
    const bool shutdown = request->query_string == "action=shutdown"sv;
    if ((!restart && !shutdown) || request->content.size() != 0) {
      BOOST_LOG(warning) << "Aurora power request rejected: invalid action or unexpected body";
      aurora_text_response(response, power_status::client_error_bad_request, "expected action=restart or action=shutdown with an empty body");
      return;
    }

    const auto action = restart ? aurora_power_action::restart : aurora_power_action::shutdown;
    const auto error = request_os_power(action);
    const auto name = restart ? "restart"sv : "shutdown"sv;
    if (error != 0) {
      const auto result = describe_os_power_error(error);
      BOOST_LOG(warning) << "Aurora power request " << name << " failed: " << result.message << " (OS error " << error << ')';
      aurora_text_response(response, result.status, result.message);
      return;
    }
    BOOST_LOG(info) << "Aurora power request " << name << " accepted by the operating system";
    aurora_text_response(response, power_status::success_accepted, "accepted");
  }
#endif

#ifdef AURORA_SUNSHINE_HOST
  void aurora_wallpaper(resp_https_t response, req_https_t request) {
    constexpr std::streamsize max_preview_bytes = 512 * 1024;
    using SimpleWeb::StatusCode;
    std::ifstream image(platf::appdata() / "aurora-wallpaper.jpg", std::ios::binary | std::ios::ate);
    const auto size = image.tellg();
    if (size <= 0 || size > max_preview_bytes) {
      response->write(StatusCode::client_error_not_found);
      return;
    }
    std::string jpeg(static_cast<std::size_t>(size), '\0');
    image.seekg(0);
    if (!image.read(jpeg.data(), size)) {
      response->write(StatusCode::client_error_not_found);
      return;
    }

    const auto revision = get_arg(request->parse_query_string(), "revision", "");
    SimpleWeb::CaseInsensitiveMultimap headers {{"Cache-Control", "private, no-store"}, {"Connection", "close"}};
    if (revision.size() == 64 && revision == util::hex_vec(crypto::hash(jpeg), true)) {
      response->write(StatusCode::success_no_content, headers);
    } else {
      headers.emplace("Content-Type", "image/jpeg");
      response->write(StatusCode::success_ok, jpeg, headers);
    }
    // Simple-Web-Server omits Content-Length if this is set before write().
    response->close_connection_after_response = true;
  }
#endif

  void setup(const std::string &pkey, const std::string &cert) {
    conf_intern.pkey = pkey;
    conf_intern.servercert = cert;
#ifdef AURORA_SUNSHINE_HOST
    auto leaf = crypto::x509(cert);
    Aurora::ClientProof::CertificateDigest fingerprint {};
    unsigned int length = 0;
    const bool available = leaf && X509_digest(leaf.get(), EVP_sha256(), fingerprint.data(), &length) == 1 &&
      length == fingerprint.size();
    std::lock_guard lock {client_auth_mutex()};
    tailnet_host_fingerprint = available ? std::optional {fingerprint} : std::nullopt;
#endif
  }

  /**
   * @brief Check whether a paired client certificate is allowed to connect and retrieve its friendly name.
   *
   * @param cert_pem PEM-encoded client certificate to look up.
   * @return Pair of (bool enabled, string name) where "enabled" is True when the client certificate belongs to an
             enabled device and "name" is the friendly client name set during pairing.
   */
  std::pair<bool, std::string> get_client_status(const std::string_view cert_pem);

#ifdef AURORA_SUNSHINE_HOST
  std::optional<paired_identity_t> aurora_verified_peer(https_server_t &server, const https_server_t::Request &request) {
    auto cert = server.peer_certificate(request);
    if (!cert) return std::nullopt;
    std::lock_guard lock {client_auth_mutex()};
    if (verify_client_certificate(cert.get()) != nullptr) return std::nullopt;

    auto pem = crypto::pem(cert);
    return paired_identity_t {pem, get_client_status(pem).second};
  }
#endif

#if defined(_WIN32) && defined(AURORA_SUNSHINE_HOST)
  void aurora_session_privacy(https_server_t &server, resp_https_t response, req_https_t request) {
    using action_t = rtsp_stream::aurora_privacy_action;
    const auto &query = request->query_string;
    const auto action = query == "action=status"sv ? std::optional {action_t::status} :
                        query == "action=on"sv ? std::optional {action_t::on} :
                        query == "action=display-off"sv ? std::optional {action_t::display_off} :
                        query == "action=off"sv ? std::optional {action_t::off} : std::nullopt;
    if (!action || request->content.size() != 0) {
      aurora_text_response(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid privacy action");
      return;
    }
    const auto identity = aurora_verified_peer(server, *request);
    if (!identity) {
      aurora_text_response(response, SimpleWeb::StatusCode::client_error_forbidden, "paired client certificate required");
      return;
    }
    aurora_text_response(response, SimpleWeb::StatusCode::success_ok, rtsp_stream::aurora_session_privacy(identity->first, *action));
  }

  /** @brief Handle Aurora's paired, per-session bitrate POST. */
  void aurora_video_bitrate(https_server_t &server, resp_https_t response, req_https_t request) {
    const auto identity = aurora_verified_peer(server, *request);
    if (!identity) {
      aurora_text_response(response, SimpleWeb::StatusCode::client_error_forbidden, "unavailable");
      return;
    }
    const auto command = aurora::video_bitrate::parse(request->query_string, request->content.size() == 0);
    if (!command) {
      aurora_text_response(response, SimpleWeb::StatusCode::client_error_bad_request, "failed");
      return;
    }
    aurora_text_response(response, SimpleWeb::StatusCode::success_ok, rtsp_stream::aurora_session_bitrate(identity->first, *command));
  }
#endif

  void start() {
    platf::set_thread_name("nvhttp");
    auto shutdown_event = mail::man->event<bool>(mail::shutdown);

    auto port_http = net::map_port(PORT_HTTP);
    auto port_https = net::map_port(PORT_HTTPS);
    auto address_family = net::af_from_enum_string(config::sunshine.address_family);

    bool clean_slate = config::sunshine.flags[config::flag::FRESH_STATE];

    if (!clean_slate) {
      load_state();
    }

    auto pkey = file_handler::read_file(config::nvhttp.pkey.c_str());
    auto cert = file_handler::read_file(config::nvhttp.cert.c_str());
    setup(pkey, cert);

    // resume doesn't always get the parameter "localAudioPlayMode"
    // launch will store it in host_audio
    bool host_audio {};

    https_server_t https_server {config::nvhttp.cert, config::nvhttp.pkey};
    http_server_t http_server;

    // Verify certificates after establishing connection
    https_server.verify = [](SSL *ssl) {
      crypto::x509_t x509 {
#if OPENSSL_VERSION_MAJOR >= 3
        SSL_get1_peer_certificate(ssl)
#else
        SSL_get_peer_certificate(ssl)
#endif
      };
      if (!x509) {
        BOOST_LOG(info) << "unknown -- denied"sv;
        return 0;
      }

      int verified = 0;

      auto fg = util::fail_guard([&]() {
        char subject_name[256];

        X509_NAME_oneline(X509_get_subject_name(x509.get()), subject_name, sizeof(subject_name));

        BOOST_LOG(debug) << subject_name << " -- "sv << (verified ? "verified"sv : "denied"sv);
      });

      std::lock_guard lock {client_auth_mutex()};
      auto err_str = verify_client_certificate(x509.get());
      if (err_str) {
        BOOST_LOG(warning) << "SSL Verification error :: "sv << err_str;

        return verified;
      }

      // Check if this client is enabled
      auto pem = crypto::pem(x509);
      auto [enabled, client_name] = get_client_status(pem);
      if (!enabled) {
        BOOST_LOG(info) << "Client is disabled -- denied"sv;
        return verified;
      }

      last_verified_client_cert = pem;
      last_verified_client_name = client_name;
      verified = 1;

      return verified;
    };

    https_server.on_verify_failed = [](resp_https_t resp, req_https_t req) {
      pt::ptree tree;
      auto g = util::fail_guard([&]() {
        std::ostringstream data;

        pt::write_xml(data, tree);
        resp->write(data.str());
        resp->close_connection_after_response = true;
      });

      tree.put("root.<xmlattr>.status_code"s, 401);
      tree.put("root.<xmlattr>.query"s, req->path);
      tree.put("root.<xmlattr>.status_message"s, "The client is not authorized. Certificate verification failed."s);
    };

#ifdef AURORA_SUNSHINE_HOST
    const auto authorized = [&https_server](auto handler) {
      return [&https_server, handler](resp_https_t response, req_https_t request) {
        if (!aurora_verified_peer(https_server, *request)) {
          https_server.on_verify_failed(response, request);
          return;
        }
        handler(response, request);
      };
    };
#else
    const auto authorized = [](auto handler) { return handler; };
#endif
    https_server.default_resource["GET"] = not_found<SunshineHTTPS>;
    https_server.resource["^/serverinfo$"]["GET"] = authorized(serverinfo<SunshineHTTPS>);
    https_server.resource["^/pair$"]["GET"] = authorized([](auto resp, auto req) {
      pair<SunshineHTTPS>(resp, req);
    });
    https_server.resource["^/applist$"]["GET"] = authorized(applist);
    https_server.resource["^/appasset$"]["GET"] = authorized(appasset);
#if defined(_WIN32) && defined(AURORA_SUNSHINE_HOST)
    https_server.resource["^/aurora-session-privacy$"]["POST"] = [&https_server](resp_https_t resp, req_https_t req) {
      aurora_session_privacy(https_server, resp, req);
    };
    https_server.resource["^/aurora-session-privacy$"]["GET"] = [](resp_https_t resp, req_https_t) {
      aurora_text_response(resp, SimpleWeb::StatusCode::client_error_method_not_allowed, "POST required", {{"Allow", "POST"}});
    };
    https_server.resource["^/aurora-video-bitrate-v2$"]["POST"] = [&https_server](resp_https_t resp, req_https_t req) {
      aurora_video_bitrate(https_server, resp, req);
    };
    https_server.resource["^/aurora-video-bitrate-v2$"]["GET"] = [](resp_https_t resp, req_https_t req) {
      aurora_text_response(resp, SimpleWeb::StatusCode::client_error_method_not_allowed, "unavailable", {{"Allow", "POST"}});
    };
#endif
#if defined(AURORA_SUNSHINE_HOST) && (defined(_WIN32) || defined(__APPLE__))
    https_server.resource["^/aurora-power$"]["POST"] = authorized(aurora_power);
    https_server.resource["^/aurora-power$"]["GET"] = [](resp_https_t resp, req_https_t) {
      BOOST_LOG(warning) << "Aurora power request rejected: POST required";
      aurora_text_response(resp, power_status::client_error_method_not_allowed, "POST required", {{"Allow", "POST"}});
    };
#endif
#ifdef AURORA_SUNSHINE_HOST
    https_server.resource["^/aurora-wallpaper$"]["GET"] = authorized(aurora_wallpaper);
#endif
    https_server.resource["^/launch$"]["GET"] = [&host_audio, &https_server](auto resp, auto req) {
#ifdef AURORA_SUNSHINE_HOST
      const auto identity = aurora_verified_peer(https_server, *req);
      if (!identity) {
        https_server.on_verify_failed(resp, req);
        return;
      }
      launch(host_audio, resp, req, &*identity);
#else
      launch(host_audio, resp, req);
#endif
    };
    https_server.resource["^/resume$"]["GET"] = [&host_audio, &https_server](auto resp, auto req) {
#ifdef AURORA_SUNSHINE_HOST
      const auto identity = aurora_verified_peer(https_server, *req);
      if (!identity) {
        https_server.on_verify_failed(resp, req);
        return;
      }
      resume(host_audio, resp, req, &*identity);
#else
      resume(host_audio, resp, req);
#endif
    };
    https_server.resource["^/cancel$"]["GET"] = authorized(cancel);

    https_server.config.reuse_address = true;
    https_server.config.address = net::get_bind_address(address_family);
    https_server.config.port = port_https;

    http_server.default_resource["GET"] = not_found<SimpleWeb::HTTP>;
    http_server.resource["^/serverinfo$"]["GET"] = serverinfo<SimpleWeb::HTTP>;
    http_server.resource["^/pair$"]["GET"] = [](auto resp, auto req) {
      pair<SimpleWeb::HTTP>(resp, req);
    };

    http_server.config.reuse_address = true;
    http_server.config.address = net::get_bind_address(address_family);
    http_server.config.port = port_http;

    auto accept_and_run = [&](auto *http_server) {
      try {
        std::string name = "nvhttp::" + std::to_string(http_server->config.port);
        platf::set_thread_name(name);
        http_server->start();
      } catch (boost::system::system_error &err) {
        // It's possible the exception gets thrown after calling http_server->stop() from a different thread
        if (shutdown_event->peek()) {
          return;
        }

        BOOST_LOG(fatal) << "Couldn't start http server on ports ["sv << port_https << ", "sv << port_https << "]: "sv << err.what();
        shutdown_event->raise(true);
        return;
      }
    };
    std::jthread ssl {accept_and_run, &https_server};
    std::jthread tcp {accept_and_run, &http_server};

    // Wait for any event
    shutdown_event->view();

    https_server.stop();
    http_server.stop();

    ssl.join();
    tcp.join();
  }

  bool erase_all_clients() {
    std::lock_guard lock {client_auth_mutex()};
    std::erase_if(client_root.named_devices, [](const named_cert_t &entry) { return entry.tailnet_scope.empty(); });
    for (auto &entry : client_root.named_devices) {
      entry.enabled = false;
      entry.unpaired = true;
      entry.tailnet_until = {};
    }
    cert_chain.clear();
    return save_state();
  }

  bool unpair_client(const std::string_view uuid) {
    std::lock_guard lock {client_auth_mutex()};
    bool removed = false;
    for (auto it = client_root.named_devices.begin(); it != client_root.named_devices.end();) {
      if (it->uuid != uuid || it->unpaired) {
        ++it;
        continue;
      }
      removed = true;
      if (it->tailnet_scope.empty()) {
        it = client_root.named_devices.erase(it);
        continue;
      }
      // Keep the node and certificate binding as a revocation record.
      it->enabled = false;
      it->unpaired = true;
      it->tailnet_until = {};
      ++it;
    }

    rebuild_client_cert_chain();
    return removed && save_state();
  }

  bool set_client_enabled(const std::string_view uuid, bool enabled) {
    std::lock_guard lock {client_auth_mutex()};
    for (auto &named_cert : client_root.named_devices) {
      if (named_cert.uuid == uuid && !named_cert.unpaired) {
        const bool previous = named_cert.enabled;
        named_cert.enabled = enabled;
        if (!enabled) {
          named_cert.tailnet_until = {};
        }
        const bool saved = save_state();
        if (!saved && enabled) {
          named_cert.enabled = previous;
        }
        rebuild_client_cert_chain();
        return saved;
      }
    }
    return false;
  }

  /**
   * @brief Get cert by UUID.
   */
  std::string get_cert_by_uuid(const std::string_view uuid) {
    std::lock_guard lock {client_auth_mutex()};
    for (const auto &named_cert : client_root.named_devices) {
      if (named_cert.uuid == uuid && !named_cert.unpaired) {
        return named_cert.cert;
      }
    }
    return {};
  }

  /**
   * @brief Check whether a paired client certificate is allowed to connect and return its friendly name.
   */
  std::pair<bool, std::string> get_client_status(const std::string_view cert_pem) {
    const client_t &client = client_root;
    for (const auto &named_cert : client.named_devices) {
      if (named_cert.cert == cert_pem) {
        return {named_cert.authorized(), named_cert.name};
      }
    }
    return {true, {}};
  }

#ifdef SUNSHINE_TESTS
  namespace test_support {
    void pair_http(
      std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Response> response,
      std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Request> request
    ) {
      pair<SimpleWeb::HTTP>(std::move(response), std::move(request));
    }

    void reset_client_state() {
      std::lock_guard lock {client_auth_mutex()};
      client_root = {};
      cert_chain.clear();
    }

    std::string add_client(const std::string &name, std::string cert, bool enabled) {
      auto uuid = add_authorized_client(name, std::move(cert));
      if (!uuid.empty() && !enabled) {
        set_client_enabled(uuid, false);
      }
      return uuid;
    }

    bool authorize_client_certificate(const std::string_view cert) {
      auto certificate = crypto::x509(cert);
      if (!certificate) {
        return false;
      }

      std::lock_guard lock {client_auth_mutex()};
      return verify_client_certificate(certificate.get()) == nullptr;
    }

    bool complete_pairing(const std::string_view pairing_id, const bool success) {
      std::scoped_lock lock {map_id_sess_mutex()};
      const auto sess_it = std::ranges::find_if(map_id_sess, [&](const auto &entry) {
        return entry.second.async_insert_pin.id == pairing_id;
      });
      if (sess_it == map_id_sess.end()) {
        return false;
      }

      nvhttp::complete_pairing(sess_it->second, success);
      map_id_sess.erase(sess_it);
      return true;
    }

    void reload_client_state() {
      load_state();
    }
  }  // namespace test_support
#endif
}  // namespace nvhttp
