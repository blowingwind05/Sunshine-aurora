/**
 * @file tests/unit/test_nvhttp_client_auth.cpp
 * @brief Test exact paired-client certificate authorization and persistence.
 */

#include "../certificate_test_utils.h"
#include "../tests_common.h"

// standard includes
#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <thread>
#include <vector>

// local includes
#include <src/config.h>
#include <src/nvhttp.h>
#ifdef AURORA_SUNSHINE_HOST
  #include <src/aurora_client_proof.h>
#endif

namespace fs = std::filesystem;

/**
 * @brief Isolate paired-client authorization tests from the user's Sunshine state.
 */
class ClientAuthorizationTest: public BaseTest {
protected:
  /**
   * @brief Redirect persisted client state to the test build directory.
   */
  void SetUp() override {
    BaseTest::SetUp();
    original_state_file = config::nvhttp.file_state;
    original_fresh_state = config::sunshine.flags[config::flag::FRESH_STATE];
    state_file = fs::path {SUNSHINE_TEST_BIN_DIR} / "client_authorization_state.json";

    config::nvhttp.file_state = state_file.string();
    config::sunshine.flags[config::flag::FRESH_STATE] = false;
    nvhttp::test_support::reset_client_state();

    std::error_code remove_error;
    fs::remove(state_file, remove_error);
  }

  /**
   * @brief Remove test state and restore the caller's configuration.
   */
  void TearDown() override {
    nvhttp::test_support::reset_client_state();
    std::error_code remove_error;
    fs::remove(state_file, remove_error);

    config::nvhttp.file_state = original_state_file;
    config::sunshine.flags[config::flag::FRESH_STATE] = original_fresh_state;
    BaseTest::TearDown();
  }

private:
  fs::path state_file;  ///< Task-specific persisted state fixture.
  std::string original_state_file;  ///< State-file setting restored after each test.
  bool original_fresh_state;  ///< Fresh-state flag restored after each test.
};

TEST_F(ClientAuthorizationTest, CanonicalIdentityFailsClosedAndTracksEnableState) {
  const auto paired_credentials = test_utils::certificates::generate_ca_credentials();
  const auto crlf_certificate = test_utils::certificates::to_crlf_pem(paired_credentials.x509);
  const auto unknown_credentials = crypto::gen_creds("Sunshine Unknown Client", 2048);
  const auto uuid = nvhttp::test_support::add_client("paired", crlf_certificate, true);

  ASSERT_FALSE(uuid.empty());
  EXPECT_EQ(nvhttp::get_cert_by_uuid(uuid), paired_credentials.x509);
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(crlf_certificate));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(unknown_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate("not a certificate"));

  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, false));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, true));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
}

TEST_F(ClientAuthorizationTest, DerivedLeafDoesNotInheritPairedAuthorization) {
  const auto paired_credentials = test_utils::certificates::generate_ca_credentials();
  const auto derived_credentials = test_utils::certificates::generate_derived_leaf(paired_credentials);
  const auto uuid = nvhttp::test_support::add_client("paired issuer", paired_credentials.x509, true);

  ASSERT_FALSE(uuid.empty());
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(derived_credentials.x509));

  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, false));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(derived_credentials.x509));
}

TEST_F(ClientAuthorizationTest, MultipleClientsPersistAndUnpairIndependently) {
  const auto enabled_credentials = test_utils::certificates::generate_ca_credentials("Sunshine Enabled Client");
  const auto disabled_credentials = crypto::gen_creds("Sunshine Disabled Client", 2048);
  const auto expired_credentials = test_utils::certificates::expire_credentials(
    test_utils::certificates::generate_ca_credentials("Sunshine Expired Client")
  );

  const auto enabled_uuid = nvhttp::test_support::add_client("enabled", enabled_credentials.x509, true);
  const auto disabled_uuid = nvhttp::test_support::add_client("disabled", disabled_credentials.x509, false);
  const auto expired_uuid = nvhttp::test_support::add_client("expired", expired_credentials.x509, true);
  ASSERT_FALSE(enabled_uuid.empty());
  ASSERT_FALSE(disabled_uuid.empty());
  ASSERT_FALSE(expired_uuid.empty());
  EXPECT_EQ(nvhttp::get_all_clients().size(), 3);

  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(enabled_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(disabled_credentials.x509));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(expired_credentials.x509));

  nvhttp::test_support::reset_client_state();
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(enabled_credentials.x509));
  nvhttp::test_support::reload_client_state();

  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(enabled_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(disabled_credentials.x509));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(expired_credentials.x509));

  ASSERT_TRUE(nvhttp::set_client_enabled(disabled_uuid, true));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(disabled_credentials.x509));
  ASSERT_TRUE(nvhttp::unpair_client(enabled_uuid));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(enabled_credentials.x509));

  nvhttp::erase_all_clients();
  nvhttp::test_support::reset_client_state();
  nvhttp::test_support::reload_client_state();
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(disabled_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(expired_credentials.x509));
}

TEST_F(ClientAuthorizationTest, DuplicateCertificateIdentityFailsClosed) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  ASSERT_FALSE(nvhttp::test_support::add_client("first", credentials.x509, true).empty());
  ASSERT_FALSE(nvhttp::test_support::add_client("second", credentials.x509, true).empty());

  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(credentials.x509));
}

