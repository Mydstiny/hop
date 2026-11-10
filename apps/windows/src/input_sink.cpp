#include "hop_windows/input_sink.h"
#include <utility>

namespace hop::windows {
namespace {
constexpr ULONG_PTR hop_event_marker = static_cast<ULONG_PTR>(0x484f5032u);
constexpr std::int32_t wheel_limit = 120000;
INPUT make_key(ScanCode key, bool down) {
    INPUT event{};
    event.type = INPUT_KEYBOARD;
    event.ki.wScan = key.code;
    event.ki.dwFlags = KEYEVENTF_SCANCODE | (key.extended ? KEYEVENTF_EXTENDEDKEY : 0) | (down ? 0 : KEYEVENTF_KEYUP);
    event.ki.dwExtraInfo = hop_event_marker;
    return event;
}
bool button_flags(Button button, bool down, DWORD& flags, DWORD& data) {
    data = 0;
    switch (button) {
    case Button::left: flags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; return true;
    case Button::right: flags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; return true;
    case Button::middle: flags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; return true;
    case Button::back: flags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP; data = XBUTTON1; return true;
    case Button::forward: flags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP; data = XBUTTON2; return true;
    }
    return false;
}
INPUT make_mouse(DWORD flags, DWORD data = 0) {
    INPUT event{};
    event.type = INPUT_MOUSE;
    event.mi.dwFlags = flags;
    event.mi.mouseData = data;
    event.mi.dwExtraInfo = hop_event_marker;
    return event;
}
}
InputSink::InputSink(Send send) : send_(std::move(send)) {}
InputSink InputSink::system() { return InputSink([](UINT n, LPINPUT p, int size) { return SendInput(n, p, size); }); }
bool InputSink::emit(INPUT& event) { return send_ && send_(1, &event, sizeof(event)) == 1; }
bool InputSink::key(ScanCode key, bool down) {
    // SendInput's documented scan-code flags represent E0 only. The E1 Pause
    // sequence needs separate verified handling; never inject a different key.
    last_error_ = SinkError::none;
    if (!key.code || (key.e1 && key.extended)) { last_error_ = SinkError::invalid_input; return false; }
    if (key.e1) { last_error_ = SinkError::unsupported_sequence; return false; }
    const auto index = static_cast<std::size_t>(key.code) + (key.extended ? 256 : 0);
    // A release for an untracked key is never injected, preventing a remote
    // peer from releasing a key physically held on this device.
    if (!down && !keys_[index]) { last_error_ = SinkError::unmatched_release; return false; }
    INPUT event = make_key(key, down);
    if (!emit(event)) { last_error_ = SinkError::system_rejected; return false; }
    if (down && !keys_[index]) { keys_[index] = true; ++key_count_; }
    else if (!down) { keys_[index] = false; --key_count_; }
    return true;
}
bool InputSink::button(Button button, bool down) {
    last_error_ = SinkError::none;
    DWORD flags = 0, data = 0;
    if (!button_flags(button, down, flags, data)) { last_error_ = SinkError::invalid_input; return false; }
    const auto index = static_cast<std::size_t>(button);
    if (!down && !buttons_[index]) { last_error_ = SinkError::unmatched_release; return false; }
    // A repeated button-down is not a Windows repeat event.
    if (down && buttons_[index]) { last_error_ = SinkError::duplicate_button_down; return false; }
    INPUT event = make_mouse(flags, data);
    if (!emit(event)) { last_error_ = SinkError::system_rejected; return false; }
    if (down) { buttons_[index] = true; ++button_count_; }
    else { buttons_[index] = false; --button_count_; }
    return true;
}
bool InputSink::move_absolute(std::uint16_t x, std::uint16_t y) {
    last_error_ = SinkError::none;
    INPUT event = make_mouse(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK);
    event.mi.dx = x; event.mi.dy = y;
    if (!emit(event)) { last_error_ = SinkError::system_rejected; return false; }
    return true;
}
bool InputSink::scroll(std::int32_t x, std::int32_t y) {
    last_error_ = SinkError::none;
    if (x < -wheel_limit || x > wheel_limit || y < -wheel_limit || y > wheel_limit) { last_error_ = SinkError::invalid_input; return false; }
    // Separate calls: mouseData cannot carry two independent axes at once.
    if (x) {
        INPUT event = make_mouse(MOUSEEVENTF_HWHEEL, static_cast<DWORD>(x));
        if (!emit(event)) { last_error_ = SinkError::system_rejected; return false; }
    }
    if (y) {
        INPUT event = make_mouse(MOUSEEVENTF_WHEEL, static_cast<DWORD>(-y));
        if (!emit(event)) { last_error_ = SinkError::system_rejected; return false; }
    }
    return true;
}
bool InputSink::release_all() {
    bool ok = true;
    for (std::size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i] && !key({static_cast<std::uint8_t>(i & 0xff), i >= 256}, false)) ok = false;
    }
    for (std::size_t i = 0; i < buttons_.size(); ++i) {
        if (buttons_[i] && !button(static_cast<Button>(i), false)) ok = false;
    }
    last_error_ = ok ? SinkError::none : SinkError::system_rejected;
    return ok;
}
} // namespace hop::windows
