#include "hop_windows/input_ownership.h"
namespace hop::windows {
void InputOwnership::abandon_remote() {
    active_ = false;
    remote_keys_.fill(false); remote_buttons_.fill(false);
    remote_key_count_ = remote_button_count_ = 0;
}
bool InputOwnership::activate() {
    if (local_key_count_ || local_button_count_ || !drained()) return false;
    active_ = true;
    return true;
}
Decision InputOwnership::key(std::uint16_t code, bool down) {
    if (down) {
        if (remote_keys_[code]) return {true, active_, active_};
        if (local_keys_[code]) return {false, false};
        if (active_) { remote_keys_[code] = true; ++remote_key_count_; return {true, true}; }
        local_keys_[code] = true; ++local_key_count_; return {false, false};
    }
    if (remote_keys_[code]) { remote_keys_[code] = false; --remote_key_count_; return {true, active_}; }
    if (local_keys_[code]) { local_keys_[code] = false; --local_key_count_; return {false, false}; }
    // Unknown releases are never synthesized toward remote or swallowed.
    return {false, false};
}
Decision InputOwnership::button(std::uint8_t code, bool down) {
    if (down) {
        if (remote_buttons_[code]) return {true, active_};
        if (local_buttons_[code]) return {false, false};
        if (active_) { remote_buttons_[code] = true; ++remote_button_count_; return {true, true}; }
        local_buttons_[code] = true; ++local_button_count_; return {false, false};
    }
    if (remote_buttons_[code]) { remote_buttons_[code] = false; --remote_button_count_; return {true, active_}; }
    if (local_buttons_[code]) { local_buttons_[code] = false; --local_button_count_; return {false, false}; }
    return {false, false};
}
Decision InputOwnership::motion(std::uint8_t held_button, bool drag) const {
    // Win32 low-level WM_MOUSEMOVE does not identify the held button. During
    // post-remote draining, suppress movement until every remote-held button
    // is physically released so a remote drag cannot leak into a local app.
    return {active_ || remote_button_count_ > 0 || (drag && remote_buttons_[held_button]), active_};
}
} // namespace hop::windows
