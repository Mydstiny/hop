#include "hop/shortcut_translator.h"
#include <algorithm>
namespace hop {
std::uint16_t ShortcutTranslator::bit(std::uint16_t usage) {
    switch (usage) {
    case 0xe0: return left_control; case 0xe1: return left_shift;
    case 0xe2: return left_alt; case 0xe3: return left_meta;
    case 0xe4: return right_control; case 0xe5: return right_shift;
    case 0xe6: return right_alt; case 0xe7: return right_meta;
    default: return 0;
    }
}
std::uint16_t ShortcutTranslator::usage_for_bit(std::uint16_t b) {
    switch (b) {
    case left_control: return 0xe0; case left_shift: return 0xe1; case left_alt: return 0xe2; case left_meta: return 0xe3;
    case right_control: return 0xe4; case right_shift: return 0xe5; case right_alt: return 0xe6; case right_meta: return 0xe7;
    default: return 0;
    }
}
ShortcutTranslator::ShortcutTranslator(Platform source, Platform target, KeyMode mode) : source_(source), target_(target), mode_(mode) {
    if (mode == KeyMode::common_shortcuts && (source == Platform::macos) != (target == Platform::macos))
        main_mask_ = source == Platform::macos ? left_meta | right_meta : left_control | right_control;
}
std::uint16_t ShortcutTranslator::target_usage(std::uint16_t usage) const {
    if (source_ == Platform::macos) return usage == 0xe3 ? 0xe0 : usage == 0xe7 ? 0xe4 : usage;
    return usage == 0xe0 ? 0xe3 : usage == 0xe4 ? 0xe7 : usage;
}
void ShortcutTranslator::emit(std::vector<KeyEvent>& out, std::uint16_t usage, bool down, bool repeat) {
    const auto b = bit(usage);
    if (b) { if (down) output_modifiers_ |= b; else output_modifiers_ &= static_cast<std::uint16_t>(~b); }
    out.push_back({usage,output_modifiers_,down,repeat});
}
void ShortcutTranslator::decide(std::vector<KeyEvent>& out, Decision choice) {
    decision_ = choice;
    for (auto usage : deferred_) emit(out, choice == Decision::mapped ? target_usage(usage) : usage, true);
    deferred_.clear();
}
std::optional<std::vector<KeyEvent>> ShortcutTranslator::push(KeyEvent input) {
    if (failed_) return std::nullopt;
    const auto b = bit(input.usage);
    if (!input.usage || input.usage > 0xe7 || (input.modifiers & ~std::uint16_t(0x1ff)) ||
        (input.repeat && (!input.down || b)) ||
        (b && (bool(source_modifiers_ & b) == input.down || bool(input.modifiers & b) != input.down))) {
        failed_ = true; return std::nullopt;
    }
    const auto expected = b ? (input.down ? source_modifiers_ | b : source_modifiers_ & ~b) : source_modifiers_;
    if ((input.modifiers & 0xff) != expected) { failed_ = true; return std::nullopt; }
    if (!b && ((input.down && !input.repeat && held_keys_.count(input.usage)) ||
               (input.down && input.repeat && !held_keys_.count(input.usage)) ||
               (!input.down && !held_keys_.count(input.usage)))) { failed_ = true; return std::nullopt; }
    std::vector<KeyEvent> out;
    try {
        output_modifiers_ = (output_modifiers_ & ~std::uint16_t(caps_lock)) | (input.modifiers & caps_lock);
        if (b) {
            source_modifiers_ = static_cast<std::uint16_t>(expected);
            if (b & main_mask_) {
                if (input.down) {
                    if (decision_ == Decision::undecided) deferred_.push_back(input.usage);
                    else emit(out, decision_ == Decision::mapped ? target_usage(input.usage) : input.usage, true);
                } else {
                    if (decision_ == Decision::undecided) decide(out,Decision::physical);
                    emit(out, decision_ == Decision::mapped ? target_usage(input.usage) : input.usage, false);
                    if (!(source_modifiers_ & main_mask_)) decision_ = Decision::undecided;
                }
            } else {
                // Alt or the opposite main modifier prevents semantic mapping.
                const auto incompatible = source_ == Platform::macos ? left_control | right_control | left_alt | right_alt : left_meta | right_meta | left_alt | right_alt;
                if (input.down && decision_ == Decision::undecided && !deferred_.empty() && (b & incompatible)) decide(out,Decision::physical);
                emit(out,input.usage,input.down);
            }
        } else {
            if (input.down && !input.repeat) held_keys_.insert(input.usage);
            if (!input.down) held_keys_.erase(input.usage);
            if (decision_ == Decision::undecided && !deferred_.empty()) {
                const auto mapped = map_chord(source_,target_,mode_,{input.usage,input.modifiers});
                decide(out,input.down && !input.repeat && mapped.modifiers != input.modifiers ? Decision::mapped : Decision::physical);
            }
            emit(out,input.usage,input.down,input.repeat);
        }
        return out;
    } catch (...) { failed_ = true; return std::nullopt; }
}
std::vector<KeyEvent> ShortcutTranslator::flush_for_pointer() {
    std::vector<KeyEvent> out;
    if (!failed_ && decision_ == Decision::undecided && !deferred_.empty()) decide(out,Decision::physical);
    return out;
}
std::vector<KeyEvent> ShortcutTranslator::release_all() {
    std::vector<KeyEvent> out;
    // Deferred modifiers were never sent and must not trigger a launcher here.
    deferred_.clear();
    for (auto key : held_keys_) emit(out,key,false);
    held_keys_.clear();
    for (std::uint16_t b=1;b<=right_meta;b=static_cast<std::uint16_t>(b<<1))
        if (output_modifiers_ & b) emit(out,usage_for_bit(b),false);
    source_modifiers_ = 0; output_modifiers_ = 0; decision_ = Decision::undecided;
    return out;
}
} // namespace hop
