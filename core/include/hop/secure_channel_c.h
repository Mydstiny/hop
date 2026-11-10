#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct hop_tls_channel hop_tls_channel;
typedef int (*hop_tls_send_fn)(void*, const unsigned char*, size_t);
typedef int (*hop_tls_recv_fn)(void*, unsigned char*, size_t);
// Callbacks are nonblocking and must return these values on temporary I/O
// unavailability. They must never throw or outlive the channel.
int hop_tls_want_read_error(void);
int hop_tls_want_write_error(void);
// Cryptographically secure random bytes for pairing nonces; 1...32 bytes.
// Clears the caller buffer if entropy generation fails.
int hop_tls_random_bytes(uint8_t* output, size_t size);
// Generates a new device identity into caller-owned, non-overlapping buffers.
// The fingerprint is 64 uppercase hexadecimal characters plus NUL. The caller
// must immediately store the private key securely and erase the buffer.
int hop_tls_generate_identity(char* certificate, size_t certificate_capacity,
                              char* private_key, size_t private_key_capacity,
                              char* fingerprint, size_t fingerprint_capacity);
// Verifies that the single certificate and private key match. Input lengths
// exclude NUL terminators; callers own and must clear private-key memory.
int hop_tls_validate_identity(const char* certificate, size_t certificate_size,
                              const char* private_key, size_t private_key_size);
// Computes the SHA-256 fingerprint of exactly one PEM certificate. Output is
// 64 uppercase hex characters plus NUL. Computing it does not verify the peer;
// both people must compare fingerprints through a trusted out-of-band channel.
int hop_tls_certificate_fingerprint(const char* certificate, size_t certificate_size,
                                    char* fingerprint, size_t fingerprint_capacity);
// client=1 for client, client=0 for server. PEM strings must be NUL terminated.
// peer_certificate_pem must be separately verified; no trust-on-first-use.
// The caller owns io and must keep it alive until hop_tls_free.
hop_tls_channel* hop_tls_create(int client, const char* own_certificate_pem,
                                const char* private_key_pem, const char* peer_certificate_pem,
                                void* io, hop_tls_send_fn send, hop_tls_recv_fn recv);
// 1 authenticated, 0 needs more I/O, -1 failure. Never delivers application
// data before certificate-chain validation and exact peer-certificate pinning.
int hop_tls_handshake(hop_tls_channel* channel);
int hop_tls_read(hop_tls_channel* channel, uint8_t* data, size_t size);
// If a write needs more I/O, retry with the same buffer and length.
int hop_tls_write(hop_tls_channel* channel, const uint8_t* data, size_t size);
int hop_tls_error(const hop_tls_channel* channel);
int hop_tls_io_want(const hop_tls_channel* channel);
void hop_tls_free(hop_tls_channel* channel);
#ifdef __cplusplus
}
#endif
