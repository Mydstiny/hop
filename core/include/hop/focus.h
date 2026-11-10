#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace hop {

// Pure routing policy. The platform adapter MUST restore local input on abort
// and release target input on disconnect; this class performs neither action.
enum class FocusPhase { local, releasing, entering, remote };
enum class FocusAction { none, release_remote, enter_remote, restore_local };
struct FocusCommand {
    FocusAction action;
    std::uint64_t generation;
    std::string device_id;
};

class Focus {
public:
    FocusPhase phase() const { return phase_; }
    const std::string& remote() const { return remote_; }
    // Input may be forwarded only while remote() identifies the acknowledged target.
    bool can_forward_to(const std::string& device_id) const;
    // A switch suspends forwarding immediately. A remote-to-remote switch first
    // asks the old target to release, then enters the new target after its ack.
    std::optional<FocusCommand> switch_to(const std::string& device_id);
    std::optional<FocusCommand> released(std::uint64_t generation, const std::string& device_id);
    bool entered(std::uint64_t generation, const std::string& device_id);
    // The caller sends best-effort release separately, then restores local input.
    FocusCommand abort();
private:
    std::uint64_t next_generation();
    FocusPhase phase_ = FocusPhase::local;
    std::uint64_t generation_ = 0;
    std::string remote_;
    std::string pending_;
};
} // namespace hop
