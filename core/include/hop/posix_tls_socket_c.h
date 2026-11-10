#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct hop_posix_tls_socket hop_posix_tls_socket;
// Owns and closes fd even on failure. Peer PEM must already have been
// independently verified. No peer discovery, timeout or plaintext fallback.
hop_posix_tls_socket* hop_posix_tls_adopt(int fd, int client, const char* own_certificate,
                                          const char* private_key, const char* verified_peer_certificate);
// Returns 1 = ready, 0 = would block, 2 = closed, -1 = failure. On read or
// write, transferred bytes are returned via count. A would-block write must be
// retried using identical contents and length. Calls must be serialized.
int hop_posix_tls_handshake(hop_posix_tls_socket* socket);
int hop_posix_tls_read(hop_posix_tls_socket* socket, uint8_t* buffer, size_t capacity, size_t* count);
int hop_posix_tls_write(hop_posix_tls_socket* socket, const uint8_t* buffer, size_t length, size_t* count);
int hop_posix_tls_error(const hop_posix_tls_socket* socket);
// 1 means poll for writability, 0 for readability, -1 for invalid socket.
int hop_posix_tls_wants_write(const hop_posix_tls_socket* socket);
// Null until mutual TLS is authenticated. Returned uppercase fingerprint is
// owned by the socket and valid only until it is freed/closed.
const char* hop_posix_tls_authenticated_peer(const hop_posix_tls_socket* socket);
void hop_posix_tls_free(hop_posix_tls_socket* socket);
#ifdef __cplusplus
}
#endif
