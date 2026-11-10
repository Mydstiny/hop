#include "hop/posix_tls_socket_c.h"
#include "hop/posix_tls_socket.h"
#include <mbedtls/platform_util.h>
#include <unistd.h>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
struct hop_posix_tls_socket { std::unique_ptr<hop::PosixTlsSocket> value; };
namespace {
struct OwnedFd { int value; ~OwnedFd() { if (value >= 0) close(value); } };
struct Credentials {
    hop::PosixTlsCredentials value;
    ~Credentials() { if (!value.private_key_pem.empty()) mbedtls_platform_zeroize(&value.private_key_pem[0],value.private_key_pem.size()); }
};
int status(hop::SocketProgress progress) {
    switch (progress) {
    case hop::SocketProgress::ready: return 1;
    case hop::SocketProgress::again: return 0;
    case hop::SocketProgress::closed: return 2;
    case hop::SocketProgress::failed: return -1;
    }
    return -1;
}
bool valid(const char* text) { return text && *text && strnlen(text, 65537) <= 65536; }
}
extern "C" hop_posix_tls_socket* hop_posix_tls_adopt(int fd, int client, const char* cert, const char* key, const char* peer) {
    OwnedFd owned{fd};
    if (fd < 0 || (client != 0 && client != 1) || !valid(cert) || !valid(key) || !valid(peer)) return nullptr;
    try {
        Credentials credentials{{cert, key, {peer}}};
        auto result = std::make_unique<hop_posix_tls_socket>();
        // Ownership crosses to adopt; it promises to close on all failures.
        int transferred = owned.value; owned.value = -1;
        result->value = hop::PosixTlsSocket::adopt(transferred, client != 0, credentials.value);
        return result->value ? result.release() : nullptr;
    } catch (...) { return nullptr; }
}
extern "C" int hop_posix_tls_handshake(hop_posix_tls_socket* socket) {
    if (!socket || !socket->value) return -1;
    try { return status(socket->value->handshake()); } catch (...) { return -1; }
}
extern "C" int hop_posix_tls_read(hop_posix_tls_socket* socket, uint8_t* buffer, size_t capacity, size_t* count) {
    if (count) *count = 0;
    if (!socket || !socket->value || !count) return -1;
    try { auto r = socket->value->read(buffer, capacity); *count = r.bytes; return status(r.status); } catch (...) { return -1; }
}
extern "C" int hop_posix_tls_write(hop_posix_tls_socket* socket, const uint8_t* buffer, size_t length, size_t* count) {
    if (count) *count = 0;
    if (!socket || !socket->value || !count) return -1;
    try { auto r = socket->value->write(buffer, length); *count = r.bytes; return status(r.status); } catch (...) { return -1; }
}
extern "C" int hop_posix_tls_error(const hop_posix_tls_socket* socket) { return socket && socket->value ? socket->value->error() : -1; }
extern "C" int hop_posix_tls_wants_write(const hop_posix_tls_socket* socket) {
    return socket && socket->value ? (socket->value->io_want()==hop_tls_want_write_error() ? 1 : 0) : -1;
}
extern "C" const char* hop_posix_tls_authenticated_peer(const hop_posix_tls_socket* socket) {
    return socket && socket->value && socket->value->authenticated() ? socket->value->pinned_peer_fingerprint().c_str() : nullptr;
}
extern "C" void hop_posix_tls_free(hop_posix_tls_socket* socket) { delete socket; }
