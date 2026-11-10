#include "hop/posix_tls_socket.h"
#include "hop/tls_identity.h"
#include <mbedtls/net_sockets.h>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <new>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
namespace hop {
namespace {
constexpr std::size_t max_pem = 64 * 1024, max_io = 16 * 1024;
bool valid_pem(const std::string& s) { return !s.empty() && s.size() <= max_pem && s.find('\0') == std::string::npos; }
void clear(std::vector<std::uint8_t>& data) {
    volatile std::uint8_t* p = data.data();
    for (std::size_t i = 0; i < data.size(); ++i) p[i] = 0;
    data.clear();
}
}
std::unique_ptr<PosixTlsSocket> PosixTlsSocket::adopt(int fd, bool client, const PosixTlsCredentials& credentials) {
    std::unique_ptr<PosixTlsSocket> out(new (std::nothrow) PosixTlsSocket(fd));
    if (!out) { if (fd >= 0) ::close(fd); return nullptr; }
    if (fd < 0 || !valid_pem(credentials.certificate_pem) || !valid_pem(credentials.private_key_pem) || !valid_pem(credentials.peer.certificate_pem)) return nullptr;
    sockaddr_storage peer{}; socklen_t size = sizeof(peer);
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &size) != 0 ||
        (peer.ss_family != AF_INET && peer.ss_family != AF_INET6)) return nullptr;
    int type = 0; socklen_t type_size = sizeof(type);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &type_size) != 0 || type != SOCK_STREAM) return nullptr;
    int no_delay = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay)) != 0) return nullptr;
    const int flags = fcntl(fd, F_GETFL, 0);
    const int descriptor_flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0 || descriptor_flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
        fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) return nullptr;
#ifdef SO_NOSIGPIPE
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) != 0) return nullptr;
#endif
    if (!tls_certificate_fingerprint(credentials.peer.certificate_pem,out->peer_fingerprint_)) return nullptr;
    out->channel_ = hop_tls_create(client ? 1 : 0, credentials.certificate_pem.c_str(), credentials.private_key_pem.c_str(),
                                   credentials.peer.certificate_pem.c_str(), out.get(), &send_callback, &recv_callback);
    return out->channel_ ? std::move(out) : nullptr;
}
PosixTlsSocket::~PosixTlsSocket() { close(); }
void PosixTlsSocket::close() {
    if (channel_) { hop_tls_free(channel_); channel_ = nullptr; }
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    clear(pending_); status_ = SocketProgress::closed;
}
int PosixTlsSocket::send_callback(void* context, const unsigned char* data, std::size_t size) {
    auto* self = static_cast<PosixTlsSocket*>(context);
    if (!self || self->fd_ < 0 || !data || !size) return MBEDTLS_ERR_NET_SEND_FAILED;
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    const auto n = ::send(self->fd_, data, std::min<std::size_t>(size, INT_MAX), flags);
    if (n > 0) return static_cast<int>(n);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return hop_tls_want_write_error();
    if (n < 0 && (errno == ECONNRESET || errno == EPIPE)) return MBEDTLS_ERR_NET_CONN_RESET;
    return MBEDTLS_ERR_NET_SEND_FAILED;
}
int PosixTlsSocket::recv_callback(void* context, unsigned char* data, std::size_t size) {
    auto* self = static_cast<PosixTlsSocket*>(context);
    if (!self || self->fd_ < 0 || !data || !size) return MBEDTLS_ERR_NET_RECV_FAILED;
    const auto n = ::recv(self->fd_, data, std::min<std::size_t>(size, INT_MAX), 0);
    if (n >= 0) return static_cast<int>(n);
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return hop_tls_want_read_error();
    if (errno == ECONNRESET) return MBEDTLS_ERR_NET_CONN_RESET;
    return MBEDTLS_ERR_NET_RECV_FAILED;
}
SocketProgress PosixTlsSocket::handshake() {
    if (status_ != SocketProgress::again || !channel_) return status_;
    const auto r = hop_tls_handshake(channel_);
    if (r == 1) status_ = SocketProgress::ready;
    else if (r < 0) status_ = SocketProgress::failed;
    return status_;
}
SocketResult PosixTlsSocket::result(int n) {
    if (n > 0) return {SocketProgress::ready, static_cast<std::size_t>(n)};
    if (n == hop_tls_want_read_error() || n == hop_tls_want_write_error()) return {SocketProgress::again, 0};
    status_ = n == 0 ? SocketProgress::closed : SocketProgress::failed;
    return {status_, 0};
}
SocketResult PosixTlsSocket::write(const std::uint8_t* data, std::size_t length) {
    if (!authenticated() || !data || !length || length > max_io) return {SocketProgress::failed, 0};
    if (!pending_.empty()) {
        if (pending_.size() != length || std::memcmp(pending_.data(), data, length) != 0) {
            clear(pending_); status_ = SocketProgress::failed; return {status_, 0};
        }
    } else {
        try { pending_.assign(data, data + length); } catch (...) { status_ = SocketProgress::failed; return {status_, 0}; }
    }
    auto r = result(hop_tls_write(channel_, pending_.data(), pending_.size()));
    if (r.status != SocketProgress::again) clear(pending_);
    return r;
}
SocketResult PosixTlsSocket::read(std::uint8_t* data, std::size_t length) {
    if (!authenticated() || !data || !length || length > max_io) return {SocketProgress::failed, 0};
    return result(hop_tls_read(channel_, data, length));
}
int PosixTlsSocket::error() const { return channel_ ? hop_tls_error(channel_) : -1; }
int PosixTlsSocket::io_want() const { return channel_ ? hop_tls_io_want(channel_) : 0; }
}
