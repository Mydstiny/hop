#pragma once
#include "hop_windows/clipboard_actor.h"
#include "hop_windows/session.h"
namespace hop::windows {
enum class FocusSwitchState { idle, waiting, ready, preparing, prepared, failed };
// Serial session worker: restores local input through Session::release first,
// requests clipboard revoke without blocking, and prevents the next ENTER
// until the previous RELEASE_ACK, clipboard barrier, local key drain, and
// optional activation of the new target clipboard have all completed.
class FocusCoordinator {
public:
    // local_drained must be a nonblocking snapshot: a separate control worker
    // finishes hook deactivation/drain and publishes when it is safe to enter.
    FocusCoordinator(ClipboardActor& clipboard,std::function<bool()> local_drained)
      : clipboard_(clipboard),local_drained_(std::move(local_drained)) {}
    bool begin(Session& previous,std::uint64_t now_ms);
    FocusSwitchState step(std::uint64_t now_ms);
    // When clipboard is enabled, configure the new generation on its worker
    // before ENTER, so an immediate clipboard START after ENTER_ACK is safe.
    bool prepare_next(Session& next,std::string peer,std::uint64_t epoch,std::uint32_t baseline,bool clipboard_enabled,std::uint64_t now_ms);
    bool enter_next(Session& next,std::uint64_t epoch,std::uint64_t screen,std::uint16_t x,std::uint16_t y);
    FocusSwitchState state() const {return state_;}
private:
    FocusSwitchState fail(std::uint64_t now_ms);
    ClipboardActor& clipboard_;std::function<bool()> local_drained_;Session* previous_=nullptr;Session* next_=nullptr;
    std::uint64_t ticket_=0,next_epoch_=0,last_now_=0,started_=0;
    FocusSwitchState state_=FocusSwitchState::idle;
};
}
