#pragma once
#include "hop/posix_tls_socket.h"
#include "hop/session.h"
#include <functional>
#include <vector>
namespace hop {
// Serialized worker; TLS must already have completed a pinned mutual handshake
// and Session must be bound to that exact peer fingerprint. Each step performs
// bounded nonblocking I/O: write, read, then write the replies those reads
// produced. Caller drives poll/timeouts and queues UI commands.
class PosixSessionPump {
public:
    PosixSessionPump(PosixTlsSocket& transport, v2::Session& session,
                     std::function<bool()> begin_batch = {}, std::function<void()> end_batch = {});
    ~PosixSessionPump();
    bool step(std::uint64_t monotonic_ms);
    // Writes frames queued since the last step without ticking or reading, so
    // a caller that just produced a reply need not wait for its next wake-up.
    bool flush();
    void stop();
private:
    bool abort();
    bool write_pending();
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
