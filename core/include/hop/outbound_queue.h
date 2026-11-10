#pragma once
#include "hop/frame.h"
#include <deque>
#include <optional>

namespace hop {
// Bounded per-connection queues. Input/control keep their mutual order and
// take priority between clipboard chunks; this cannot preempt an in-flight TLS
// write. Enqueue failure requires caller backpressure, never silent key loss.
class OutboundQueue {
public:
    OutboundQueue() = default;
    ~OutboundQueue();
    OutboundQueue(const OutboundQueue&) = delete;
    OutboundQueue& operator=(const OutboundQueue&) = delete;
    static constexpr std::size_t max_interactive_frames = 256;
    static constexpr std::size_t max_clipboard_frames = 8;
    static constexpr std::size_t max_clipboard_bytes = 256 * 1024;
    bool enqueue(FrameKind kind, std::uint64_t epoch, std::vector<std::uint8_t> payload);
    // Ownership transfers to caller, including responsibility for clearing sensitive payloads.
    std::optional<Frame> pop();
    // Called at a focus transition; queued clipboard data must not be delivered
    // to a target which no longer has focus. In-flight frames require a separate
    // epoch check by the recipient.
    void discard_clipboard();
    // At focus release, drop unsent input for this epoch before queuing a
    // release control frame. The caller must still complete/cancel in-flight
    // writes safely and explicitly release every remote key/button.
    std::size_t discard_input(std::uint64_t epoch);
    std::size_t clipboard_bytes() const { return clipboard_bytes_; }
    bool clipboard_has_room(std::size_t bytes) const {
        return clipboard_.size() < max_clipboard_frames && bytes <= max_clipboard_bytes - clipboard_bytes_;
    }
private:
    std::deque<Frame> interactive_, clipboard_;
    std::size_t clipboard_bytes_ = 0;
    std::uint64_t next_sequence_ = 1;
};
} // namespace hop
