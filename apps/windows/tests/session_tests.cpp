#include "hop_windows/session.h"
#include "hop/input_policy.h"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>
using namespace hop;
using namespace hop::windows;
namespace {
int checks=0;
void check(bool value) { ++checks; if (!value) { std::cerr<<"failed check "<<checks<<'\n'; std::abort(); } }
std::vector<std::uint8_t> screens() { return {1,3,0,0,0,1,0,0, 0,0,0,0,0,0,0,7, 0,0,0,0,0,0,0,0, 0,0,7,128,0,0,4,56}; }
std::vector<std::uint8_t> key(bool down, bool repeat=false) { return {1,4,0,0,0,6,0,0,std::uint8_t(down),std::uint8_t(repeat),0,0}; }
std::vector<std::uint8_t> move() { return {1,1,0,0,0,0,0,0,0,0,0,7,0,0,0,0}; }
bool deliver(Session& a, Session& b, std::uint64_t now=0) {
    std::vector<std::uint8_t> bytes; if (!a.take(true,bytes)) return false;
    if (bytes.empty()) return true;
    for (auto byte:bytes) if (!b.receive(true,&byte,1,now)) return false;
    return true;
}
struct Pair {
    int host_restore=0, recv_restore=0, enters=0, inputs=0, clip_applied=0; bool allow_restore=true, clips_enabled=false;
    std::uint32_t baseline=100, received_baseline=0; std::vector<std::uint8_t> clip_data;
    Session host, receiver;
    Pair() : host(SessionRole::host,42,{}, { {},{},[this]{ ++host_restore; return true; }}),
      receiver(SessionRole::receiver,0,{7},{[this](const Frame&){ ++enters; return true; },[this](const Frame&){ ++inputs; return true; },[this]{ ++recv_restore; return allow_restore; }}) {}
    void bind() { check(host.bind_verified_peer(std::string(64,'A'))); check(receiver.bind_verified_peer(std::string(64,'B'))); }
    void clips() {
        clips_enabled=true;auto sample=[this]{return baseline;};
        auto apply=[this](const hop::ClipboardValue& value,std::uint32_t sequence){++clip_applied; received_baseline=sequence; clip_data=value.data; return true;};
        check(host.enable_clipboard(sample,apply)); check(receiver.enable_clipboard(sample,apply));
    }
    void ready() {
        check(receiver.start(true,0)); check(host.start(true,0)); check(deliver(host,receiver)); check(deliver(receiver,host));
        check(receiver.advertise_screens(screens())); check(deliver(receiver,host));
        check(host.phase()==SessionPhase::ready); check(receiver.phase()==SessionPhase::ready);
    }
    void active() { ready(); check(host.enter(43,7,0,0)); check(!host.send_input(key(true))); check(deliver(host,receiver)); check(deliver(receiver,host)); check(host.phase()==SessionPhase::active); check(enters==1);
        if(clips_enabled){check(host.request_clipboard_ready() && receiver.request_clipboard_ready());check(deliver(host,receiver));check(deliver(receiver,host));check(host.clipboard_can_sync() && receiver.clipboard_can_sync());} }
};
}
int main() {
    static_assert(left_shift==1 && right_shift==2 && left_control==4 && right_control==8 && left_alt==16 && right_alt==32 && left_meta==64 && right_meta==128, "v2 modifier order");
    { Pair p; p.bind(); check(p.host.require_pairing_confirmation()); check(p.receiver.start(true,0)); check(p.host.start(true,0));
      check(deliver(p.host,p.receiver)); check(deliver(p.receiver,p.host));
      check(p.host.phase()==SessionPhase::pair_wait && p.receiver.phase()==SessionPhase::pair_wait);
      check(!p.receiver.advertise_screens(screens())); check(!p.host.enter(43,7,0,0));
      check(p.host.confirm_pairing()); check(!p.host.confirm_pairing()); check(deliver(p.host,p.receiver));
      check(!p.receiver.pairing_confirmed() && p.receiver.phase()==SessionPhase::pair_wait);
      check(p.receiver.confirm_pairing() && p.receiver.pairing_confirmed()); check(deliver(p.receiver,p.host));
      check(p.host.pairing_confirmed() && p.host.phase()==SessionPhase::ready); check(p.receiver.advertise_screens(screens())); check(deliver(p.receiver,p.host)); }
    { Pair p; p.bind(); check(p.receiver.require_pairing_confirmation()); check(p.receiver.start(true,0)); check(p.host.start(true,0));
      check(deliver(p.host,p.receiver)); check(deliver(p.receiver,p.host));
      check(p.host.phase()==SessionPhase::pair_wait && p.receiver.phase()==SessionPhase::pair_wait);
      auto early=encode_frame({FrameKind::control,2,42,screens()}); check(!p.host.receive(true,early.data(),early.size(),1)); check(p.host_restore==1); }
    { Pair p; p.bind(); check(p.host.require_pairing_confirmation()); check(p.receiver.start(true,0)); check(p.host.start(true,0));
      check(deliver(p.host,p.receiver)); check(deliver(p.receiver,p.host)); check(p.host.cancel_pairing());
      check(p.host.phase()==SessionPhase::pair_cancel_wait && p.host_restore==1); check(!deliver(p.host,p.receiver)); check(p.recv_restore==1); }
    { Pair p; p.bind(); check(p.host.require_pairing_confirmation()); check(p.receiver.start(true,0)); check(p.host.start(true,0));
      check(deliver(p.host,p.receiver)); check(deliver(p.receiver,p.host));
      auto cancel=encode_frame({FrameKind::control,2,42,{1,5,0,0}}); check(!p.host.receive(true,cancel.data(),cancel.size(),1)); check(p.host_restore==1 && !p.host.pairing_confirmed()); }
    { Pair p; p.bind(); check(p.host.require_pairing_confirmation()); check(p.receiver.start(true,0)); check(p.host.start(true,0));
      check(deliver(p.host,p.receiver)); check(deliver(p.receiver,p.host));
      auto first=encode_frame({FrameKind::control,2,42,{1,4,0,0}}); auto duplicate=encode_frame({FrameKind::control,3,42,{1,4,0,0}});
      check(p.host.receive(true,first.data(),first.size(),1)); check(!p.host.receive(true,duplicate.data(),duplicate.size(),2)); check(p.host_restore==1); }
    { Pair p; check(!p.host.start(false,0)); check(p.host_restore==1); }
    { Pair p; p.active(); check(p.host.send_input(key(true))); check(deliver(p.host,p.receiver)); check(p.inputs==1);
      check(p.host.send_input(key(true,true))); check(deliver(p.host,p.receiver));
      check(p.host.send_input(key(false))); check(deliver(p.host,p.receiver));
      check(p.host.send_input(move())); check(p.host.release(0)); check(p.host_restore==1);
      check(deliver(p.host,p.receiver)); check(p.recv_restore==1); check(p.inputs==3); check(deliver(p.receiver,p.host));
      check(p.host.phase()==SessionPhase::ready); check(!p.host.enter(43,7,0,0)); check(p.host.enter(44,7,0,0)); }
    { Pair p; p.active(); check(p.host.send_input(key(true))); check(!p.host.send_input(key(true))); check(p.host_restore==1); }
    { Pair p; p.active(); check(!p.host.send_input({1,3,0,0,0,2,0,0,0,0,0,0})); check(p.host_restore==1); }
    { Pair p; check(p.host.start(true,0));
      auto ack=encode_frame({FrameKind::control,1,42,{1,2,0,0,2,3,0,0,0,0,0,0,0,0,0,1}});
      auto displays=encode_frame({FrameKind::control,2,42,screens()});
      check(p.host.receive(true,ack.data(),ack.size(),0)); check(p.host.receive(true,displays.data(),displays.size(),0)); check(p.host.enter(43,7,0,0));
      auto entered=encode_frame({FrameKind::control,3,43,{1,0x11,0,0,0,0,0,0,0,0,0,7}});
      check(p.host.receive(true,entered.data(),entered.size(),0));
      check(!p.host.send_input({1,3,0,0,0,0,0,120,0,0,0,0})); check(p.host_restore==1); }
    { Pair p; p.active(); check(p.host.send_input({1,3,0,0,0,0,0,120,0,0,0,0})); check(deliver(p.host,p.receiver));
      check(p.host.send_input({1,2,0,0,0,0,0,0,0,0,0,7,0,0,0,0,4,1,8,0})); check(deliver(p.host,p.receiver)); check(p.inputs==2); }
    { Pair p; p.ready(); check(p.host.enter(43,7,0,0)); check(!p.host.tick(true,3000)); check(p.host_restore==1); }
    { Pair p; p.active(); check(!p.receiver.receive(false,nullptr,0,1)); check(p.recv_restore==1); }
    { Pair p; p.ready(); auto f=encode_frame({FrameKind::input,1,43,key(true)}); check(!p.receiver.receive(true,f.data(),f.size(),1)); check(p.recv_restore==1); }
    { Pair p; p.active(); auto f=encode_frame({FrameKind::input,3,44,key(true)}); check(!p.receiver.receive(true,f.data(),f.size(),1)); check(p.recv_restore==1); }
    { Pair p; p.active(); auto f=encode_frame({FrameKind::input,3,43,key(true)});
      check(p.receiver.receive(true,f.data(),f.size()-1,1)); check(p.inputs==0); p.receiver.disconnect(); check(p.inputs==0 && p.recv_restore==1); }
    { Pair p; p.ready(); auto f=encode_frame({FrameKind::input,2,43,key(true)}); f[8]=0; f[9]=0; f[10]=0x10; f[11]=1;
      check(!p.receiver.receive(true,f.data(),frame_header_size,1)); check(p.recv_restore==1); }
    { Pair p; p.active(); auto f=encode_frame({FrameKind::input,3,43,key(true)}); f[frame_header_size]=2;
      check(!p.receiver.receive(true,f.data(),f.size(),1)); check(p.inputs==0 && p.recv_restore==1); }
    { Pair p; check(p.host.start(true,0)); std::vector<std::uint8_t> out; check(p.host.take(true,out) && !out.empty());
      auto ack=encode_frame({FrameKind::control,1,42,{1,2,0,0,2,1,0,0,0,0,0,0,0,0,0,25}});
      auto displays=encode_frame({FrameKind::control,2,42,screens()});
      check(p.host.receive(true,ack.data(),ack.size(),0)); check(p.host.receive(true,displays.data(),displays.size(),0)); check(p.host.enter(43,7,0,0)); check(p.host.take(true,out) && !out.empty());
      auto entered=encode_frame({FrameKind::control,3,43,{1,0x11,0,0,0,0,0,0,0,0,0,7}}); check(p.host.receive(true,entered.data(),entered.size(),0));
      auto next=[&]{ hop::Frame f{}; check(p.host.take(true,out) && !out.empty()); hop::FrameDecoder d([&](hop::Frame&& value){f=std::move(value); return true;}); check(d.feed(out.data(),out.size()) && d.finish()); return f; };
      // Windows Ctrl+C becomes Mac Command+C; Caps Lock remains set.
      check(p.host.send_input({1,4,0,0,0,0xe0,1,4,1,0,0,0})); check(p.host.take(true,out) && out.empty());
      check(p.host.send_input({1,4,0,0,0,6,1,4,1,0,0,0})); auto meta=next(); auto c=next();
      check(meta.payload[5]==0xe3 && meta.payload[6]==1 && meta.payload[7]==0x40 && meta.payload[8]==1);
      check(c.payload[5]==6 && c.payload[6]==1 && c.payload[7]==0x40 && c.payload[8]==1);
      check(p.host.send_input({1,4,0,0,0,6,1,4,0,0,0,0})); c=next(); check(c.payload[5]==6 && c.payload[8]==0);
      check(p.host.send_input({1,4,0,0,0,0xe0,1,0,0,0,0,0})); meta=next(); check(meta.payload[5]==0xe3 && meta.payload[7]==0 && meta.payload[8]==0);
      // A pending Ctrl must be emitted physically before a mouse event.
      check(p.host.send_input({1,4,0,0,0,0xe0,1,4,1,0,0,0})); check(p.host.send_input(move()));
      auto ctrl=next(); auto pointer=next(); check(ctrl.payload[5]==0xe0 && pointer.payload[1]==1);
      check(!p.host.send_input({1,4,0,0,0,0xe0,1,4,1,0,0,0})); check(p.host_restore==1);
    }
    { Pair p; int leaves=0; check(p.host.set_leave_handler([&](std::uint64_t id,std::uint16_t x,std::uint16_t y){++leaves; return id==7 && x==65535 && y==123; }));
      p.active(); check(p.receiver.request_leave(65535,123)); check(deliver(p.receiver,p.host)); check(leaves==1 && p.host.phase()==SessionPhase::active); }
    { Pair p; p.clips(); p.active();
      check(p.host.clipboard_start(1,hop::ClipboardFormat::text,4));
      const std::uint8_t text[]={ 'H','o','p','!' }; check(p.host.clipboard_chunk(1,text,2)); check(p.host.clipboard_chunk(1,text+2,2));
      check(deliver(p.host,p.receiver)); check(deliver(p.host,p.receiver)); check(p.clip_applied==0); check(deliver(p.host,p.receiver));
      check(p.clip_applied==1 && p.received_baseline==100 && p.clip_data==std::vector<std::uint8_t>(text,text+4));
      check(!p.host.clipboard_start(1,hop::ClipboardFormat::text,0));
      check(!p.receiver.clipboard_start(2,hop::ClipboardFormat::text,0));check(p.host.send_clipboard_initial_done());check(deliver(p.host,p.receiver));
      check(!p.receiver.clipboard_start(2,hop::ClipboardFormat::text,0));check(p.receiver.mark_clipboard_initial_drained());check(!p.receiver.mark_clipboard_initial_drained());
      check(p.receiver.clipboard_start(2,hop::ClipboardFormat::text,0)); check(deliver(p.receiver,p.host)); check(p.clip_applied==2 && p.clip_data.empty()); }
    { Pair p;p.clips();p.active();p.baseline=101;const std::uint8_t text[]={'x'};
      check(p.host.clipboard_start(1,hop::ClipboardFormat::text,1) && !p.host.send_clipboard_initial_done());
      check(p.host.clipboard_chunk(1,text,1) && p.host.send_clipboard_initial_done());
      check(deliver(p.host,p.receiver));check(deliver(p.host,p.receiver));check(p.received_baseline==100 && !p.receiver.clipboard_initial_done());
      check(deliver(p.host,p.receiver) && p.receiver.clipboard_initial_done());check(!p.host.send_clipboard_initial_done()); }
    { Pair p; p.clips(); p.active();check(p.host.clipboard_start(1,hop::ClipboardFormat::text,4));
      check(p.host.pause_clipboard() && !p.host.clipboard_can_sync() && !p.host.request_clipboard_ready());
      check(deliver(p.host,p.receiver));check(!p.receiver.clipboard_can_sync() && !p.receiver.request_clipboard_ready() && p.clip_applied==0); }
    {
      int applied=0,restored=0;std::vector<std::uint8_t> received;
      Session receiver(SessionRole::receiver,0,{7},{[](const Frame&){return true;},[](const Frame&){return true;},[&]{++restored;return true;}});
      check(receiver.enable_clipboard([]{return std::uint32_t(10);},[&](const hop::ClipboardValue& value,std::uint32_t){++applied;received=value.data;return true;}));
      check(receiver.start(true,0));
      auto feed=[&](Frame frame){auto bytes=encode_frame(frame);return receiver.receive(true,bytes.data(),bytes.size(),1);};
      check(feed({FrameKind::control,1,42,{1,1,0,0,1,2,0,0,0,0,0,0,0,0,0,27}}));
      check(feed({FrameKind::control,2,43,{1,0x10,0,0,0,0,0,0,0,0,0,7,0,0,0,0,0,0,0,0}}));
      check(receiver.request_clipboard_ready());check(feed({FrameKind::control,3,43,{1,0x15,0,0}}));
      check(feed({FrameKind::clipboard_control,4,43,{1,1,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,2}}));
      check(feed({FrameKind::clipboard_chunk,5,43,{1,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,'x'}}));
      check(feed({FrameKind::control,6,43,{1,0x16,0,0}}));check(!receiver.clipboard_can_sync() && !receiver.request_clipboard_ready());
      check(feed({FrameKind::control,7,43,{1,0x15,0,0}}));
      check(feed({FrameKind::clipboard_chunk,8,43,{1,1,0,0,0,0,0,0,0,0,0,1,0,0,0,1,'z'}}) && applied==0);
      check(feed({FrameKind::control,9,43,{1,0x12,0,0,0,0,0,0}}));
      check(feed({FrameKind::control,10,44,{1,0x10,0,0,0,0,0,0,0,0,0,7,0,0,0,0,0,0,0,0}}));
      check(receiver.request_clipboard_ready());check(feed({FrameKind::control,11,44,{1,0x15,0,0}}));
      check(feed({FrameKind::clipboard_control,12,44,{1,1,0,0,0,0,0,0,0,0,0,2,1,0,0,0,0,0,0,2}}));
      check(feed({FrameKind::clipboard_chunk,13,43,{1,1,0,0,0,0,0,0,0,0,0,1,0,0,0,1,'z'}}));
      check(feed({FrameKind::clipboard_chunk,14,44,{1,1,0,0,0,0,0,0,0,0,0,2,0,0,0,0,'h','i'}}));
      check(applied==1 && received==std::vector<std::uint8_t>({'h','i'}) && restored==1);
    }
    { Pair p; p.clips(); p.active(); check(p.host.clipboard_start(1,hop::ClipboardFormat::text,4)); check(deliver(p.host,p.receiver));
      const std::uint8_t half[]={ 'h','i' }; check(p.host.clipboard_chunk(1,half,2)); check(p.host.release(0));
      check(deliver(p.host,p.receiver)); check(deliver(p.receiver,p.host)); check(p.clip_applied==0); }
    { Pair p; p.clips(); p.active(); check(p.host.clipboard_start(1,hop::ClipboardFormat::text,4));
      check(p.host.clipboard_cancel(1,0)); check(deliver(p.host,p.receiver)); check(deliver(p.host,p.receiver));
      check(p.clip_applied==0 && !p.host.clipboard_start(2,hop::ClipboardFormat::text,0));check(p.host.send_clipboard_initial_done() && deliver(p.host,p.receiver));
      check(p.host.clipboard_start(2,hop::ClipboardFormat::text,0)); check(deliver(p.host,p.receiver)); check(p.clip_applied==1); }
    { Pair p; p.clips(); p.active(); check(p.host.clipboard_start(1,hop::ClipboardFormat::text,4)); check(deliver(p.host,p.receiver));
      auto wrong=encode_frame({FrameKind::clipboard_chunk,5,43,{1,1,0,0,0,0,0,0,0,0,0,1,0,0,0,1,0x61}});
      check(!p.receiver.receive(true,wrong.data(),wrong.size(),1)); check(p.clip_applied==0 && p.recv_restore==1); }
    { Pair p; p.active(); check(!p.host.clipboard_start(1,hop::ClipboardFormat::text,0)); }
    { Pair p;bool ready=false;int begins=0;
      check(p.receiver.set_release_barrier([&](std::uint64_t){++begins;return true;},[&]{return ready;}));p.active();
      check(p.host.release(0));check(deliver(p.host,p.receiver));check(begins==1 && p.recv_restore==1 && p.receiver.phase()==SessionPhase::release_ack_wait);
      std::vector<std::uint8_t> bytes;check(p.receiver.take(true,bytes) && bytes.empty());check(p.receiver.tick(true,100));check(p.receiver.take(true,bytes) && bytes.empty());
      ready=true;check(p.receiver.tick(true,101));check(deliver(p.receiver,p.host));check(p.host.phase()==SessionPhase::ready && p.receiver.phase()==SessionPhase::ready); }
    { Pair p;check(p.receiver.set_release_barrier([](std::uint64_t){return true;},[]{return false;}));p.active();
      check(p.host.release(0));check(deliver(p.host,p.receiver));check(!p.receiver.tick(true,2000) && p.receiver.phase()==SessionPhase::failed); }
    { Pair p;p.active();check(p.host.release(0));check(deliver(p.host,p.receiver));std::vector<std::uint8_t> ack;
      check(p.receiver.take(true,ack) && !ack.empty());check(!p.host.receive(true,ack.data(),ack.size(),2000) && p.host.phase()==SessionPhase::failed); }
    { Pair p; p.active(); p.allow_restore=false; check(p.host.release(0)); check(!deliver(p.host,p.receiver)); check(p.receiver.phase()==SessionPhase::failed); }
    { Pair p; p.ready(); auto initial=p.host.peer_displays();check(initial.size()==1 && initial[0].id==7 && initial[0].width==1920 && initial[0].height==1080);
      auto updated=screens();updated[5]=2;updated.insert(updated.end(),{0,0,0,0,0,0,0,8,0xff,0xff,0xf8,0x80,0,0,0,0,0,0,7,128,0,0,4,56});
      auto message=encode_frame({FrameKind::control,3,42,updated});check(p.host.receive(true,message.data(),message.size(),1));
      auto displays=p.host.peer_displays();check(displays.size()==2 && displays[1].id==8 && displays[1].x==-1920 && displays[1].width==1920);
      displays[0].width=1;check(p.host.peer_displays()[0].width==1920); }
    { Pair p; p.ready(); check(!p.host.enter(43,8,0,0)); check(p.host.phase()==SessionPhase::ready); }
    std::cout<<"PASS "<<checks<<" session lifecycle checks; no system input or clipboard\n";
}
