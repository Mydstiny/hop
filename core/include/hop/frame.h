#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace hop {
// The framing layer must only be fed plaintext from an authenticated TLS
// session. It provides bounds and replay/order checks, not authentication.
enum class FrameKind : std::uint8_t {
    control = 1, input = 2, clipboard_control = 3, clipboard_chunk = 4
};
struct Frame {
    FrameKind kind;
    std::uint64_t sequence;
    std::uint64_t epoch;
    std::vector<std::uint8_t> payload;
};
constexpr std::size_t frame_header_size = 28;
constexpr std::size_t max_control_payload = 4096;
constexpr std::size_t max_clipboard_chunk = 65536;
// Emits an empty vector if arguments are invalid. Integers use network order.
std::vector<std::uint8_t> encode_frame(const Frame& frame);

class FrameDecoder {
public:
    // Callback must not re-enter this decoder. Return false to stop/fail closed.
    using Receiver = std::function<bool(Frame&&)>;
    explicit FrameDecoder(Receiver receiver);
    ~FrameDecoder();
    FrameDecoder(const FrameDecoder&) = delete;
    FrameDecoder& operator=(const FrameDecoder&) = delete;
    bool feed(const std::uint8_t* bytes, std::size_t count);
    bool failed() const { return failed_; }
    bool finish(); // EOF is valid only at a frame boundary.
private:
    bool parse_header();
    bool deliver();
    void wipe_current();
    Receiver receiver_;
    std::array<std::uint8_t, frame_header_size> header_{};
    std::size_t header_used_ = 0, payload_used_ = 0;
    Frame current_{};
    std::uint64_t last_sequence_ = 0;
    bool failed_ = false, finished_ = false;
};
} // namespace hop
