#include "hop/outbound_queue.h"
#include <utility>

namespace hop {
namespace {
void wipe(std::vector<std::uint8_t>& bytes) {
    volatile std::uint8_t* p = bytes.data();
    for (std::size_t i=0;i<bytes.size();++i) p[i]=0;
}
struct WipeOnExit { std::vector<std::uint8_t>& bytes; ~WipeOnExit() { wipe(bytes); } };
}
OutboundQueue::~OutboundQueue() {
    discard_clipboard();
    for (auto& frame : interactive_) wipe(frame.payload);
}
bool OutboundQueue::enqueue(FrameKind kind, std::uint64_t epoch, std::vector<std::uint8_t> payload) {
    WipeOnExit wipeArgument{payload};
    if (!epoch || !next_sequence_) return false;
    const bool clip = kind == FrameKind::clipboard_control || kind == FrameKind::clipboard_chunk;
    if (kind != FrameKind::control && kind != FrameKind::input && !clip) return false;
    if (kind == FrameKind::clipboard_chunk) {
        if (payload.empty() || payload.size() > max_clipboard_chunk) return false;
    } else if (payload.size() > max_control_payload) return false;
    if (clip) {
        if (clipboard_.size() >= max_clipboard_frames || payload.size() > max_clipboard_bytes - clipboard_bytes_) return false;
        const auto bytes = payload.size();
        try { clipboard_.push_back({kind, 0, epoch, std::move(payload)}); }
        catch (...) { return false; }
        clipboard_bytes_ += bytes;
    } else {
        if (interactive_.size() >= max_interactive_frames) return false;
        try { interactive_.push_back({kind, 0, epoch, std::move(payload)}); }
        catch (...) { return false; }
    }
    return true;
}
std::optional<Frame> OutboundQueue::pop() {
    if (!next_sequence_ || (interactive_.empty() && clipboard_.empty())) return std::nullopt;
    auto& queue = interactive_.empty() ? clipboard_ : interactive_;
    Frame out = std::move(queue.front());
    queue.pop_front();
    if (out.kind == FrameKind::clipboard_control || out.kind == FrameKind::clipboard_chunk) clipboard_bytes_ -= out.payload.size();
    out.sequence = next_sequence_++;
    return out;
}
std::size_t OutboundQueue::discard_input(std::uint64_t epoch) {
    std::size_t removed = 0;
    for (auto it = interactive_.begin(); it != interactive_.end();) {
        if (it->kind == FrameKind::input && it->epoch == epoch) { wipe(it->payload); it = interactive_.erase(it); ++removed; }
        else ++it;
    }
    return removed;
}
void OutboundQueue::discard_clipboard() { for (auto& frame : clipboard_) wipe(frame.payload); clipboard_.clear(); clipboard_bytes_ = 0; }
} // namespace hop
