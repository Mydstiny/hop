#include "hop/frame.h"
#include <algorithm>
#include <utility>

namespace hop {
namespace {
void put32(std::uint8_t* p, std::uint32_t n) {
    for (int i = 3; i >= 0; --i) { p[i] = static_cast<std::uint8_t>(n); n >>= 8; }
}
void put64(std::uint8_t* p, std::uint64_t n) {
    for (int i = 7; i >= 0; --i) { p[i] = static_cast<std::uint8_t>(n); n >>= 8; }
}
std::uint32_t get32(const std::uint8_t* p) {
    std::uint32_t n = 0; for (int i = 0; i < 4; ++i) n = (n << 8) | p[i]; return n;
}
std::uint64_t get64(const std::uint8_t* p) {
    std::uint64_t n = 0; for (int i = 0; i < 8; ++i) n = (n << 8) | p[i]; return n;
}
bool valid(FrameKind kind, std::uint64_t sequence, std::uint64_t epoch, std::size_t size) {
    if (!sequence || !epoch) return false;
    switch (kind) {
    case FrameKind::control:
    case FrameKind::input:
    case FrameKind::clipboard_control: return size <= max_control_payload;
    case FrameKind::clipboard_chunk: return size > 0 && size <= max_clipboard_chunk;
    }
    return false;
}
}
std::vector<std::uint8_t> encode_frame(const Frame& frame) {
    if (!valid(frame.kind, frame.sequence, frame.epoch, frame.payload.size())) return {};
    std::vector<std::uint8_t> out(frame_header_size + frame.payload.size());
    out[0] = 'H'; out[1] = 'O'; out[2] = 'P'; out[3] = '2';
    out[4] = 2; out[5] = static_cast<std::uint8_t>(frame.kind);
    out[6] = 0; out[7] = 0;
    put32(out.data() + 8, static_cast<std::uint32_t>(frame.payload.size()));
    put64(out.data() + 12, frame.sequence); put64(out.data() + 20, frame.epoch);
    std::copy(frame.payload.begin(), frame.payload.end(), out.begin() + frame_header_size);
    return out;
}
FrameDecoder::FrameDecoder(Receiver receiver) : receiver_(std::move(receiver)) {
    if (!receiver_) failed_ = true;
}
void FrameDecoder::wipe_current() {
    volatile std::uint8_t* p = current_.payload.data();
    for (std::size_t i=0;i<current_.payload.size();++i) p[i]=0;
    current_.payload.clear();
}
FrameDecoder::~FrameDecoder() { wipe_current(); }
bool FrameDecoder::parse_header() {
    if (header_[0] != 'H' || header_[1] != 'O' || header_[2] != 'P' || header_[3] != '2' ||
        header_[4] != 2 || header_[6] != 0 || header_[7] != 0) return false;
    const auto kind = static_cast<FrameKind>(header_[5]);
    const auto length = get32(header_.data() + 8);
    const auto sequence = get64(header_.data() + 12);
    const auto epoch = get64(header_.data() + 20);
    if (!valid(kind, sequence, epoch, length) || sequence <= last_sequence_) return false;
    current_.kind = kind; current_.sequence = sequence; current_.epoch = epoch;
    current_.payload.assign(length, 0);
    payload_used_ = 0;
    return true;
}
bool FrameDecoder::deliver() {
    last_sequence_ = current_.sequence;
    Frame completed = std::move(current_);
    current_ = Frame{}; header_used_ = 0; payload_used_ = 0;
    struct Wiper {
        Frame& frame;
        ~Wiper() { volatile std::uint8_t* p=frame.payload.data(); for (std::size_t i=0;i<frame.payload.size();++i) p[i]=0; }
    } wipe{completed};
    return receiver_(std::move(completed));
}
bool FrameDecoder::feed(const std::uint8_t* bytes, std::size_t count) {
    if (failed_ || finished_ || (!bytes && count)) { failed_ = true; wipe_current(); return false; }
    try {
    while (count) {
        if (header_used_ < frame_header_size) {
            const auto n = std::min(count, frame_header_size - header_used_);
            std::copy_n(bytes, n, header_.data() + header_used_);
            bytes += n; count -= n; header_used_ += n;
            if (header_used_ != frame_header_size) continue;
            if (!parse_header()) { failed_ = true; wipe_current(); return false; }
            if (current_.payload.empty() && !deliver()) { failed_ = true; wipe_current(); return false; }
        } else {
            const auto n = std::min(count, current_.payload.size() - payload_used_);
            std::copy_n(bytes, n, current_.payload.data() + payload_used_);
            bytes += n; count -= n; payload_used_ += n;
            if (payload_used_ == current_.payload.size() && !deliver()) { failed_ = true; wipe_current(); return false; }
        }
    }
    } catch (...) { failed_ = true; wipe_current(); return false; }
    return true;
}
bool FrameDecoder::finish() {
    if (header_used_ || failed_) { failed_ = true; wipe_current(); return false; }
    finished_ = true;
    return true;
}
} // namespace hop
