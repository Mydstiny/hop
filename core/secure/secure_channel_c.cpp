#include "hop/secure_channel_c.h"
#include "hop/secure_channel.h"
#include "hop/tls_identity.h"
#include <mbedtls/platform_util.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ssl.h>
#include <memory>
#include <string>
#include <cstring>

struct hop_tls_channel { std::unique_ptr<hop::SecureChannel> value; };
extern "C" int hop_tls_want_read_error(void) { return MBEDTLS_ERR_SSL_WANT_READ; }
extern "C" int hop_tls_want_write_error(void) { return MBEDTLS_ERR_SSL_WANT_WRITE; }
namespace {
struct ScopedIdentity {
    hop::TlsIdentity value;
    ~ScopedIdentity() { if (!value.private_key_pem.empty()) mbedtls_platform_zeroize(&value.private_key_pem[0], value.private_key_pem.size()); }
};
struct ScopedGenerated {
    hop::GeneratedTlsIdentity value;
    ~ScopedGenerated() { if (!value.private_key_pem.empty()) mbedtls_platform_zeroize(&value.private_key_pem[0], value.private_key_pem.size()); }
};
}
extern "C" int hop_tls_random_bytes(uint8_t* output, size_t size) {
    if (!output || !size || size>32) return -1;
    mbedtls_entropy_context entropy; mbedtls_ctr_drbg_context rng;
    mbedtls_entropy_init(&entropy); mbedtls_ctr_drbg_init(&rng);
    constexpr char label[]="Hop pairing nonce";
    int rc=mbedtls_ctr_drbg_seed(&rng,mbedtls_entropy_func,&entropy,reinterpret_cast<const unsigned char*>(label),sizeof(label)-1);
    if (!rc) rc=mbedtls_ctr_drbg_random(&rng,output,size);
    mbedtls_ctr_drbg_free(&rng); mbedtls_entropy_free(&entropy);
    if (rc) { mbedtls_platform_zeroize(output,size); return -1; }
    return 0;
}
extern "C" int hop_tls_generate_identity(char* cert, size_t certSize, char* key, size_t keySize, char* fingerprint, size_t fingerprintSize) {
    if (!cert || !key || !fingerprint || !certSize || !keySize || !fingerprintSize) return -1;
    mbedtls_platform_zeroize(key,keySize);
    cert[0] = fingerprint[0] = '\0';
    try {
        ScopedGenerated generated;
        if (!hop::generate_tls_identity(generated.value) || generated.value.certificate_pem.size()+1>certSize ||
            generated.value.private_key_pem.size()+1>keySize || generated.value.fingerprint_sha256.size()+1>fingerprintSize) return -1;
        std::memcpy(cert,generated.value.certificate_pem.c_str(),generated.value.certificate_pem.size()+1);
        std::memcpy(key,generated.value.private_key_pem.c_str(),generated.value.private_key_pem.size()+1);
        std::memcpy(fingerprint,generated.value.fingerprint_sha256.c_str(),generated.value.fingerprint_sha256.size()+1);
        return 0;
    } catch (...) { mbedtls_platform_zeroize(key,keySize); cert[0]=fingerprint[0]='\0'; return -1; }
}
extern "C" int hop_tls_validate_identity(const char* cert, size_t certSize, const char* key, size_t keySize) {
    if (!cert || !key || !certSize || !keySize || certSize>65536 || keySize>65536) return -1;
    try {
        ScopedIdentity identity{{std::string(cert,certSize),std::string(key,keySize),{}}};
        return hop::validate_tls_identity(identity.value.certificate_pem,identity.value.private_key_pem) ? 0 : -1;
    } catch (...) { return -1; }
}
extern "C" int hop_tls_certificate_fingerprint(const char* cert, size_t certSize, char* fingerprint, size_t fingerprintSize) {
    if (!fingerprint || !fingerprintSize) return -1;
    fingerprint[0] = '\0';
    if (!cert || !certSize || certSize > 65536 || fingerprintSize < 65) return -1;
    try {
        std::string hex;
        if (!hop::tls_certificate_fingerprint(std::string(cert, certSize), hex) || hex.size() != 64) return -1;
        std::memcpy(fingerprint, hex.c_str(), 65);
        return 0;
    } catch (...) { fingerprint[0] = '\0'; return -1; }
}
extern "C" hop_tls_channel* hop_tls_create(int client, const char* cert, const char* key, const char* peer,
                                            void* io, hop_tls_send_fn send, hop_tls_recv_fn recv) {
    if ((client != 0 && client != 1) || !cert || !key || !peer || !io || !send || !recv) return nullptr;
    try {
        ScopedIdentity identity{{cert, key, peer}};
        auto result = std::make_unique<hop_tls_channel>();
        result->value = std::make_unique<hop::SecureChannel>(client ? hop::TlsRole::client : hop::TlsRole::server,
                                                             identity.value, io, send, recv);
        if (result->value->error()) return nullptr;
        return result.release();
    } catch (...) { return nullptr; }
}
extern "C" int hop_tls_handshake(hop_tls_channel* channel) {
    if (!channel || !channel->value) return -1;
    try {
        switch (channel->value->handshake()) {
        case hop::TlsProgress::ready: return 1;
        case hop::TlsProgress::again: return 0;
        case hop::TlsProgress::failed: return -1;
        }
    } catch (...) {}
    return -1;
}
extern "C" int hop_tls_read(hop_tls_channel* channel, uint8_t* data, size_t size) {
    if (!channel || !channel->value) return -1;
    try { return channel->value->read(data,size); } catch (...) { return -1; }
}
extern "C" int hop_tls_write(hop_tls_channel* channel, const uint8_t* data, size_t size) {
    if (!channel || !channel->value) return -1;
    try { return channel->value->write(data,size); } catch (...) { return -1; }
}
extern "C" int hop_tls_error(const hop_tls_channel* channel) {
    return channel && channel->value ? channel->value->error() : -1;
}
extern "C" int hop_tls_io_want(const hop_tls_channel* channel) { return channel && channel->value ? channel->value->io_want() : 0; }
extern "C" void hop_tls_free(hop_tls_channel* channel) { delete channel; }
