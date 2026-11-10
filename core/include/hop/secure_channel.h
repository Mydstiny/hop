#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace hop {
// Experimental TLS transport. A trusted peer certificate must be obtained and
// verified out of band before construction. No first-use trust or fallback.
enum class TlsRole { client, server };
enum class TlsProgress { ready, again, failed };
using TlsSend = int (*)(void*, const unsigned char*, std::size_t);
using TlsRecv = int (*)(void*, unsigned char*, std::size_t);
struct TlsIdentity {
    std::string certificate_pem;
    std::string private_key_pem;
    std::string trusted_peer_certificate_pem;
};
class SecureChannel {
public:
    SecureChannel(TlsRole role, const TlsIdentity& identity, void* io, TlsSend send, TlsRecv recv);
    ~SecureChannel();
    SecureChannel(const SecureChannel&) = delete;
    SecureChannel& operator=(const SecureChannel&) = delete;
    TlsProgress handshake();
    // Positive is bytes transferred, 0 is EOF, negative is an mbed TLS error.
    // These reject use until both certificate verification and pin check pass.
    int write(const std::uint8_t* data, std::size_t length);
    int read(std::uint8_t* data, std::size_t length);
    bool authenticated() const;
    int error() const;
    int io_want() const; // last WANT_READ/WANT_WRITE, or zero
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hop
