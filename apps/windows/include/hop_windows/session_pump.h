#pragma once
#include "hop_windows/session.h"
#include "hop_windows/tls_socket.h"
namespace hop::windows {
// Serial worker only. A caller must complete and verify TLS handshake first;
// never feeds plaintext to Session before pinned mutual authentication. The
// pending frame is retained unchanged while TLS reports backpressure.
class SessionPump {
public:
    SessionPump(TlsSocket& transport, Session& session) : transport_(transport), session_(session) {}
    bool step(std::uint64_t monotonic_ms);
private:
    bool abort();
    TlsSocket& transport_;
    Session& session_;
    std::vector<std::uint8_t> pending_;
    std::size_t offset_ = 0;
};
}
