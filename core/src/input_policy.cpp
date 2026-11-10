#include "hop/input_policy.h"
#include <cmath>

namespace hop {
namespace {
constexpr std::uint16_t controls = left_control | right_control;
constexpr std::uint16_t shifts = left_shift | right_shift;
constexpr std::uint16_t alts = left_alt | right_alt;
constexpr std::uint16_t metas = left_meta | right_meta;
constexpr std::uint16_t all_modifiers = controls | shifts | alts | metas | caps_lock;
bool mac(Platform p) { return p == Platform::macos; }
bool valid_scale(double scale) { return std::isfinite(scale) && scale >= .1 && scale <= 10.; }
bool valid_delta(Delta d) {
    return std::isfinite(d.x) && std::isfinite(d.y) &&
           std::abs(d.x) <= 1000000 && std::abs(d.y) <= 1000000;
}
bool common_key(std::uint16_t usage) {
    // A, C, F, S, V, X, Z: common select-all/edit/find/save shortcuts.
    return usage == 0x04 || usage == 0x06 || usage == 0x09 || usage == 0x16 ||
           usage == 0x19 || usage == 0x1b || usage == 0x1d;
}
}
Chord map_chord(Platform source, Platform target, KeyMode mode, Chord chord) {
    if (mode != KeyMode::common_shortcuts || mac(source) == mac(target) ||
        !common_key(chord.usage) || (chord.modifiers & ~all_modifiers) ||
        (chord.modifiers & alts)) return chord;
    auto src = mac(source) ? metas : controls;
    auto other = mac(source) ? controls : metas;
    if (!(chord.modifiers & src) || (chord.modifiers & other)) return chord;
    // Shift is only included for the commonly used redo chord. Preserve other
    // shifted app-specific shortcuts rather than guessing their meaning.
    if ((chord.modifiers & shifts) && chord.usage != 0x1d) return chord;
    const bool left = chord.modifiers & (mac(source) ? left_meta : left_control);
    const bool right = chord.modifiers & (mac(source) ? right_meta : right_control);
    chord.modifiers &= static_cast<std::uint16_t>(~src);
    if (mac(target)) {
        if (left) chord.modifiers |= left_meta;
        if (right) chord.modifiers |= right_meta;
    } else {
        if (left) chord.modifiers |= left_control;
        if (right) chord.modifiers |= right_control;
    }
    return chord;
}
bool valid_pointer_settings(const PointerSettings& s) {
    return valid_scale(s.movement_scale) && valid_scale(s.horizontal_scroll_scale) &&
           valid_scale(s.vertical_scroll_scale);
}
std::optional<Delta> map_motion(const PointerSettings& s, Delta input) {
    if (!valid_pointer_settings(s) || !valid_delta(input)) return std::nullopt;
    return Delta{input.x * s.movement_scale, input.y * s.movement_scale};
}
std::optional<Delta> map_scroll(const PointerSettings& s, Delta input) {
    if (!valid_pointer_settings(s) || !valid_delta(input)) return std::nullopt;
    return Delta{input.x * s.horizontal_scroll_scale * (s.reverse_horizontal ? -1 : 1),
                 input.y * s.vertical_scroll_scale * (s.reverse_vertical ? -1 : 1)};
}
} // namespace hop
