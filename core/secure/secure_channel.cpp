#include "hop/secure_channel.h"
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <algorithm>
#include <cstring>

namespace hop {
struct SecureChannel::Impl {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context rng;
    mbedtls_x509_crt identity, peer;
    mbedtls_pk_context key;
    mbedtls_ssl_config config;
    mbedtls_ssl_context ssl;
    bool ready = false;
    int failure = 0, want = 0;
    Impl() {
        mbedtls_entropy_init(&entropy); mbedtls_ctr_drbg_init(&rng);
        mbedtls_x509_crt_init(&identity); mbedtls_x509_crt_init(&peer);
        mbedtls_pk_init(&key); mbedtls_ssl_config_init(&config); mbedtls_ssl_init(&ssl);
    }
    ~Impl() {
        mbedtls_ssl_free(&ssl); mbedtls_ssl_config_free(&config);
        mbedtls_pk_free(&key); mbedtls_x509_crt_free(&peer); mbedtls_x509_crt_free(&identity);
        mbedtls_ctr_drbg_free(&rng); mbedtls_entropy_free(&entropy);
    }
};
SecureChannel::SecureChannel(TlsRole role, const TlsIdentity& id, void* io, TlsSend send, TlsRecv recv)
    : impl_(new Impl) {
    auto& s = *impl_;
    if (!io || !send || !recv || id.certificate_pem.empty() || id.private_key_pem.empty() || id.trusted_peer_certificate_pem.empty() ||
        id.certificate_pem.size() > 65536 || id.private_key_pem.size() > 65536 || id.trusted_peer_certificate_pem.size() > 65536 ||
        id.certificate_pem.find('\0') != std::string::npos || id.private_key_pem.find('\0') != std::string::npos ||
        id.trusted_peer_certificate_pem.find('\0') != std::string::npos) {
        s.failure = MBEDTLS_ERR_SSL_BAD_INPUT_DATA; return;
    }
    constexpr char personalization[] = "Hop authenticated TLS v2";
    int rc = mbedtls_ctr_drbg_seed(&s.rng, mbedtls_entropy_func, &s.entropy,
                                   reinterpret_cast<const unsigned char*>(personalization), sizeof(personalization) - 1);
    if (!rc) rc = mbedtls_x509_crt_parse(&s.identity, reinterpret_cast<const unsigned char*>(id.certificate_pem.c_str()), id.certificate_pem.size() + 1);
    if (!rc) rc = mbedtls_x509_crt_parse(&s.peer, reinterpret_cast<const unsigned char*>(id.trusted_peer_certificate_pem.c_str()), id.trusted_peer_certificate_pem.size() + 1);
    if (!rc && (s.identity.next || s.peer.next)) rc = MBEDTLS_ERR_X509_BAD_INPUT_DATA;
    if (!rc) rc = mbedtls_pk_parse_key(&s.key, reinterpret_cast<const unsigned char*>(id.private_key_pem.c_str()), id.private_key_pem.size() + 1,
                                       nullptr, 0, mbedtls_ctr_drbg_random, &s.rng);
    if (!rc) rc = mbedtls_pk_check_pair(&s.identity.pk, &s.key, mbedtls_ctr_drbg_random, &s.rng);
    if (!rc) rc = mbedtls_ssl_config_defaults(&s.config, role == TlsRole::client ? MBEDTLS_SSL_IS_CLIENT : MBEDTLS_SSL_IS_SERVER,
                                               MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc) { s.failure = rc; return; }
    mbedtls_ssl_conf_authmode(&s.config, MBEDTLS_SSL_VERIFY_REQUIRED);
    // Do not silently downgrade the new channel to older protocol versions.
    mbedtls_ssl_conf_min_tls_version(&s.config, MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_rng(&s.config, mbedtls_ctr_drbg_random, &s.rng);
    mbedtls_ssl_conf_ca_chain(&s.config, &s.peer, nullptr);
    rc = mbedtls_ssl_conf_own_cert(&s.config, &s.identity, &s.key);
    if (!rc) rc = mbedtls_ssl_setup(&s.ssl, &s.config);
    // Peer identity is the separately verified exact certificate pin, not an
    // unauthenticated DNS name. Calling with nullptr explicitly disables SNI.
    if (!rc && role == TlsRole::client) rc = mbedtls_ssl_set_hostname(&s.ssl, nullptr);
    if (rc) { s.failure = rc; return; }
    mbedtls_ssl_set_bio(&s.ssl, io, send, recv, nullptr);
}
SecureChannel::~SecureChannel() = default;
TlsProgress SecureChannel::handshake() {
    auto& s = *impl_;
    if (s.failure) return TlsProgress::failed;
    if (s.ready) return TlsProgress::ready;
    int rc = mbedtls_ssl_handshake(&s.ssl);
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) { s.want=rc; return TlsProgress::again; }
    s.want=0;
    if (rc) { s.failure = rc; return TlsProgress::failed; }
    const auto* actual = mbedtls_ssl_get_peer_cert(&s.ssl);
    if (mbedtls_ssl_get_version_number(&s.ssl) != MBEDTLS_SSL_VERSION_TLS1_3 ||
        mbedtls_ssl_get_verify_result(&s.ssl) != 0 || !actual || !actual->raw.p ||
        actual->raw.len != s.peer.raw.len || std::memcmp(actual->raw.p, s.peer.raw.p, s.peer.raw.len) != 0) {
        s.failure = MBEDTLS_ERR_X509_CERT_VERIFY_FAILED; return TlsProgress::failed;
    }
    s.ready = true;
    return TlsProgress::ready;
}
int SecureChannel::write(const std::uint8_t* data, std::size_t length) {
    if (!impl_->ready || (!data && length)) return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
    if (!length) return 0;
    const int rc = mbedtls_ssl_write(&impl_->ssl, data, length);
    impl_->want = (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) ? rc : 0;
    if (rc < 0 && rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
        impl_->failure = rc; impl_->ready = false;
    }
    return rc;
}
int SecureChannel::read(std::uint8_t* data, std::size_t length) {
    if (!impl_->ready || (!data && length)) return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
    if (!length) return 0;
    const int rc = mbedtls_ssl_read(&impl_->ssl, data, length);
    impl_->want = (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) ? rc : 0;
    if (rc == 0 || (rc < 0 && rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE)) {
        impl_->failure = rc == 0 ? MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY : rc; impl_->ready = false;
    }
    return rc;
}
bool SecureChannel::authenticated() const { return impl_->ready && !impl_->failure; }
int SecureChannel::error() const { return impl_->failure; }
int SecureChannel::io_want() const { return impl_->want; }
} // namespace hop
