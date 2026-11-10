#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "hop/secure_channel_c.h"

namespace hop::windows {
// This type is an explicit assertion by the caller: the certificate must have
// been verified out of band. Merely receiving it over the network is not proof.
struct VerifiedPeerCertificate { std::string pem; };
struct TlsCredentials {
    std::string own_certificate_pem;
    std::string private_key_pem;
    VerifiedPeerCertificate peer;
};
enum class TlsIoStatus { ready, again, closed, failed };
struct TlsIoResult { TlsIoStatus status; std::size_t bytes = 0; };

// Single-threaded owner of an already-connected TCP socket. WSAStartup must be
// completed by the caller. adopt() consumes the SOCKET even when it fails;
// it never discovers peers, trusts certificates, reconnects or falls back to
// plaintext. Each connection must have a separately verified peer certificate.
class TlsSocket {
public:
    static std::unique_ptr<TlsSocket> adopt(SOCKET socket, bool client, const TlsCredentials& credentials);
    ~TlsSocket();
    // Abort the connection immediately; idempotent and nonblocking.
    void close();
    TlsSocket(const TlsSocket&) = delete;
    TlsSocket& operator=(const TlsSocket&) = delete;
    TlsIoStatus handshake();
    TlsIoResult write(const std::uint8_t* data, std::size_t size);
    TlsIoResult read(std::uint8_t* data, std::size_t capacity);
    bool authenticated() const { return status_ == TlsIoStatus::ready; }
    int tls_error() const;
    const std::string& pinned_peer_fingerprint() const { return pinned_peer_fingerprint_; }
private:
    explicit TlsSocket(SOCKET socket) : socket_(socket) {}
    static int send_callback(void* context, const unsigned char* data, std::size_t size);
    static int recv_callback(void* context, unsigned char* data, std::size_t size);
    TlsIoResult io_result(int result);
    SOCKET socket_ = INVALID_SOCKET;
    hop_tls_channel* channel_ = nullptr;
    TlsIoStatus status_ = TlsIoStatus::again;
    std::vector<std::uint8_t> pending_write_;
    std::string pinned_peer_fingerprint_;
};
} // namespace hop::windows
