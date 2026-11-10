#pragma once
#include "hop/secure_channel_c.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
namespace hop {
// Caller is responsible for out-of-band verification; merely receiving a
// certificate from the network does not make it a verified peer.
struct VerifiedPeer { std::string certificate_pem; };
struct PosixTlsCredentials { std::string certificate_pem, private_key_pem; VerifiedPeer peer; };
enum class SocketProgress { ready, again, closed, failed };
struct SocketResult { SocketProgress status; std::size_t bytes = 0; };
// Single-owner, nonblocking TCP wrapper. Takes ownership of fd even on error.
// Caller supplies deadlines/cancellation and ensures calls are serialized.
class PosixTlsSocket {
public:
    static std::unique_ptr<PosixTlsSocket> adopt(int fd, bool client, const PosixTlsCredentials& credentials);
    ~PosixTlsSocket();
    PosixTlsSocket(const PosixTlsSocket&) = delete;
    PosixTlsSocket& operator=(const PosixTlsSocket&) = delete;
    SocketProgress handshake();
    SocketResult read(std::uint8_t* data, std::size_t length);
    SocketResult write(const std::uint8_t* data, std::size_t length);
    bool authenticated() const { return status_ == SocketProgress::ready; }
    int error() const;
    int io_want() const;
    int native_fd() const { return fd_; } // poll only; ownership stays here
    const std::string& pinned_peer_fingerprint() const { return peer_fingerprint_; }
    void close();
private:
    explicit PosixTlsSocket(int fd) : fd_(fd) {}
    static int send_callback(void*, const unsigned char*, std::size_t);
    static int recv_callback(void*, unsigned char*, std::size_t);
    SocketResult result(int);
    int fd_;
    hop_tls_channel* channel_ = nullptr;
    SocketProgress status_ = SocketProgress::again;
    std::vector<std::uint8_t> pending_;
    std::string peer_fingerprint_;
};
}
