// Pinned peers must connect regardless of certificate validity dates: trust is
// the exact certificate the user verified, and device clocks disagree.
// Certificates are generated in memory; no files, network or system identities.
#include "hop/secure_channel.h"
#include "hop/tls_identity.h"
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <string>

namespace {
int checks = 0;
void check(bool value, const char* message) { ++checks; if (!value) { std::cerr << "FAIL " << message << '\n'; std::exit(1); } }
struct Identity { std::string certificate, key; };
Identity make(const char* not_before, const char* not_after, const char* name) {
    mbedtls_entropy_context entropy; mbedtls_ctr_drbg_context rng; mbedtls_pk_context key; mbedtls_x509write_cert cert;
    mbedtls_entropy_init(&entropy); mbedtls_ctr_drbg_init(&rng); mbedtls_pk_init(&key); mbedtls_x509write_crt_init(&cert);
    std::array<unsigned char, 8192> key_pem{}, cert_pem{};
    const unsigned char serial[] = {1};
    int rc = mbedtls_ctr_drbg_seed(&rng, mbedtls_entropy_func, &entropy, nullptr, 0);
    if (!rc) rc = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (!rc) rc = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), mbedtls_ctr_drbg_random, &rng);
    mbedtls_x509write_crt_set_version(&cert, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&cert, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&cert, &key);
    mbedtls_x509write_crt_set_issuer_key(&cert, &key);
    if (!rc) rc = mbedtls_x509write_crt_set_serial_raw(&cert, const_cast<unsigned char*>(serial), sizeof(serial));
    if (!rc) rc = mbedtls_x509write_crt_set_subject_name(&cert, name);
    if (!rc) rc = mbedtls_x509write_crt_set_issuer_name(&cert, name);
    if (!rc) rc = mbedtls_x509write_crt_set_validity(&cert, not_before, not_after);
    if (!rc) rc = mbedtls_x509write_crt_set_basic_constraints(&cert, 1, -1);
    if (!rc) rc = mbedtls_x509write_crt_set_key_usage(&cert, MBEDTLS_X509_KU_DIGITAL_SIGNATURE | MBEDTLS_X509_KU_KEY_CERT_SIGN);
    if (!rc) rc = mbedtls_pk_write_key_pem(&key, key_pem.data(), key_pem.size());
    if (!rc) rc = mbedtls_x509write_crt_pem(&cert, cert_pem.data(), cert_pem.size(), mbedtls_ctr_drbg_random, &rng);
    Identity out;
    if (!rc) { out.certificate = reinterpret_cast<const char*>(cert_pem.data()); out.key = reinterpret_cast<const char*>(key_pem.data()); }
    mbedtls_x509write_crt_free(&cert); mbedtls_pk_free(&key); mbedtls_ctr_drbg_free(&rng); mbedtls_entropy_free(&entropy);
    check(rc == 0 && hop::validate_tls_identity(out.certificate, out.key), "generate a test identity with explicit validity dates");
    return out;
}
struct Endpoint { std::deque<unsigned char> incoming; Endpoint* other = nullptr; };
int send(void* ctx, const unsigned char* data, std::size_t size) {
    auto* e = static_cast<Endpoint*>(ctx);
    for (std::size_t i = 0; i < size; ++i) e->other->incoming.push_back(data[i]);
    return int(size);
}
int receive(void* ctx, unsigned char* data, std::size_t size) {
    auto* e = static_cast<Endpoint*>(ctx); if (e->incoming.empty()) return MBEDTLS_ERR_SSL_WANT_READ;
    const auto n = std::min(size, e->incoming.size());
    for (std::size_t i = 0; i < n; ++i) { data[i] = e->incoming.front(); e->incoming.pop_front(); }
    return int(n);
}
bool connects(const Identity& client_id, const Identity& server_id, const std::string& client_pin, const std::string& server_pin) {
    Endpoint a, b; a.other = &b; b.other = &a;
    hop::SecureChannel client(hop::TlsRole::client, {client_id.certificate, client_id.key, client_pin}, &a, send, receive);
    hop::SecureChannel server(hop::TlsRole::server, {server_id.certificate, server_id.key, server_pin}, &b, send, receive);
    for (int i = 0; i < 20000 && (!client.authenticated() || !server.authenticated()); ++i) {
        if (client.handshake() == hop::TlsProgress::failed || server.handshake() == hop::TlsProgress::failed) return false;
    }
    if (!client.authenticated() || !server.authenticated()) return false;
    const std::uint8_t ping[] = {'h', 'o', 'p'}; std::uint8_t pong[3]{};
    return client.write(ping, sizeof(ping)) == 3 && server.read(pong, sizeof(pong)) == 3 && std::equal(ping, ping + 3, pong);
}
}
int main() {
    const auto current = make("20200101000000", "20991231235959", "CN=Hop test current");
    const auto future = make("20900101000000", "20991231235959", "CN=Hop test not yet valid");
    const auto expired = make("20000101000000", "20010101000000", "CN=Hop test expired");
    const auto stranger = make("20200101000000", "20991231235959", "CN=Hop test stranger");
    check(connects(current, future, future.certificate, current.certificate), "a pinned certificate dated in the future connects");
    check(connects(future, current, current.certificate, future.certificate), "a future-dated client connects to its pinned peer");
    check(connects(current, expired, expired.certificate, current.certificate), "a pinned certificate past its end date connects");
    check(connects(expired, future, future.certificate, expired.certificate), "two out-of-range pinned certificates connect");
    // Ignoring dates must not relax the identity check.
    check(!connects(current, future, stranger.certificate, current.certificate), "a future-dated certificate that is not the pin is rejected");
    check(!connects(expired, current, current.certificate, stranger.certificate), "an expired certificate that is not the pin is rejected");
    check(!connects(stranger, current, current.certificate, future.certificate), "a valid certificate that is not the pin is rejected");
    hop::GeneratedTlsIdentity generated;
    check(hop::generate_tls_identity(generated) && hop::validate_tls_identity(generated.certificate_pem, generated.private_key_pem), "generated identity is usable");
    std::cout << "PASS " << checks << " pinned TLS checks independent of certificate validity dates\n";
}
