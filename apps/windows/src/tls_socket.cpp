#include "hop_windows/tls_socket.h"
#include <mbedtls/net_sockets.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <climits>
#include <cstring>
#include <new>

namespace hop::windows {
namespace {
constexpr std::size_t max_pem = 64 * 1024;
constexpr std::size_t max_io = 16 * 1024;
bool pem_ok(const std::string& text) {
    return !text.empty() && text.size() <= max_pem && text.find('\0') == std::string::npos;
}
}
std::unique_ptr<TlsSocket> TlsSocket::adopt(SOCKET socket, bool client, const TlsCredentials& credentials) {
    std::unique_ptr<TlsSocket> out(new (std::nothrow) TlsSocket(socket));
    if (!out) { if (socket != INVALID_SOCKET) closesocket(socket); return nullptr; }
    if (socket == INVALID_SOCKET || !pem_ok(credentials.own_certificate_pem) || !pem_ok(credentials.private_key_pem) || !pem_ok(credentials.peer.pem)) return nullptr;
    char fingerprint[65]{};
    if (hop_tls_certificate_fingerprint(credentials.peer.pem.data(), credentials.peer.pem.size(), fingerprint, sizeof(fingerprint)) != 0) return nullptr;
    out->pinned_peer_fingerprint_=fingerprint;
    sockaddr_storage peer{}; int peer_len = sizeof(peer);
    if (getpeername(socket, reinterpret_cast<sockaddr*>(&peer), &peer_len) == SOCKET_ERROR) return nullptr;
    // Interactive key/button events should not wait for Nagle batching.
    // This changes only this owned socket; it does not guarantee latency.
    int nodelay=1;
    if (setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&nodelay), sizeof(nodelay)) == SOCKET_ERROR) return nullptr;
    u_long nonblocking = 1;
    if (ioctlsocket(socket, FIONBIO, &nonblocking) == SOCKET_ERROR) return nullptr;
    out->channel_ = hop_tls_create(client ? 1 : 0, credentials.own_certificate_pem.c_str(), credentials.private_key_pem.c_str(),
                                   credentials.peer.pem.c_str(), out.get(), &TlsSocket::send_callback, &TlsSocket::recv_callback);
    return out->channel_ ? std::move(out) : nullptr;
}
TlsSocket::~TlsSocket() { close(); }
void TlsSocket::close() {
    status_=TlsIoStatus::closed;
    if (channel_) { hop_tls_free(channel_); channel_=nullptr; }
    if (socket_ != INVALID_SOCKET) { closesocket(socket_); socket_=INVALID_SOCKET; }
    // Pending application bytes may contain clipboard text: clear their buffer.
    if (!pending_write_.empty()) SecureZeroMemory(pending_write_.data(), pending_write_.size());
    pending_write_.clear();
}
int TlsSocket::send_callback(void* context, const unsigned char* data, std::size_t size) {
    auto* self = static_cast<TlsSocket*>(context);
    if (!self || self->socket_ == INVALID_SOCKET || !data || !size) return MBEDTLS_ERR_NET_SEND_FAILED;
    const int n = ::send(self->socket_, reinterpret_cast<const char*>(data), static_cast<int>(std::min<std::size_t>(size, INT_MAX)), 0);
    if (n > 0) return n;
    if (n == SOCKET_ERROR) {
        const int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK || error == WSAEINTR) return hop_tls_want_write_error();
        if (error == WSAECONNRESET) return MBEDTLS_ERR_NET_CONN_RESET;
    }
    return MBEDTLS_ERR_NET_SEND_FAILED;
}
int TlsSocket::recv_callback(void* context, unsigned char* data, std::size_t size) {
    auto* self = static_cast<TlsSocket*>(context);
    if (!self || self->socket_ == INVALID_SOCKET || !data || !size) return MBEDTLS_ERR_NET_RECV_FAILED;
    const int n = ::recv(self->socket_, reinterpret_cast<char*>(data), static_cast<int>(std::min<std::size_t>(size, INT_MAX)), 0);
    if (n >= 0) return n;
    const int error = WSAGetLastError();
    if (error == WSAEWOULDBLOCK || error == WSAEINTR) return hop_tls_want_read_error();
    if (error == WSAECONNRESET) return MBEDTLS_ERR_NET_CONN_RESET;
    return MBEDTLS_ERR_NET_RECV_FAILED;
}
TlsIoStatus TlsSocket::handshake() {
    if (status_ != TlsIoStatus::again || !channel_) return status_;
    const auto result = hop_tls_handshake(channel_);
    if (result == 1) status_ = TlsIoStatus::ready;
    else if (result < 0) status_ = TlsIoStatus::failed;
    return status_;
}
TlsIoResult TlsSocket::io_result(int result) {
    if (result > 0) return {TlsIoStatus::ready, static_cast<std::size_t>(result)};
    if (result == hop_tls_want_read_error() || result == hop_tls_want_write_error()) return {TlsIoStatus::again, 0};
    status_ = result == 0 ? TlsIoStatus::closed : TlsIoStatus::failed;
    return {status_, 0};
}
TlsIoResult TlsSocket::write(const std::uint8_t* data, std::size_t size) {
    if (!authenticated() || !data || !size || size > max_io) return {TlsIoStatus::failed, 0};
    if (!pending_write_.empty()) {
        if (size != pending_write_.size() || std::memcmp(data, pending_write_.data(), size) != 0) {
            SecureZeroMemory(pending_write_.data(), pending_write_.size());
            pending_write_.clear();
            status_ = TlsIoStatus::failed;
            return {TlsIoStatus::failed, 0};
        }
    } else {
        try { pending_write_.assign(data, data + size); } catch (...) { status_ = TlsIoStatus::failed; return {status_, 0}; }
    }
    const auto result = io_result(hop_tls_write(channel_, pending_write_.data(), pending_write_.size()));
    if (result.status != TlsIoStatus::again) {
        SecureZeroMemory(pending_write_.data(), pending_write_.size());
        pending_write_.clear();
    }
    return result;
}
TlsIoResult TlsSocket::read(std::uint8_t* data, std::size_t capacity) {
    if (!authenticated() || !data || !capacity || capacity > max_io) return {TlsIoStatus::failed, 0};
    return io_result(hop_tls_read(channel_, data, capacity));
}
int TlsSocket::tls_error() const { return channel_ ? hop_tls_error(channel_) : -1; }
} // namespace hop::windows
