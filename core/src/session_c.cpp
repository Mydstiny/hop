#include "hop/session_c.h"
#include "hop/session.h"
#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <set>
#include <vector>
struct hop_v2_session {
    hop_v2_callbacks callbacks{}; void* user=nullptr;
    std::unique_ptr<hop::v2::Session> value;
    std::vector<std::uint8_t> pending;
    void wipe() { volatile std::uint8_t* p=pending.data(); for(std::size_t i=0;i<pending.size();++i)p[i]=0; pending.clear(); }
    ~hop_v2_session() { if(value && value->phase()!=hop::v2::SessionPhase::initial) { try { value->disconnect(); } catch (...) {} } wipe(); }
};
namespace {
hop::Platform platform(int p) { switch(p) {case 1:return hop::Platform::macos;case 2:return hop::Platform::windows;case 3:return hop::Platform::linux_desktop;default:return hop::Platform::harmonyos;} }
bool valid_auth(int value) { return value==0 || value==1; }
int result(bool ok) { return ok?0:-1; }
}
extern "C" hop_v2_session* hop_v2_create(int role,int p,uint64_t epoch,const uint64_t* screens,size_t count,uint64_t caps,hop_v2_callbacks cb,void* user) {
    if ((role!=1&&role!=2) || p<1 || p>4 || count>64 || (!screens&&count) || !cb.restore || (role==2 && (!cb.enter || !cb.input)) || (caps&~std::uint64_t(25)) || !(caps&1)) return nullptr;
    try {
        std::vector<std::uint64_t> ids; if(count) ids.assign(screens,screens+count);
        if (std::set<std::uint64_t>(ids.begin(),ids.end()).size()!=count || std::any_of(ids.begin(),ids.end(),[](auto id){return id==0;})) return nullptr;
        auto out=std::make_unique<hop_v2_session>(); out->callbacks=cb; out->user=user; auto* ctx=out.get();
        hop::v2::SessionCallbacks hooks;
        if(cb.enter) hooks.enter=[ctx](const hop::Frame& f){return ctx->callbacks.enter(ctx->user,f.payload.data(),f.payload.size(),f.epoch)==1;};
        if(cb.input) hooks.input=[ctx](const hop::Frame& f){return ctx->callbacks.input(ctx->user,f.payload.data(),f.payload.size(),f.epoch)==1;};
        hooks.restore=[ctx]{return ctx->callbacks.restore(ctx->user)==1;};
        // A previously peeked C-ABI frame is outside Session's queue. Drop it
        // too when focus is revoked, before notifying the platform actor.
        hooks.clipboard_revoke=[ctx]{ctx->wipe();if(ctx->callbacks.clipboard_revoke)ctx->callbacks.clipboard_revoke(ctx->user);};
        out->value=std::make_unique<hop::v2::Session>(role==1?hop::v2::SessionRole::host:hop::v2::SessionRole::receiver,epoch,std::move(ids),std::move(hooks),platform(p),caps);
        if(cb.leave && role==1) out->value->set_leave_handler([ctx](std::uint64_t s,std::uint16_t x,std::uint16_t y){return ctx->callbacks.leave(ctx->user,s,x,y)==1;});
        return out.release();
    } catch (...) { return nullptr; }
}
extern "C" hop_v2_session* hop_v2_create_input_only(int role,int platform,uint64_t epoch,const uint64_t* screens,size_t count,uint64_t caps,
    hop_v2_event_callback enter,hop_v2_event_callback input,hop_v2_restore_callback restore,hop_v2_leave_callback leave,void* user) {
    hop_v2_callbacks callbacks{};callbacks.enter=enter;callbacks.input=input;callbacks.restore=restore;callbacks.leave=leave;
    return hop_v2_create(role,platform,epoch,screens,count,caps,callbacks,user);
}
extern "C" int hop_v2_bind_peer(hop_v2_session* s,const char* fp) { if(!s||!fp||strnlen(fp,65)!=64)return -1;try{return result(s->value->bind_verified_peer(fp));}catch(...){s->value->disconnect();return -1;} }
extern "C" hop_v2_session* hop_v2_create_with_clipboard(int role,int platform,uint64_t epoch,const uint64_t* screens,size_t count,uint64_t caps,
    hop_v2_event_callback enter,hop_v2_event_callback input,hop_v2_restore_callback restore,hop_v2_leave_callback leave,
    hop_v2_clipboard_revoke_callback revoke,hop_v2_clipboard_sample_callback sample,hop_v2_clipboard_apply_callback apply,void* user) {
    if(!revoke||!sample||!apply)return nullptr;
    hop_v2_callbacks callbacks{};callbacks.enter=enter;callbacks.input=input;callbacks.restore=restore;callbacks.leave=leave;
    callbacks.clipboard_revoke=revoke;callbacks.clipboard_sample=sample;callbacks.clipboard_apply=apply;
    return hop_v2_create(role,platform,epoch,screens,count,caps,callbacks,user);
}
extern "C" int hop_v2_require_pairing(hop_v2_session* s) { if(!s)return -1;try{return result(s->value->require_pairing_confirmation());}catch(...){s->value->disconnect();return -1;} }
extern "C" int hop_v2_confirm_pairing(hop_v2_session* s) { if(!s)return -1;try{return result(s->value->confirm_pairing());}catch(...){s->value->disconnect();return -1;} }
extern "C" int hop_v2_cancel_pairing(hop_v2_session* s) { if(!s)return -1;try{return result(s->value->cancel_pairing());}catch(...){s->value->disconnect();return -1;} }
extern "C" int hop_v2_pairing_confirmed(const hop_v2_session* s) {return s && s->value->pairing_confirmed()?1:0;}
extern "C" int hop_v2_set_key_mode(hop_v2_session* s,int mode) {
    if(!s||(mode!=0&&mode!=1))return -1;
    try{return result(s->value->set_key_mode(mode?hop::KeyMode::common_shortcuts:hop::KeyMode::physical));}catch(...){s->value->disconnect();return -1;}
}
extern "C" int hop_v2_enable_clipboard(hop_v2_session* s,int text,int png) {
    if(!s || (text!=0&&text!=1) || (png!=0&&png!=1) || !s->callbacks.clipboard_sample || !s->callbacks.clipboard_apply || !s->callbacks.clipboard_revoke)return -1;
    try {
        return result(s->value->enable_clipboard([s]()->std::optional<std::uint64_t>{std::uint64_t n=0;return s->callbacks.clipboard_sample(s->user,&n)==1?std::optional<std::uint64_t>(n):std::nullopt;},
            [s](const hop::ClipboardValue& v,std::uint64_t base){return s->callbacks.clipboard_apply(s->user,static_cast<std::uint8_t>(v.format),v.epoch,v.transfer_id,v.data.data(),v.data.size(),base)==1;},text!=0,png!=0));
    } catch (...) {s->value->disconnect();return -1;}
}
extern "C" int hop_v2_start(hop_v2_session* s,int auth,uint64_t now) {if(!s||!valid_auth(auth))return -1;try{return result(s->value->start(auth!=0,now));}catch(...){s->value->disconnect();return -1;}}
extern "C" int hop_v2_receive(hop_v2_session* s,int auth,const uint8_t* bytes,size_t count,uint64_t now) {if(!s||!valid_auth(auth)||(!bytes&&count))return -1;try{return result(s->value->receive(auth!=0,bytes,count,now));}catch(...){s->value->disconnect();return -1;}}
extern "C" int hop_v2_tick(hop_v2_session* s,int auth,uint64_t now) {if(!s||!valid_auth(auth))return -1;try{return result(s->value->tick(auth!=0,now));}catch(...){s->value->disconnect();return -1;}}
extern "C" int hop_v2_pull(hop_v2_session* s,int auth,uint8_t* output,size_t capacity,size_t* length) {
    if(length)*length=0;
    if(!s||!length||!valid_auth(auth)||(!output&&capacity))return -1;
    try {
        if(!auth || s->value->phase()==hop::v2::SessionPhase::failed) {s->wipe();s->value->disconnect();return -1;}
        if(s->pending.empty() && !s->value->take(true,s->pending)) {s->wipe();return -1;}
        if(s->pending.empty())return 0;
        *length=s->pending.size(); if(capacity<s->pending.size())return 1;
        std::copy(s->pending.begin(),s->pending.end(),output);s->wipe();return 1;
    } catch (...) {s->wipe();s->value->disconnect();return -1;}
}
extern "C" int hop_v2_advertise(hop_v2_session* s,const uint8_t* p,size_t n){if(!s||!p||n>4096)return -1;try{return result(s->value->advertise_screens({p,p+n}));}catch(...){s->value->disconnect();return -1;}}
extern "C" int hop_v2_enter(hop_v2_session* s,uint64_t e,uint64_t screen,uint16_t x,uint16_t y){if(!s)return -1;try{return result(s->value->enter(e,screen,x,y));}catch(...){s->value->disconnect();return -1;}}
extern "C" int hop_v2_release(hop_v2_session* s,uint8_t reason){if(!s)return -1;try{if(!s->pending.empty()){s->wipe();s->value->disconnect();return -1;}return result(s->value->release(reason));}catch(...){s->value->disconnect();return -1;}}
extern "C" int hop_v2_complete_release(hop_v2_session* s,uint64_t now){if(!s)return -1;try{return result(s->value->complete_release(now));}catch(...){s->value->disconnect();return -1;}}
extern "C" int hop_v2_request_leave(hop_v2_session* s,uint16_t x,uint16_t y){if(!s)return -1;try{return result(s->value->request_leave(x,y));}catch(...){s->value->disconnect();return -1;}}
extern "C" int hop_v2_send_input(hop_v2_session* s,const uint8_t* p,size_t n){if(!s||!p||n>4096)return -1;try{return result(s->value->send_input({p,p+n}));}catch(...){s->value->disconnect();return -1;}}
extern "C" int hop_v2_clipboard_ready(hop_v2_session* s){if(!s)return -1;try{return result(s->value->clipboard_ready());}catch(...){s->value->disconnect();return -1;}}
extern "C" int hop_v2_clipboard_pause(hop_v2_session* s){if(!s)return -1;try{return result(s->value->clipboard_pause());}catch(...){s->value->disconnect();return -1;}}
extern "C" uint64_t hop_v2_clipboard_formats(const hop_v2_session* s){return s?s->value->clipboard_formats():0;}
extern "C" int hop_v2_clipboard_available(const hop_v2_session* s){return s&&s->value->clipboard_available()?1:0;}
extern "C" int hop_v2_clipboard_initial_done(hop_v2_session* s){if(!s)return -1;try{return result(s->value->clipboard_initial_done());}catch(...){s->value->disconnect();return -1;}}
extern "C" int hop_v2_clipboard_initial_received(const hop_v2_session* s){return s&&s->value->clipboard_initial_received()?1:0;}
extern "C" int hop_v2_clipboard_initial_drained(hop_v2_session* s){if(!s)return -1;try{return result(s->value->clipboard_initial_drained());}catch(...){s->value->disconnect();return -1;}}
extern "C" int hop_v2_clipboard_start(hop_v2_session* s,uint64_t id,uint8_t f,uint32_t total){if(!s||(f!=1&&f!=2))return -1;try{return result(s->value->clipboard_start(id,static_cast<hop::ClipboardFormat>(f),total));}catch(...){s->value->disconnect();return -1;}}
extern "C" int hop_v2_clipboard_chunk(hop_v2_session* s,uint64_t id,const uint8_t* p,size_t n){if(!s||!p)return -1;try{return result(s->value->clipboard_chunk(id,p,n));}catch(...){s->value->disconnect();return -1;}}
extern "C" int hop_v2_clipboard_cancel(hop_v2_session* s,uint64_t id,uint8_t reason){if(!s)return -1;try{return result(s->value->clipboard_cancel(id,reason));}catch(...){s->value->disconnect();return -1;}}
extern "C" size_t hop_v2_peer_screens(const hop_v2_session* s,hop_v2_screen* output,size_t capacity){
    if(!s||(!output&&capacity))return 0;
    try {const auto screens=s->value->peer_screen_snapshot();if(capacity>=screens.size()&&output){for(std::size_t i=0;i<screens.size();++i)output[i]={screens[i].id,screens[i].x,screens[i].y,screens[i].width,screens[i].height};}return screens.size();}catch(...){return 0;}
}
extern "C" int hop_v2_peer_screen_at(const hop_v2_session* s,size_t index,uint64_t* id,int32_t* x,int32_t* y,uint32_t* width,uint32_t* height) {
    if(!s||!id||!x||!y||!width||!height)return -1;
    try {const auto screens=s->value->peer_screen_snapshot();if(index>=screens.size())return -1;const auto& v=screens[index];
        *id=v.id;*x=v.x;*y=v.y;*width=v.width;*height=v.height;return 0;}catch(...){return -1;}
}
extern "C" int hop_v2_phase(const hop_v2_session* s){return s?static_cast<int>(s->value->phase()):-1;}
extern "C" uint64_t hop_v2_focus_epoch(const hop_v2_session* s){return s?s->value->focus_epoch():0;}
extern "C" void hop_v2_disconnect(hop_v2_session* s){if(s){s->wipe();s->value->disconnect();}}
extern "C" void hop_v2_free(hop_v2_session* s){delete s;}