#ifdef AURORA_SUNSHINE_HOST
namespace {
  std::string fixture_der(const std::string &certificate) {
    const auto x509 = crypto::x509(certificate);
    const auto length = i2d_X509(x509.get(), nullptr);
    std::string der(length, '\0');
    auto *cursor = reinterpret_cast<unsigned char *>(der.data());
    i2d_X509(x509.get(), &cursor);
    return der;
  }

  std::string proof_hex(const auto &bytes) {
    auto encoded = util::hex_vec(bytes, true);
    std::ranges::transform(encoded, encoded.begin(), [](unsigned char digit) {
      return static_cast<char>(std::tolower(digit));
    });
    return encoded;
  }

  nvhttp::tailnet_grant_result grant_fixture(const crypto::creds_t &client, const crypto::creds_t &host,
    unsigned char scope, unsigned char nonce, bool corrupt_signature = false) {
    using namespace Aurora::ClientProof;
    Challenge challenge {};
    std::fill_n(challenge.begin(), DigestSize, nonce);
    std::ranges::copy(crypto::hash(fixture_der(host.x509)), challenge.begin() + DigestSize);
    std::fill_n(challenge.begin() + 2 * DigestSize, DigestSize, scope);
    auto der = fixture_der(client.x509);
    const auto message = transcript(challenge, crypto::hash(der));
    auto signature = crypto::sign256(crypto::pkey(client.pkey),
      {reinterpret_cast<const char *>(message.data()), message.size()});
    if (corrupt_signature && !signature.empty()) {
      signature.back() ^= 1;
    }
    return nvhttp::grant_tailnet_client("Tailnet fixture", proof_hex(challenge), proof_hex(der), proof_hex(signature));
  }
}  // namespace

TEST_F(ClientAuthorizationTest, TailnetGrantBindsOneScopeAndRequiresRenewalAfterRestart) {
  using enum nvhttp::tailnet_grant_result;
  const auto host = crypto::gen_creds("Aurora Host", 2048);
  const auto other_host = crypto::gen_creds("Other Host", 2048);
  const auto client = crypto::gen_creds("Aurora Tailnet Client", 2048);
  const auto other_client = crypto::gen_creds("Other Tailnet Client", 2048);
  nvhttp::setup(host.pkey, host.x509);
  EXPECT_EQ(grant_fixture(client, other_host, 1, 1), invalid_proof);
  EXPECT_EQ(grant_fixture(client, host, 1, 1, true), invalid_proof);
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(client.x509));

  ASSERT_EQ(grant_fixture(client, host, 1, 2), accepted);
  ASSERT_TRUE(nvhttp::test_support::authorize_client_certificate(client.x509));
  EXPECT_EQ(grant_fixture(client, host, 2, 3), conflict);
  EXPECT_EQ(grant_fixture(other_client, host, 1, 4), conflict);

  nvhttp::test_support::reset_client_state();
  nvhttp::test_support::reload_client_state();
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(client.x509));
  ASSERT_EQ(grant_fixture(client, host, 1, 5), accepted);
  ASSERT_TRUE(nvhttp::test_support::authorize_client_certificate(client.x509));
  ASSERT_EQ(nvhttp::get_all_clients().size(), 1);
}

TEST_F(ClientAuthorizationTest, TailnetGrantCannotResurrectDisabledOrUnpairedDevice) {
  using enum nvhttp::tailnet_grant_result;
  const auto host = crypto::gen_creds("Aurora Host", 2048);
  const auto client = crypto::gen_creds("Aurora Tailnet Client", 2048);
  nvhttp::setup(host.pkey, host.x509);
  ASSERT_EQ(grant_fixture(client, host, 1, 1), accepted);
  const auto id = nvhttp::get_all_clients().at(0).at("uuid").get<std::string>();
  ASSERT_TRUE(nvhttp::set_client_enabled(id, false));
  EXPECT_EQ(grant_fixture(client, host, 1, 2), revoked);
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(client.x509));

  ASSERT_TRUE(nvhttp::set_client_enabled(id, true));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(client.x509));
  ASSERT_EQ(grant_fixture(client, host, 1, 3), accepted);
  ASSERT_TRUE(nvhttp::unpair_client(id));
  EXPECT_TRUE(nvhttp::get_all_clients().empty());

  nvhttp::test_support::reset_client_state();
  nvhttp::test_support::reload_client_state();
  EXPECT_EQ(grant_fixture(client, host, 1, 4), revoked);
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(client.x509));
}
#endif

TEST_F(ClientAuthorizationTest, ConcurrentStateChangesRemainConsistent) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto uuid = nvhttp::test_support::add_client("concurrent", credentials.x509, true);
  ASSERT_FALSE(uuid.empty());

  std::atomic_bool operations_succeeded {true};
  std::vector<std::jthread> workers;
  for (std::size_t worker = 0; worker < 4; ++worker) {
    workers.emplace_back([&operations_succeeded, uuid, certificate = credentials.x509, worker]() {
      for (std::size_t iteration = 0; iteration < 8; ++iteration) {
        if (!nvhttp::set_client_enabled(uuid, (worker + iteration) % 2 == 0)) {
          operations_succeeded = false;
        }
        static_cast<void>(nvhttp::test_support::authorize_client_certificate(certificate));
      }
    });
  }
  workers.clear();

  EXPECT_TRUE(operations_succeeded);
  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, false));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(credentials.x509));
}
