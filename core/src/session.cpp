#include "hop/session.h"
#include "hop/wire_validation.h"
#include <algorithm>
#include <limits>
#include <utility>
namespace hop::v2 {
namespace {
std::vector<std::uint8_t> prefix(std::uint8_t op) { return {1,op,0,0}; }
void u16(std::vector<std::uint8_t>& p, std::uint16_t n) { p.push_back(n >> 8); p.push_back(n & 0xff); }
void u32(std::vector<std::uint8_t>& p, std::uint32_t n) { for (int i=3;i>=0;--i) p.push_back(static_cast<std::uint8_t>(n >> (i*8))); }
void u64(std::vector<std::uint8_t>& p, std::uint64_t n) { for (int i=7;i>=0;--i) p.push_back(static_cast<std::uint8_t>(n >> (i*8))); }
std::uint16_t get16(const std::uint8_t* p) { return (std::uint16_t(p[0])<<8)|p[1]; }
std::uint32_t get32(const std::uint8_t* p) { std::uint32_t n=0; for (int i=0;i<4;++i) n=(n<<8)|p[i]; return n; }
std::uint64_t get64(const std::uint8_t* p) { std::uint64_t n=0; for (int i=0;i<8;++i) n=(n<<8)|p[i]; return n; }
std::int32_t getSigned32(const std::uint8_t* p) { const auto n=get32(p); return static_cast<std::int32_t>(n<=0x7fffffffU ? std::int64_t(n) : std::int64_t(n)-0x100000000LL); }
std::uint8_t platform_code(hop::Platform p) {
    switch (p) { case hop::Platform::macos: return 1; case hop::Platform::windows: return 2; case hop::Platform::linux_desktop: return 3; case hop::Platform::harmonyos: return 4; }
    return 0;
}
bool has(const std::vector<std::uint64_t>& screens, std::uint64_t id) { return std::find(screens.begin(),screens.end(),id)!=screens.end(); }
}
Session::Session(SessionRole role, std::uint64_t epoch, std::vector<std::uint64_t> screens, SessionCallbacks callbacks, hop::Platform platform, std::uint64_t capabilities)
 : role_(role), platform_(platform), session_epoch_(epoch), local_caps_(capabilities), local_screens_(std::move(screens)), callbacks_(std::move(callbacks)),
   decoder_([this](Frame&& f){ return deliver(std::move(f)); }) {}
void Session::revoke_clipboard() {
    clip_local_ready_=false;clip_remote_ready_=false;clip_paused_=true;
    clip_initial_done_=false;clip_initial_started_=false;clip_initial_drained_=false;initial_clipboard_baseline_.reset();
    queue_.discard_clipboard();clipboard_.focus(0);incoming_clip_id_=outgoing_clip_id_=0;clipboard_baseline_.reset();
    if(callbacks_.clipboard_revoke) { try { callbacks_.clipboard_revoke(); } catch (...) {} }
}
void Session::begin_clipboard_focus(std::optional<std::uint64_t> baseline) {
    clip_local_ready_=false;clip_remote_ready_=false;clip_paused_=false;clip_initial_done_=false;clip_initial_started_=false;clip_initial_drained_=false;
    initial_clipboard_baseline_=baseline;clipboard_.focus(focus_epoch_);
}
bool Session::fail() {
    if (phase_ != SessionPhase::failed) {
        phase_=SessionPhase::failed; pairing_confirmed_=false; translator_.reset(); revoke_clipboard(); keys_.fill(false); buttons_=0;
        if (callbacks_.restore) callbacks_.restore();
    }
    return false;
}
void Session::disconnect() { fail(); }
bool Session::enqueue(FrameKind kind, std::uint64_t epoch, std::vector<std::uint8_t> payload) {
    Frame f{kind,1,epoch,std::move(payload)};
    if (!valid_wire_payload(f)) {
        if (kind==FrameKind::clipboard_control || kind==FrameKind::clipboard_chunk) {
            volatile std::uint8_t* data=f.payload.data(); for (std::size_t i=0;i<f.payload.size();++i) data[i]=0;
        }
        return fail();
    }
    if (!queue_.enqueue(kind,epoch,std::move(f.payload))) return fail();
    return true;
}
bool Session::enable_clipboard(std::function<std::optional<std::uint64_t>()> sequence,
                               std::function<bool(const hop::ClipboardValue&,std::uint64_t)> apply, bool allow_text, bool allow_png) {
    if (phase_!=SessionPhase::initial || !sequence || !apply || !callbacks_.clipboard_revoke || (!allow_text && !allow_png) || clipboard_sequence_) return false;
    clipboard_sequence_=std::move(sequence); clipboard_apply_=std::move(apply);
    if (allow_text) local_caps_|=2; if (allow_png) local_caps_|=4; return true;
}
bool Session::bind_verified_peer(std::string fingerprint) {
    if (phase_!=SessionPhase::initial || !peer_fingerprint_.empty() || fingerprint.size()!=64 ||
        !std::all_of(fingerprint.begin(),fingerprint.end(),[](char c){return (c>='0'&&c<='9') || (c>='A'&&c<='F');})) return false;
    peer_fingerprint_=std::move(fingerprint); return true;
}
bool Session::require_pairing_confirmation() {
    if (phase_!=SessionPhase::initial) return false;
    local_caps_|=32; return true;
}
bool Session::confirm_pairing() {
    if (phase_!=SessionPhase::pair_wait || local_pair_confirmed_) return false;
    if (!enqueue(FrameKind::control,session_epoch_,prefix(4))) return false;
    local_pair_confirmed_=true;
    if (peer_pair_confirmed_) { pairing_confirmed_=true; phase_=SessionPhase::ready; }
    return true;
}
bool Session::cancel_pairing() {
    if (phase_!=SessionPhase::pair_wait) return false;
    if (!callbacks_.restore()) return fail();
    if (!enqueue(FrameKind::control,session_epoch_,prefix(5))) return false;
    phase_=SessionPhase::pair_cancel_wait; phase_started_=now_;
    return true;
}
bool Session::set_key_mode(hop::KeyMode mode) {
    if (phase_!=SessionPhase::initial || (mode!=hop::KeyMode::physical && mode!=hop::KeyMode::common_shortcuts)) return false;
    key_mode_=mode; return true;
}
bool Session::set_leave_handler(std::function<bool(std::uint64_t,std::uint16_t,std::uint16_t)> handler) {
    if (phase_!=SessionPhase::initial || role_!=SessionRole::host || !handler) return false;
    leave_handler_=std::move(handler); return true;
}
bool Session::start(bool authenticated, std::uint64_t now) {
    if (phase_ != SessionPhase::initial || !authenticated || !callbacks_.restore || !platform_code(platform_) || (local_caps_ & ~std::uint64_t(63)) || !(local_caps_ & 1) || ((local_caps_ & 6) && (!clipboard_sequence_ || !clipboard_apply_)) ||
        (role_==SessionRole::host && (!session_epoch_ || session_epoch_>std::numeric_limits<std::int64_t>::max())) ||
        (role_==SessionRole::receiver && session_epoch_) || ((local_caps_&32) && peer_fingerprint_.empty())) return fail();
    now_=last_received_=last_heartbeat_=now; phase_=SessionPhase::hello_wait;
    if (role_==SessionRole::host) {
        auto p=prefix(1); p.insert(p.end(),{1,platform_code(platform_),0,0}); u64(p,local_caps_);
        return enqueue(FrameKind::control,session_epoch_,std::move(p));
    }
    return true;
}
bool Session::receive(bool authenticated, const std::uint8_t* bytes, std::size_t count, std::uint64_t now) {
    if (!authenticated || phase_==SessionPhase::initial || phase_==SessionPhase::failed || now<now_) return fail();
    now_=now;
    if (!decoder_.feed(bytes,count)) return fail();
    return true;
}
bool Session::take(bool authenticated, std::vector<std::uint8_t>& encoded) {
    encoded.clear();
    if (!authenticated || phase_==SessionPhase::initial || phase_==SessionPhase::failed) return fail();
    auto next=queue_.pop(); if (!next) return true;
    try { encoded=encode_frame(*next); } catch (...) { encoded.clear(); }
    if (next->kind==FrameKind::clipboard_control || next->kind==FrameKind::clipboard_chunk) {
        volatile std::uint8_t* data=next->payload.data(); for (std::size_t i=0;i<next->payload.size();++i) data[i]=0;
    }
    return !encoded.empty() || fail();
}
bool Session::tick(bool authenticated, std::uint64_t now) {
    if (!authenticated || phase_==SessionPhase::initial || phase_==SessionPhase::failed || now<now_) return fail();
    now_=now;
    if (now-last_received_>=3000 || ((phase_==SessionPhase::enter_wait || phase_==SessionPhase::release_wait || phase_==SessionPhase::pair_cancel_wait) && now-phase_started_>=3000) ||
        ((phase_==SessionPhase::release_ack_wait || (phase_==SessionPhase::release_wait && !release_local_drained_)) && now-phase_started_>=2000) ||
        (phase_==SessionPhase::pair_wait && now-phase_started_>=120000)) return fail();
    if (phase_!=SessionPhase::hello_wait && phase_!=SessionPhase::pair_cancel_wait && now-last_heartbeat_>=1000) {
        auto p=prefix(0x1e); u64(p,now); if (!enqueue(FrameKind::control,session_epoch_,std::move(p))) return false;
        last_heartbeat_=now;
    }
    return true;
}
bool Session::advertise_screens(std::vector<std::uint8_t> payload) {
    if (role_!=SessionRole::receiver || phase_!=SessionPhase::ready) return false;
    Frame f{FrameKind::control,1,session_epoch_,payload};
    if (!valid_wire_payload(f) || payload[1]!=3 || get16(payload.data()+4)!=local_screens_.size()) return false;
    for (std::size_t i=0;i<local_screens_.size();++i) if (!has(local_screens_,get64(payload.data()+8+i*24))) return false;
    return enqueue(FrameKind::control,session_epoch_,std::move(payload));
}
bool Session::enter(std::uint64_t epoch, std::uint64_t screen, std::uint16_t x, std::uint16_t y) {
    if (role_!=SessionRole::host || phase_!=SessionPhase::ready || !has(peer_screens_,screen) || epoch<=session_epoch_ || epoch<=last_focus_) return false;
    auto p=prefix(0x10); u64(p,screen); u16(p,x); u16(p,y); u16(p,0); p.push_back(0); p.push_back(0);
    focus_epoch_=last_focus_=epoch; screen_=screen; translator_.emplace(platform_,peer_platform_,key_mode_); phase_=SessionPhase::enter_wait; phase_started_=now_;
    return enqueue(FrameKind::control,epoch,std::move(p));
}
bool Session::release(std::uint8_t reason) {
    if (role_!=SessionRole::host || phase_!=SessionPhase::active || reason>2) return false;
    // Revoke clipboard work before restoring ownership, without waiting for its actor.
    revoke_clipboard();
    if (!callbacks_.restore()) return fail();
    queue_.discard_input(focus_epoch_); translator_.reset(); phase_=SessionPhase::release_wait; phase_started_=now_;
    release_local_drained_=!(negotiated_caps_&6); release_remote_acked_=false;
    auto p=prefix(0x12); p.insert(p.end(),{reason,0,0,0});
    return enqueue(FrameKind::control,focus_epoch_,std::move(p));
}
bool Session::complete_release(std::uint64_t now) {
    if (now<now_) return fail();
    if ((phase_==SessionPhase::release_ack_wait || (phase_==SessionPhase::release_wait && !release_local_drained_)) && now-phase_started_>=2000) return fail();
    now_=now;
    if (role_==SessionRole::receiver && phase_==SessionPhase::release_ack_wait) {
        if (!enqueue(FrameKind::control,focus_epoch_,prefix(0x13))) return false;
        focus_epoch_=0; phase_=SessionPhase::ready; return true;
    }
    if (role_==SessionRole::host && phase_==SessionPhase::release_wait && !release_local_drained_) {
        release_local_drained_=true;
        if (release_remote_acked_) { focus_epoch_=0; keys_.fill(false); buttons_=0; phase_=SessionPhase::ready; }
        return true;
    }
    return false;
}
bool Session::request_leave(std::uint16_t x, std::uint16_t y) {
    if (role_!=SessionRole::receiver || phase_!=SessionPhase::active) return false;
    auto p=prefix(0x14); u64(p,screen_); u16(p,x); u16(p,y);
    return enqueue(FrameKind::control,focus_epoch_,std::move(p));
}
bool Session::transition_input(const Frame& f) {
    const auto& p=f.payload;
    if ((p[1]==1 || p[1]==2) && get64(p.data()+4)!=screen_) return false;
    // Optional event types must be supported by both peers.
    if ((p[1]==2 && p[16]>3 && !(negotiated_caps_&16)) ||
        (p[1]==3 && (p[4] || p[5] || p[6] || p[7]) && !(negotiated_caps_&8))) return false;
    if (p[1]==2) {
        const auto bit=std::uint8_t(1u<<(p[16]-1));
        if (bool(buttons_ & bit)==bool(p[17])) return false;
        const auto next=std::uint8_t(p[17] ? buttons_|bit : buttons_&~bit);
        if (next!=p[18]) return false;
        buttons_=next;
    } else if (p[1]==4) {
        const auto usage=get16(p.data()+4); const bool down=p[8]!=0, repeat=p[9]!=0;
        if (down ? (keys_[usage]!=repeat) : !keys_[usage]) return false;
        keys_[usage]=down;
    }
    return true;
}
bool Session::queue_key(const hop::KeyEvent& key) {
    auto p=prefix(4); u16(p,key.usage); u16(p,key.modifiers); p.push_back(key.down); p.push_back(key.repeat); u16(p,0);
    Frame f{FrameKind::input,1,focus_epoch_,p};
    if (!valid_wire_payload(f) || !transition_input(f)) return fail();
    return enqueue(FrameKind::input,focus_epoch_,std::move(p));
}
bool Session::send_input(std::vector<std::uint8_t> payload) {
    if (role_!=SessionRole::host || phase_!=SessionPhase::active || !translator_) return false;
    Frame f{FrameKind::input,1,focus_epoch_,payload};
    if (!valid_wire_payload(f)) return fail();
    try {
        if (payload[1]==4) {
            auto events=translator_->push({get16(payload.data()+4),get16(payload.data()+6),payload[8]!=0,payload[9]!=0});
            if (!events) return fail();
            for (const auto& event:*events) if (!queue_key(event)) return false;
            return true;
        }
        for (const auto& event:translator_->flush_for_pointer()) if (!queue_key(event)) return false;
        if (!transition_input(f)) return fail();
        return enqueue(FrameKind::input,focus_epoch_,std::move(payload));
    } catch (...) { return fail(); }
}
bool Session::clipboard_ready() {
    if(phase_!=SessionPhase::active || clip_paused_ || clip_local_ready_ || !(negotiated_caps_&6) || !clipboard_sequence_ || !clipboard_apply_) return false;
    if(!enqueue(FrameKind::control,focus_epoch_,prefix(0x15)))return false;
    clip_local_ready_=true;return true;
}
bool Session::clipboard_pause() {
    if(phase_!=SessionPhase::active || clip_paused_ || !(negotiated_caps_&6))return false;
    revoke_clipboard();
    return enqueue(FrameKind::control,focus_epoch_,prefix(0x16));
}
bool Session::clipboard_initial_done() {
    if(role_!=SessionRole::host || !clipboard_available() || clip_initial_done_ || outgoing_clip_id_)return false;
    if(!enqueue(FrameKind::clipboard_control,focus_epoch_,prefix(3)))return false;
    clip_initial_done_=true;return true;
}
bool Session::clipboard_initial_drained() {
    if(!clipboard_initial_received() || clip_initial_drained_)return false;
    clip_initial_drained_=true;return true;
}
bool Session::clipboard_start(std::uint64_t id, hop::ClipboardFormat format, std::uint32_t total) {
    if (!clipboard_available() || !id || id<=outgoing_last_id_ || outgoing_clip_id_) return false;
    if ((role_==SessionRole::receiver && (!clip_initial_done_ || !clip_initial_drained_)) || (role_==SessionRole::host && !clip_initial_done_ && clip_initial_started_)) return false;
    const auto bit = format==hop::ClipboardFormat::text ? 2u : format==hop::ClipboardFormat::png ? 4u : 0u;
    if (!bit || !(negotiated_caps_&bit)) return false;
    auto p=prefix(1); u64(p,id); p.push_back(static_cast<std::uint8_t>(format)); p.insert(p.end(),{0,0,0}); u32(p,total);
    Frame f{FrameKind::clipboard_control,1,focus_epoch_,p}; if (!valid_wire_payload(f)) return false;
    if (!enqueue(f.kind,focus_epoch_,std::move(p))) return false;
    if(role_==SessionRole::host && !clip_initial_done_)clip_initial_started_=true;
    outgoing_last_id_=id; outgoing_clip_id_=total ? id : 0; outgoing_total_=total; outgoing_offset_=0; return true;
}
bool Session::clipboard_chunk(std::uint64_t id, const std::uint8_t* bytes, std::size_t count) {
    if (!clipboard_available() || !id || id!=outgoing_clip_id_ || !bytes || !count || count>max_clipboard_chunk-16 || count>outgoing_total_-outgoing_offset_) return false;
    auto p=prefix(1); u64(p,id); u32(p,outgoing_offset_); p.insert(p.end(),bytes,bytes+count);
    if (!enqueue(FrameKind::clipboard_chunk,focus_epoch_,std::move(p))) return false;
    outgoing_offset_+=static_cast<std::uint32_t>(count); if (outgoing_offset_==outgoing_total_) outgoing_clip_id_=0;
    return true;
}
bool Session::clipboard_cancel(std::uint64_t id, std::uint8_t reason) {
    if (!clipboard_available() || !id || id!=outgoing_clip_id_) return false;
    auto p=prefix(2); u64(p,id); p.insert(p.end(),{reason,0,0,0});
    if (!enqueue(FrameKind::clipboard_control,focus_epoch_,std::move(p))) return false;
    outgoing_clip_id_=0; return true;
}
bool Session::finish_clipboard() {
    auto value=clipboard_.take(); if (!value) return true;
    incoming_clip_id_=0;
    bool ok=false;
    try { ok=clipboard_apply_ && clipboard_baseline_ && clipboard_apply_(*value,*clipboard_baseline_); } catch (...) { ok=false; }
    volatile std::uint8_t* data=value->data.data(); for (std::size_t i=0;i<value->data.size();++i) data[i]=0;
    clipboard_baseline_.reset(); return ok || fail();
}
bool Session::deliver(Frame&& f) {
    if (!valid_wire_payload(f)) return fail();
    const auto& p=f.payload; const auto op=p[1];
    if (phase_==SessionPhase::hello_wait) {
        if (f.kind!=FrameKind::control || (role_==SessionRole::host ? op!=2 || f.epoch!=session_epoch_ : op!=1 || f.epoch>std::numeric_limits<std::int64_t>::max())) return fail();
        switch (p[5]) {
        case 1: peer_platform_=hop::Platform::macos; break;
        case 2: peer_platform_=hop::Platform::windows; break;
        case 3: peer_platform_=hop::Platform::linux_desktop; break;
        case 4: peer_platform_=hop::Platform::harmonyos; break;
        default: return fail();
        }
        const auto remote_caps=get64(p.data()+8);
        negotiated_caps_=remote_caps&local_caps_&31;
        if (!(negotiated_caps_&1)) return fail();
        if (role_==SessionRole::receiver) {
            session_epoch_=f.epoch; auto ack=prefix(2); ack.insert(ack.end(),{2,platform_code(platform_),0,0}); u64(ack,local_caps_);
            if (!enqueue(FrameKind::control,session_epoch_,std::move(ack))) return false;
        }
        if ((remote_caps|local_caps_)&32) { if (peer_fingerprint_.empty()) return fail(); phase_=SessionPhase::pair_wait; phase_started_=now_; }
        else phase_=SessionPhase::ready;
        last_received_=now_; return true;
    }
    if (phase_==SessionPhase::pair_cancel_wait) return fail();
    if (phase_==SessionPhase::pair_wait) {
        if (f.kind!=FrameKind::control || f.epoch!=session_epoch_) return fail();
        if (op==4) {
            if (peer_pair_confirmed_) return fail();
            peer_pair_confirmed_=true;
            if (local_pair_confirmed_) { pairing_confirmed_=true; phase_=SessionPhase::ready; }
        } else if (op==5 || op==0x1f) return fail();
        else if (op!=0x1e) return fail();
        last_received_=now_; return true;
    }
    if (f.kind==FrameKind::control && op==3) {
        if (f.epoch!=session_epoch_ || phase_!=SessionPhase::ready || role_!=SessionRole::host) return fail();
        std::vector<std::uint64_t> ids; std::vector<PeerScreen> geometry;
        for (std::size_t i=0;i<get16(p.data()+4);++i) {
            const auto* screen=p.data()+8+i*24;
            ids.push_back(get64(screen));
            geometry.push_back({get64(screen),getSigned32(screen+8),getSigned32(screen+12),get32(screen+16),get32(screen+20)});
        }
        peer_screens_=std::move(ids); peer_geometry_=std::move(geometry);
    } else if (f.kind==FrameKind::control && op==0x1e) {
        if (f.epoch!=session_epoch_) return fail();
    } else if (f.kind==FrameKind::control && op==0x1f) {
        return fail();
    } else if (f.kind==FrameKind::control && op==0x10) {
        if (role_!=SessionRole::receiver || phase_!=SessionPhase::ready || f.epoch<=session_epoch_ || f.epoch<=last_focus_ || !has(local_screens_,get64(p.data()+4)) || get16(p.data()+16) || p[18] || !callbacks_.enter) return fail();
        std::optional<std::uint64_t> baseline;
        if(negotiated_caps_&6) { try { baseline=clipboard_sequence_(); } catch(...) { return fail(); } if(!baseline)return fail(); }
        if(!callbacks_.enter(f))return fail();
        focus_epoch_=last_focus_=f.epoch; screen_=get64(p.data()+4); keys_.fill(false); buttons_=0;
        auto ack=prefix(0x11); u64(ack,screen_); if (!enqueue(FrameKind::control,focus_epoch_,std::move(ack))) return false;
        phase_=SessionPhase::active; begin_clipboard_focus(baseline);
    } else if (f.kind==FrameKind::control && op==0x11) {
        if (role_!=SessionRole::host || phase_!=SessionPhase::enter_wait || f.epoch!=focus_epoch_ || get64(p.data()+4)!=screen_) return fail();
        keys_.fill(false); buttons_=0; phase_=SessionPhase::active; begin_clipboard_focus();
    } else if (f.kind==FrameKind::control && op==0x12) {
        if (role_!=SessionRole::receiver || phase_!=SessionPhase::active || f.epoch!=focus_epoch_) return fail();
        revoke_clipboard();
        if (!callbacks_.restore()) return fail();
        keys_.fill(false); buttons_=0;
        phase_=SessionPhase::release_ack_wait; phase_started_=now_;
        if (!(negotiated_caps_&6) && !complete_release(now_)) return fail();
    } else if (f.kind==FrameKind::control && op==0x13) {
        if (role_!=SessionRole::host || phase_!=SessionPhase::release_wait || f.epoch!=focus_epoch_ || release_remote_acked_) return fail();
        release_remote_acked_=true;
        if (release_local_drained_) { focus_epoch_=0; keys_.fill(false); buttons_=0; phase_=SessionPhase::ready; }
    } else if (f.kind==FrameKind::control && (op==0x15 || op==0x16)) {
        if (f.epoch<focus_epoch_ || (phase_!=SessionPhase::active && f.epoch<=last_focus_)) {last_received_=now_;return true;}
        if (phase_!=SessionPhase::active || f.epoch!=focus_epoch_ || !(negotiated_caps_&6)) return fail();
        if (op==0x16) { if(!clip_paused_)revoke_clipboard(); }
        else if (!clip_paused_) { if(clip_remote_ready_)return fail();clip_remote_ready_=true; }
    } else if (f.kind==FrameKind::control && op==0x14) {
        if (role_!=SessionRole::host || phase_!=SessionPhase::active || f.epoch!=focus_epoch_ || get64(p.data()+4)!=screen_ || !leave_handler_ || !leave_handler_(screen_,get16(p.data()+12),get16(p.data()+14))) return fail();
    } else if (f.kind==FrameKind::input) {
        if (role_!=SessionRole::receiver || phase_!=SessionPhase::active || f.epoch!=focus_epoch_ || !transition_input(f) || !callbacks_.input || !callbacks_.input(f)) return fail();
    } else if ((f.kind==FrameKind::clipboard_control || f.kind==FrameKind::clipboard_chunk) &&
               (phase_!=SessionPhase::active || f.epoch!=focus_epoch_ || !clipboard_available())) {
        // Previously in-flight data must not affect the current focus/actor.
        if(f.epoch<=last_focus_){last_received_=now_;return true;}return fail();
    } else if (f.kind==FrameKind::clipboard_control && op==1) {
        if (phase_!=SessionPhase::active || f.epoch!=focus_epoch_ || incoming_clip_id_ || !clipboard_sequence_ || !clipboard_apply_) return fail();
        if ((role_==SessionRole::host && !clip_initial_done_) || (role_==SessionRole::receiver && !clip_initial_done_ && clip_initial_started_)) return fail();
        const auto format=static_cast<hop::ClipboardFormat>(p[12]);
        const auto bit=format==hop::ClipboardFormat::text ? 2u : 4u;
        if (!(negotiated_caps_&bit)) return fail();
        if(role_==SessionRole::receiver && !clip_initial_done_) {clip_initial_started_=true;clipboard_baseline_=initial_clipboard_baseline_;}
        else clipboard_baseline_=clipboard_sequence_();
        if (!clipboard_baseline_) return fail();
        const auto id=get64(p.data()+4); if (!clipboard_.begin(f.epoch,id,format,get32(p.data()+16))) return fail();
        incoming_clip_id_=id; if (!finish_clipboard()) return false;
    } else if (f.kind==FrameKind::clipboard_control && op==2) {
        if (phase_!=SessionPhase::active || f.epoch!=focus_epoch_ || !incoming_clip_id_ || get64(p.data()+4)!=incoming_clip_id_) return fail();
        clipboard_.cancel(); incoming_clip_id_=0; clipboard_baseline_.reset();
    } else if (f.kind==FrameKind::clipboard_control && op==3) {
        if(role_!=SessionRole::receiver || !clipboard_available() || clip_initial_done_ || incoming_clip_id_)return fail();
        clip_initial_done_=true;initial_clipboard_baseline_.reset();
    } else if (f.kind==FrameKind::clipboard_chunk) {
        if (phase_!=SessionPhase::active || f.epoch!=focus_epoch_ || !incoming_clip_id_ || !clipboard_.chunk(f.epoch,get64(p.data()+4),get32(p.data()+12),p.data()+16,p.size()-16)) return fail();
        if (!finish_clipboard()) return false;
    } else return fail();
    last_received_=now_; return true;
}
} // namespace hop::v2
