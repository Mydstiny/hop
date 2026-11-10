#include "hop_windows/pairing_worker.h"
#include "hop/secure_channel_c.h"
#include <algorithm>
#include <limits>
namespace hop::windows {
PairingWorker::PairingWorker(DeviceIdentity& id,TrustStore& store,SessionRole role,std::string name,std::vector<std::uint64_t> screens,std::function<SessionCallbacks()> callbacks,std::function<bool(Session&)> configure)
 :identity_(id),store_(store),role_(role),name_(std::move(name)),screens_(std::move(screens)),callbacks_(std::move(callbacks)),configure_session_(std::move(configure)) {}
PairingWorker::~PairingWorker(){cancel();}
bool PairingWorker::begin_listening(const TcpAddress& address,std::uint64_t now) {
    if(state_!=PairingState::idle || !callbacks_ || now>std::numeric_limits<std::uint64_t>::max()-120000)return false;
    return begin_listening(LanServer::start(address,true),now);
}
bool PairingWorker::begin_listening(std::shared_ptr<LanServer> listener,std::uint64_t now) {
    if(state_!=PairingState::idle || !callbacks_ || !listener || !listener->open_pairing_window(now,120000))return false;
    server_=std::move(listener);
    started_=last_now_=now;state_=PairingState::waiting_candidate;return true;
}
bool PairingWorker::begin_connecting(const TcpAddress& address,std::uint64_t now) {
    if(state_!=PairingState::idle || !callbacks_ || now>std::numeric_limits<std::uint64_t>::max()-120000)return false;
    remote_=address;dialer_=TcpDialer::start(address,true,now);if(!dialer_)return false;
    outgoing_=true;started_=last_now_=now;state_=PairingState::waiting_candidate;return true;
}
bool PairingWorker::local_address(TcpAddress& address) const {return server_ && server_->local_address(address);}
bool PairingWorker::start_candidate(SOCKET socket,std::uint64_t now) {
    exchange_=CandidateExchange::start(socket,true,outgoing_,role_==SessionRole::host?1:2,name_,identity_.certificate,now);
    return exchange_!=nullptr;
}
bool PairingWorker::make_session() {
    std::uint64_t epoch=0;
    if(role_==SessionRole::host) {
        if(hop_tls_random_bytes(reinterpret_cast<std::uint8_t*>(&epoch),sizeof(epoch))!=0)return false;
        epoch&=0x7fffffffffffffffULL;if(!epoch)return false;
    }
    next_focus_epoch_=epoch;session_=std::make_unique<Session>(role_,epoch,screens_,callbacks_());
    if(configure_session_ && !configure_session_(*session_)){session_.reset();return false;}return true;
}
std::vector<RemoteScreen> PairingWorker::peer_displays() const {
    return session_ && state_==PairingState::connected ? session_->peer_displays() : std::vector<RemoteScreen>{};
}
bool PairingWorker::request_focus(std::uint64_t screen,std::uint16_t x,std::uint16_t y) {
    if(state_!=PairingState::connected || role_!=SessionRole::host || !session_ || session_->phase()!=SessionPhase::ready || next_focus_epoch_==std::numeric_limits<std::uint64_t>::max())return false;
    return session_->enter(++next_focus_epoch_,screen,x,y);
}
bool PairingWorker::fail() {cancel();state_=PairingState::failed;return false;}
void PairingWorker::cancel() {
    if(review_)review_->cancel();path_.reset();session_.reset();exchange_.reset();dialer_.reset();
    if(server_)server_->close_pairing_window();server_.reset();review_.reset();
    local_confirmed_=false;if(state_!=PairingState::failed)state_=PairingState::idle;
}
bool PairingWorker::confirm_authenticated_pair(bool confirmed) {
    if(state_!=PairingState::confirming || !session_ || !path_ || !path_->authenticated() || local_confirmed_)return false;
    if(!confirmed){if(!session_->cancel_pairing())return fail();local_confirmed_=true;return true;}
    if(!session_->confirm_pairing())return fail();local_confirmed_=true;return true;
}
bool PairingWorker::step(std::uint64_t now) {
    if(state_==PairingState::idle || state_==PairingState::failed || now<last_now_ || (state_!=PairingState::connected && now-started_>=120000))return fail();
    last_now_=now;
    try {
        if(state_==PairingState::connected)return path_ && path_->step(now) ? true : fail();
        if(review_ && review_->cancelled())return fail();
        // The independent comparison can finish between worker ticks. Apply
        // that decision before polling an already queued selector.
        if(state_==PairingState::reviewing && review_) {
            VerifiedPeerCertificate peer;std::uint64_t expires=0;
            if(review_->approved_peer(now,peer,expires))state_=PairingState::connecting;
        }
        if(server_) {
            std::unique_ptr<ClassifiedSocket> accepted;if(!server_->poll(now,accepted))return fail();
            if(accepted) {
                if(accepted->kind()==ConnectionKind::candidate_offer && state_==PairingState::waiting_candidate && !exchange_ && !path_) {
                    if(!start_candidate(accepted->release(),now))return fail();
                } else if(accepted->kind()==ConnectionKind::trusted_selector && !exchange_ && !path_ &&
                          (state_==PairingState::waiting_candidate || (review_ && state_==PairingState::connecting))) {
                    if(!make_session())return fail();
                    path_=ConnectionPath::accept(accepted->release(),identity_,store_,review_ ? &*review_ : nullptr,*session_,true,now);
                    if(!path_){session_.reset();return fail();}
                }
            }
        }
        if(dialer_) {
            const auto progress=dialer_->step(now);if(progress==TcpProgress::failed)return fail();
            if(progress==TcpProgress::ready){const auto socket=dialer_->release();dialer_.reset();if(!start_candidate(socket,now))return fail();}
        }
        if(exchange_) {
            const auto progress=exchange_->step(now);if(progress==PairIo::failed)return fail();
            if(progress==PairIo::ready){auto offer=exchange_->take(now);exchange_.reset();if(!offer)return fail();review_=PairingReview::create(identity_.certificate,std::move(*offer),now);if(!review_)return fail();state_=PairingState::reviewing;}
        }
        if(state_==PairingState::connecting && outgoing_ && !path_ && now>=retry_after_) {
            if(!make_session())return fail();
            path_=ConnectionPath::connect_candidate(remote_,*review_,identity_,*session_,true,now);
            if(!path_){session_.reset();retry_after_=now+500;}
        }
        if(path_) {
            if(!path_->step(now)) {
                path_.reset();session_.reset();local_confirmed_=false;
                if(state_==PairingState::waiting_candidate)return true; // reject an unknown or stale pin
                if(state_==PairingState::confirming || !outgoing_)return fail();
                state_=PairingState::connecting;retry_after_=now+500;return true;
            }
            if(!review_) {
                if(session_->phase()==SessionPhase::pair_wait) {
                    // A saved pin can reconnect, but asking to pair again
                    // requires a fresh candidate and independent comparison.
                    path_.reset();session_.reset();return true;
                }
                if(session_->phase()==SessionPhase::ready && path_->authenticated() && !session_->peer_fingerprint().empty()) {
                    server_->close_pairing_window();server_.reset();state_=PairingState::connected;
                }
                return true;
            }
            if(session_->phase()==SessionPhase::pair_wait)state_=PairingState::confirming;
            if(session_->pairing_confirmed()) {
                VerifiedPeerCertificate peer;std::uint64_t expires=0;
                if(!review_->approved_peer(now,peer,expires) || !path_->transport() || !store_.remember(*session_,*path_->transport(),peer.pem))return fail();
                if(server_)server_->close_pairing_window();server_.reset();state_=PairingState::connected;
            }
        }
        return true;
    } catch(...) {return fail();}
}
}
