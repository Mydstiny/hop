#pragma once

#include <cstdint>
#include <optional>

namespace hop {
enum class Platform { macos, windows, linux_desktop, harmonyos };
enum class KeyMode { physical, common_shortcuts };
// USB HID keyboard-page usage. This is a normalized key identity, not text.
struct Chord {
    std::uint16_t usage;
    std::uint16_t modifiers;
};
enum Modifier : std::uint16_t {
    left_shift = 1u << 0, right_shift = 1u << 1,
    left_control = 1u << 2, right_control = 1u << 3,
    left_alt = 1u << 4, right_alt = 1u << 5,
    left_meta = 1u << 6, right_meta = 1u << 7, caps_lock = 1u << 8
};
// Translate only explicit common chords; all other physical identities pass
// through unchanged. Event sequencing, modifier release, and OS keycode mapping
// are separate responsibilities of the future input adapter.
Chord map_chord(Platform source, Platform target, KeyMode mode, Chord chord);

struct PointerSettings {
    double movement_scale = 1;
    double horizontal_scroll_scale = 1;
    double vertical_scroll_scale = 1;
    bool reverse_horizontal = false;
    bool reverse_vertical = false;
};
struct Delta { double x; double y; };
// Settings are accepted only in [0.1, 10]; untrusted non-finite or unusually
// large deltas are rejected, not silently clamped into a potentially unsafe move.
bool valid_pointer_settings(const PointerSettings& settings);
std::optional<Delta> map_motion(const PointerSettings& settings, Delta input);
std::optional<Delta> map_scroll(const PointerSettings& settings, Delta input);
} // namespace hop
