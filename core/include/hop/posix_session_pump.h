#pragma once
#include "hop/posix_tls_socket.h"
#include "hop/session.h"
#include <functional>
#include <vector>
namespace hop {
// Serialized worker; TLS must already have completed a pinned mutual handshake
// and Session must be bound to that exact peer fingerprint. Each step performs
// bounded nonblocking I/O. Caller drives poll/timeouts and queues UI commands.
class PosixSessionPump {
public:
    PosixSessionPump(PosixTlsSocket& transport, v2::Session& session,
                     std::function<bool()> begin_batch = {}, std::function<void()> end_batch = {});
    ~PosixSessionPump();
    bool step(std::uint64_t monotonic_ms);
    void stop();
private:
    bool abort();
    void wipe();
    PosixTlsSocket& transport_;
    v2::Session& session_;
    std::function<bool()> begin_batch_;
    std::function<void()> end_batch_;
    std::vector<std::uint8_t> pending_;
    std::size_t offset_=0;
    bool stopped_=false;
};
}
