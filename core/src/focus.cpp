#include "hop/focus.h"

namespace hop {
std::uint64_t Focus::next_generation() {
    ++generation_;
    if (generation_ == 0) ++generation_;
    return generation_;
}
bool Focus::can_forward_to(const std::string& device_id) const {
    return phase_ == FocusPhase::remote && !device_id.empty() && remote_ == device_id;
}
std::optional<FocusCommand> Focus::switch_to(const std::string& device_id) {
    if (device_id.empty() || phase_ == FocusPhase::releasing || phase_ == FocusPhase::entering ||
        (phase_ == FocusPhase::remote && remote_ == device_id)) return std::nullopt;
    next_generation();
    pending_ = device_id;
    if (phase_ == FocusPhase::remote) {
        phase_ = FocusPhase::releasing;
        return FocusCommand{FocusAction::release_remote, generation_, remote_};
    }
    phase_ = FocusPhase::entering;
    return FocusCommand{FocusAction::enter_remote, generation_, pending_};
}
std::optional<FocusCommand> Focus::released(std::uint64_t generation, const std::string& device_id) {
    if (phase_ != FocusPhase::releasing || generation != generation_ || device_id != remote_)
        return std::nullopt;
    remote_.clear();
    phase_ = FocusPhase::entering;
    return FocusCommand{FocusAction::enter_remote, generation_, pending_};
}
bool Focus::entered(std::uint64_t generation, const std::string& device_id) {
    if (phase_ != FocusPhase::entering || generation != generation_ || device_id != pending_)
        return false;
    remote_ = pending_;
    pending_.clear();
    phase_ = FocusPhase::remote;
    return true;
}
FocusCommand Focus::abort() {
    const auto previous = remote_.empty() ? pending_ : remote_;
    next_generation();
    phase_ = FocusPhase::local;
    remote_.clear();
    pending_.clear();
    return FocusCommand{FocusAction::restore_local, generation_, previous};
}
} // namespace hop
