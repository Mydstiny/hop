#pragma once
#include <cstdint>
#include <array>
#include <cstddef>

namespace hop::windows {
struct Decision {
    bool suppress_local;
    bool forward_remote;
    bool repeat = false;
};
// Pure, single-threaded ownership policy for a hook worker; never installs a
// hook or reads physical state. Tracks even keys that cannot be mapped.
class InputOwnership {
public:
    // Refuse a transition while an input from either ownership domain is held.
    // The adapter must seed pre-existing physical input before accepting edges.
    bool activate();
    void deactivate() { active_ = false; }
    // Last-resort recovery after missing physical releases/device removal.
    // May let a later release pass locally, so use only after remote release.
    void abandon_remote();
    bool active() const { return active_; }
    bool drained() const { return remote_key_count_ == 0 && remote_button_count_ == 0; }
    void seed_local_key(std::uint16_t code) { if (!local_keys_[code]) { local_keys_[code] = true; ++local_key_count_; } }
    void seed_local_button(std::uint8_t button) { if (!local_buttons_[button]) { local_buttons_[button] = true; ++local_button_count_; } }
    Decision key(std::uint16_t code, bool down);
    Decision button(std::uint8_t button, bool down);
    Decision motion(std::uint8_t held_button, bool drag) const;
private:
    bool active_ = false;
    // Fixed arrays avoid allocator work from within a low-level hook callback.
    std::array<bool, 65536> local_keys_{}, remote_keys_{};
    std::array<bool, 256> local_buttons_{}, remote_buttons_{};
    std::size_t local_key_count_ = 0, remote_key_count_ = 0;
    std::size_t local_button_count_ = 0, remote_button_count_ = 0;
};
} // namespace hop::windows
