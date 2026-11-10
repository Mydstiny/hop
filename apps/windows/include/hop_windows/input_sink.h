#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <functional>
#include <array>

namespace hop::windows {

enum class Button : std::uint8_t { left, right, middle, back, forward };
enum class SinkError { none, invalid_input, unsupported_sequence, unmatched_release, duplicate_button_down, system_rejected };
struct ScanCode {
    std::uint8_t code;
    bool extended;
    // E1 denotes the Pause make sequence E1 1D 45; it must not be treated as
    // KEYEVENTF_EXTENDEDKEY, which represents only the E0 prefix.
    bool e1 = false;
    bool operator<(const ScanCode& other) const {
        return code < other.code || (code == other.code && (extended < other.extended || (extended == other.extended && e1 < other.e1)));
    }
};

// This adapter is single-threaded. Its caller must gate all operations on an
// authenticated, active session and serialize them onto one input worker.
// The injector is injectable so tests never call the real OS input service.
class InputSink {
public:
    using Send = std::function<UINT(UINT, LPINPUT, int)>;
    explicit InputSink(Send send);
    static InputSink system();
    bool key(ScanCode key, bool down);
    bool button(Button button, bool down);
    // x/y represent the normalized entire Windows virtual desktop, not a
    // particular monitor. The layout adapter must convert correctly first.
    bool move_absolute(std::uint16_t x, std::uint16_t y);
    // Positive x is right; positive y is down. Units: 120 per wheel step.
    bool scroll(std::int32_t x, std::int32_t y);
    // Attempts every outstanding release even if an earlier one fails.
    // Failed releases remain tracked for an explicit retry; caller disconnects.
    bool release_all();
    std::size_t held_keys() const { return key_count_; }
    std::size_t held_buttons() const { return button_count_; }
    SinkError last_error() const { return last_error_; }
private:
    bool emit(INPUT& event);
    Send send_;
    std::array<bool, 512> keys_{};
    std::array<bool, 5> buttons_{};
    std::size_t key_count_ = 0, button_count_ = 0;
    SinkError last_error_ = SinkError::none;
};
} // namespace hop::windows
