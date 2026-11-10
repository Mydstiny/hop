#pragma once
#include "hop_windows/clipboard.h"
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
namespace hop::windows {
// A dedicated clipboard worker serializes configuration and writes. Network,
// hook and UI threads only enqueue/revoke under a short mutex: they never call
// the OS clipboard. Callers must restore local input immediately on focus loss,
// request revoke, and only commit a new focus after that ticket is acknowledged.
// If the ticket misses its deadline, the owner closes the session and leaves
// synchronization disabled; it must not terminate a blocked OS worker thread.
// The owner must stop and join its clipboard worker before destruction.
class ClipboardActor {
public:
    explicit ClipboardActor(ClipboardBackend& backend) : sync_(backend) {}
    ~ClipboardActor();
    std::uint64_t activate(std::string peer,std::uint64_t epoch,std::uint32_t baseline,std::uint64_t now_ms);
    std::uint64_t revoke(std::uint64_t now_ms);
    bool acknowledged(std::uint64_t ticket) const;
    bool timed_out(std::uint64_t ticket,std::uint64_t now_ms) const;
    // Notifications contain only a sequence number; actual content is read on
    // this actor's worker after the focus and sequence are checked.
    bool observe_local(std::uint64_t epoch,std::uint32_t sequence);
    bool request_initial_snapshot(std::uint64_t epoch,std::uint32_t sequence);
    bool quiescent(std::uint64_t epoch) const;
    std::optional<ClipboardPayload> take_local(std::uint64_t epoch);
    bool submit_remote(const std::string& peer,std::uint64_t epoch,std::uint32_t baseline,ClipboardPayload payload);
    // Called only by one dedicated clipboard worker. May block in the OS.
    bool work_once();
private:
    struct Configuration {bool enabled=false;std::string peer;std::uint64_t epoch=0;std::uint32_t baseline=0;};
    struct Pending {std::string peer;std::uint64_t epoch;std::uint32_t baseline;ClipboardPayload payload;};
    void clear_pending();
    void clear_local();
    ClipboardSync sync_;mutable std::mutex mutex_;Configuration requested_;std::optional<Pending> pending_;std::optional<ClipboardPayload> outgoing_;
    std::uint32_t pending_sequence_=0;std::uint64_t outgoing_epoch_=0;bool pending_snapshot_=false;
    std::uint64_t requested_ticket_=0,completed_ticket_=0,deadline_=0;bool busy_=false;
};
}
