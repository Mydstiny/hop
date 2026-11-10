#pragma once
#include "hop/input_policy.h"
#include <cstdint>
#include <optional>
#include <set>
#include <vector>
namespace hop {
struct KeyEvent { std::uint16_t usage, modifiers; bool down, repeat; };
// Main-modifier transitions may be buffered until a chord is known. Call
// flush_for_pointer() before mouse/scroll events and release_all() at focus end.
// Single-threaded; on failure caller must stop forwarding and release the peer.
class ShortcutTranslator {
public:
    ShortcutTranslator(Platform source, Platform target, KeyMode mode);
    std::optional<std::vector<KeyEvent>> push(KeyEvent input);
    std::vector<KeyEvent> flush_for_pointer();
    std::vector<KeyEvent> release_all();
    bool failed() const { return failed_; }
private:
    enum class Decision { undecided, physical, mapped };
    static std::uint16_t bit(std::uint16_t usage);
    static std::uint16_t usage_for_bit(std::uint16_t bit);
    std::uint16_t target_usage(std::uint16_t usage) const;
    void emit(std::vector<KeyEvent>&, std::uint16_t usage, bool down, bool repeat=false);
    void decide(std::vector<KeyEvent>&, Decision);
    Platform source_, target_;
    KeyMode mode_;
    Decision decision_ = Decision::undecided;
    std::uint16_t source_modifiers_ = 0, output_modifiers_ = 0, main_mask_ = 0;
    std::vector<std::uint16_t> deferred_;
    std::set<std::uint16_t> held_keys_;
    bool failed_ = false;
};
}
