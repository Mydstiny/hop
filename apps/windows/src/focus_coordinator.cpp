#include "hop_windows/focus_coordinator.h"
namespace hop::windows {
FocusSwitchState FocusCoordinator::fail(std::uint64_t now) {
    clipboard_.revoke(now);if(previous_)previous_->disconnect();if(next_ && next_!=previous_)next_->disconnect();
    state_=FocusSwitchState::failed;return state_;
}
bool FocusCoordinator::begin(Session& previous,std::uint64_t now) {
    if(state_!=FocusSwitchState::idle || !local_drained_ || previous.phase()!=SessionPhase::active)return false;
    previous_=&previous;last_now_=started_=now;
    if(!previous.release(0)){fail(now);return false;}
    ticket_=clipboard_.revoke(now);
    if(!ticket_){fail(now);return false;}
    state_=FocusSwitchState::waiting;return true;
}
FocusSwitchState FocusCoordinator::step(std::uint64_t now) {
    if(state_!=FocusSwitchState::waiting && state_!=FocusSwitchState::preparing)return state_;
    if(now<last_now_ || now-started_>=2000 || clipboard_.timed_out(ticket_,now) || previous_->phase()==SessionPhase::failed)return fail(now<last_now_ ? last_now_ : now);
    last_now_=now;
    try {
        if(state_==FocusSwitchState::waiting && previous_->phase()==SessionPhase::ready && clipboard_.acknowledged(ticket_) && local_drained_())state_=FocusSwitchState::ready;
        else if(state_==FocusSwitchState::preparing && clipboard_.acknowledged(ticket_))state_=FocusSwitchState::prepared;
    } catch (...) {return fail(now);}
    return state_;
}
bool FocusCoordinator::prepare_next(Session& next,std::string peer,std::uint64_t epoch,std::uint32_t baseline,bool enabled,std::uint64_t now) {
    if(state_!=FocusSwitchState::ready || next.phase()!=SessionPhase::ready || !epoch || now<last_now_)return false;
    next_=&next;next_epoch_=epoch;last_now_=started_=now;
    if(!enabled){state_=FocusSwitchState::prepared;return true;}
    ticket_=clipboard_.activate(std::move(peer),epoch,baseline,now);
    if(!ticket_){fail(now);return false;}
    state_=FocusSwitchState::preparing;return true;
}
bool FocusCoordinator::enter_next(Session& next,std::uint64_t epoch,std::uint64_t screen,std::uint16_t x,std::uint16_t y) {
    if(state_!=FocusSwitchState::prepared || &next!=next_ || epoch!=next_epoch_)return false;
    if(!next.enter(epoch,screen,x,y)){fail(last_now_);return false;}
    previous_=next_=nullptr;ticket_=next_epoch_=0;state_=FocusSwitchState::idle;return true;
}
}
