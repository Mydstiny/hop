#pragma once
#include <string>

namespace hop {
struct GeneratedTlsIdentity {
    std::string certificate_pem;
    std::string private_key_pem;
    std::string fingerprint_sha256;
};
// Generates an independent P-256 self-signed identity using the system entropy
// source. The caller must persist the private key in platform secure storage,
// protect it, and clear its buffers. Peer fingerprint verification is separate.
bool generate_tls_identity(GeneratedTlsIdentity& output);
// Verifies that one PEM certificate and its private key are a matching pair.
// Does not assess user authorization or certificate expiration.
bool validate_tls_identity(const std::string& certificate_pem, const std::string& private_key_pem);
bool tls_certificate_fingerprint(const std::string& certificate_pem, std::string& hex);
} // namespace hop
