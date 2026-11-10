#include "hop/session_c.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>
namespace {
int checks=0;
void check(bool value) { ++checks; if(!value){std::cerr<<"FAIL C session check "<<checks<<'\n';std::abort();} }
struct State { int enters=0, inputs=0, restores=0, clips=0; std::uint64_t baseline=0; std::vector<std::uint8_t> last; };
int enter(void* p,const uint8_t*,size_t,uint64_t){++static_cast<State*>(p)->enters;return 1;}
int input(void* p,const uint8_t* b,size_t n,uint64_t){auto* s=static_cast<State*>(p);++s->inputs;s->last.assign(b,b+n);return 1;}
int restore(void* p){++static_cast<State*>(p)->restores;return 1;}
int sample(void*,uint64_t* result){*result=0;return 1;}
int apply(void* p,uint8_t format,uint64_t epoch,uint64_t id,const uint8_t* data,size_t n,uint64_t baseline){
    auto* s=static_cast<State*>(p);++s->clips;s->baseline=baseline;
    if(n) s->last.assign(data,data+n); else s->last.clear();
    return format==1 && epoch==43 && id==1 ? 1 : 0;
}
std::vector<std::uint8_t> screens(){return {1,3,0,0,0,1,0,0,0,0,0,0,0,0,0,7,0,0,0,0,0,0,0,0,0,0,7,128,0,0,4,56};}
bool pump(hop_v2_session* from,hop_v2_session* to) {
    size_t n=0; int status=hop_v2_pull(from,1,nullptr,0,&n); if(status==0)return true; if(status!=1||!n)return false;
    std::vector<uint8_t> bytes(n); size_t received=0;
    if(hop_v2_pull(from,1,bytes.data(),bytes.size(),&received)!=1 || received!=n)return false;
    return hop_v2_receive(to,1,bytes.data(),bytes.size(),0)==0;
}
}
int main() {
    State a,b; hop_v2_callbacks host{};host.restore=restore;host.clipboard_sample=sample;host.clipboard_apply=apply;host.clipboard_revoke=[](void*){};
    hop_v2_callbacks receiver{};receiver.enter=enter;receiver.input=input;receiver.restore=restore;receiver.clipboard_sample=sample;receiver.clipboard_apply=apply;receiver.clipboard_revoke=[](void*){};
    const uint64_t screen=7;
    auto* h=hop_v2_create(1,1,42,nullptr,0,25,host,&a);
    auto* r=hop_v2_create(2,4,0,&screen,1,25,receiver,&b);
    check(h && r); check(hop_v2_set_key_mode(h,1)==0 && hop_v2_set_key_mode(h,3)==-1); check(hop_v2_enable_clipboard(h,1,0)==0 && hop_v2_enable_clipboard(r,1,0)==0);
    check(hop_v2_start(r,1,0)==0 && hop_v2_start(h,1,0)==0); check(pump(h,r) && pump(r,h));
    auto list=screens(); check(hop_v2_advertise(r,list.data(),list.size())==0 && pump(r,h));
    check(hop_v2_peer_screens(h,nullptr,0)==1);hop_v2_screen visible{};
    check(hop_v2_peer_screens(h,&visible,1)==1 && visible.id==7 && visible.width==1920 && visible.height==1080);
    check(hop_v2_enter(h,43,7,0,0)==0 && pump(h,r) && pump(r,h)); check(b.enters==1 && hop_v2_focus_epoch(h)==43);
    const uint8_t meta[]={1,4,0,0,0,0xe3,0,0x40,1,0,0,0}, key[]={1,4,0,0,0,6,0,0x40,1,0,0,0};
    check(hop_v2_send_input(h,meta,sizeof(meta))==0 && hop_v2_send_input(h,key,sizeof(key))==0);
    check(pump(h,r) && pump(h,r) && b.inputs==2 && b.last[5]==6 && b.last[7]==4);
    check(hop_v2_clipboard_start(h,1,1,0)==-1);
    check(hop_v2_clipboard_ready(h)==0 && hop_v2_clipboard_ready(r)==0 && pump(h,r) && pump(r,h));
    check(hop_v2_clipboard_available(h)==1 && hop_v2_clipboard_available(r)==1);
    check(hop_v2_clipboard_start(h,1,1,0)==0 && pump(h,r)); check(b.clips==1 && b.baseline==0);
    check(hop_v2_clipboard_initial_done(h)==0 && pump(h,r) && hop_v2_clipboard_initial_received(r)==1 && hop_v2_clipboard_initial_drained(r)==0);
    check(hop_v2_clipboard_start(r,2,1,0)==0); size_t pending=0;
    check(hop_v2_pull(r,1,nullptr,0,&pending)==1 && pending>0);
    check(hop_v2_release(h,0)==0 && pump(h,r));
    pending=99; check(hop_v2_pull(r,1,nullptr,0,&pending)==0 && pending==0);
    check(hop_v2_complete_release(r,0)==0 && hop_v2_complete_release(h,0)==0 && pump(r,h)); check(a.restores==1 && b.restores==1);
    hop_v2_free(h);hop_v2_free(r);
    auto without_revoke=host; without_revoke.clipboard_revoke=nullptr;
    auto* unsafe_clip=hop_v2_create(1,1,42,nullptr,0,25,without_revoke,&a);
    check(unsafe_clip && hop_v2_enable_clipboard(unsafe_clip,1,0)==-1); hop_v2_free(unsafe_clip);
    State empty; auto* invalid=hop_v2_create(1,1,42,nullptr,0,25,host,&empty);check(invalid);check(hop_v2_start(invalid,0,0)==-1 && empty.restores==1);hop_v2_free(invalid);
    check(hop_v2_create(2,4,0,&screen,1,25,host,&a)==nullptr);
    std::cout<<"PASS "<<checks<<" C ABI session lifecycle checks with fake input and clipboard\n";
}
