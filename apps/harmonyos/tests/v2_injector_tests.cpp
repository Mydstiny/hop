#include "v2_injector.h"
#include "hop/session_c.h"
#include <cstdint>
#include <iostream>
#include <vector>
namespace {
int checks=0;
void check(bool b,const char* reason){++checks;if(!b){std::cerr<<"FAIL "<<reason<<'\n';std::exit(1);}}
int restore(void*){return 1;}
std::vector<std::uint8_t> screens(){return {1,3,0,0,0,1,0,0,0,0,0,0,0,0,0,7,0,0,0,0,0,0,0,0,0,0,3,233,0,0,1,245};}
bool pump(hop_v2_session* a,hop_v2_session* b,hop::V2Injector* receiver=nullptr) {
    std::size_t n=0;int r=hop_v2_pull(a,1,nullptr,0,&n);if(r==0)return true;if(r!=1||!n)return false;
    std::vector<std::uint8_t> bytes(n);if(hop_v2_pull(a,1,bytes.data(),n,&n)!=1)return false;
    if(receiver&&!receiver->beginBatch())return false;
    const bool ok=hop_v2_receive(b,1,bytes.data(),n,0)==0;
    if(receiver)receiver->endBatch();return ok;
}
}
int main(){
    fake::Reset();hop::V2Injector injector;check(injector.configure({{6,0,0,1001,501}}),"configure primary display");hop_v2_callbacks host{};host.restore=restore;uint64_t screen=7;
    auto* h=hop_v2_create(1,1,42,nullptr,0,25,host,nullptr);
    auto* r=hop_v2_create(2,4,0,&screen,1,25,hop::V2Injector::callbacks(),&injector);
    check(h&&r,"create simulated mac and Harmony sessions");
    check(hop_v2_start(r,1,0)==0&&hop_v2_start(h,1,0)==0&&pump(h,r,&injector)&&pump(r,h),"authenticated hello");
    auto s=screens();check(hop_v2_advertise(r,s.data(),s.size())==0&&pump(r,h),"screen advertisement");
    check(hop_v2_enter(h,43,7,32768,32768)==0&&pump(h,r,&injector)&&pump(r,h),"focus and injection accepted");
    const uint8_t meta[]={1,4,0,0,0,0xe3,0,0x40,1,0,0,0},key[]={1,4,0,0,0,6,0,0x40,1,0,0,0};
    check(hop_v2_send_input(h,meta,sizeof(meta))==0&&hop_v2_send_input(h,key,sizeof(key))==0,"queue shortcut");
    check(pump(h,r,&injector)&&pump(h,r,&injector),"inject shortcut");
    check(fake::keys.count(hop::MapHidKey(0xe0))==1&&fake::keys.count(hop::MapHidKey(0x06))==1,"Mac Command+C produces Harmony Ctrl+C");
    const uint8_t wheel[]={1,3,0,0,0,0,0,0,0,0,0,120};
    check(hop_v2_send_input(h,wheel,sizeof(wheel))==0&&pump(h,r,&injector),"inject wheel");
    check(hop_v2_release(h,0)==0,"host starts release");
    fake::failNext=true;
    check(!pump(h,r,&injector),"failed release not acknowledged");
    check(fake::Empty(),"failure recovery retries held key releases");
    hop_v2_free(h);hop_v2_free(r);
    fake::Reset();hop::V2Injector edge;auto callbacks=hop::V2Injector::callbacks();
    check(edge.configure({{6,0,0,1001,501}})&&edge.beginBatch(),"edge test authorization");
    const uint8_t enterEdge[]={1,0x10,0,0,0,0,0,0,0,0,0,7,0,0,0x80,0,0,0,0,0};
    const uint8_t parallel[]={1,1,0,0,0,0,0,0,0,0,0,7,0,0,0x90,0};
    const uint8_t inside[]={1,1,0,0,0,0,0,0,0,0,0,7,0x10,0,0x90,0};
    check(callbacks.enter(&edge,enterEdge,sizeof(enterEdge),1)==1&&callbacks.input(&edge,parallel,sizeof(parallel),1)==1,"edge enter and parallel motion");
    uint16_t x=0,y=0;check(!edge.takeEdge(x,y),"parallel edge movement does not request return");
    check(callbacks.input(&edge,inside,sizeof(inside),1)==1&&callbacks.input(&edge,parallel,sizeof(parallel),1)==1,"move inside and return");
    check(edge.takeEdge(x,y)&&x==0&&y==0x9000,"return to edge is requested");edge.endBatch();edge.release();

    const auto point=[](std::uint8_t op,std::uint16_t x,std::uint16_t y) {
        std::vector<std::uint8_t> p={1,op,0,0,0,0,0,0,0,0,0,7,
            std::uint8_t(x>>8),std::uint8_t(x),std::uint8_t(y>>8),std::uint8_t(y)};
        if(op==0x10)p.resize(20);
        return p;
    };
    const auto center=point(1,32000,32000), entry=point(0x10,32000,32000);
    for(auto boundary:{std::pair<std::uint16_t,std::uint16_t>{0,32000},{65535,32000},{32000,0},{32000,65535}}) {
        const auto move=point(1,boundary.first,boundary.second);
        check(edge.beginBatch()&&callbacks.enter(&edge,entry.data(),entry.size(),2)==1,"enter before directional edge");
        check(callbacks.input(&edge,move.data(),move.size(),2)==1&&callbacks.input(&edge,move.data(),move.size(),2)==1&&
              edge.takeEdge(x,y)&&x==boundary.first&&y==boundary.second,"duplicate clamped moves in a batch preserve all four exits");
        check(!edge.takeEdge(x,y)&&callbacks.input(&edge,move.data(),move.size(),2)==1&&edge.takeEdge(x,y),
              "continued movement at the edge can retry after a rejected request");
        check(callbacks.input(&edge,move.data(),move.size(),2)==1&&callbacks.input(&edge,center.data(),center.size(),2)==1&&
              !edge.takeEdge(x,y),"retreat before the throttle expires cancels the pending exit");
        edge.endBatch();check(edge.release(),"reset focus and edge before next direction");
    }
    const auto atSide=point(0x10,0,32000), atCorner=point(1,0,0);
    check(edge.beginBatch()&&callbacks.enter(&edge,atSide.data(),atSide.size(),3)==1&&
          callbacks.input(&edge,atCorner.data(),atCorner.size(),3)==1&&edge.takeEdge(x,y)&&x==1&&y==0,
          "reaching a corner from the side reports only the axis crossed");
    edge.endBatch();check(edge.release(),"release corner test");

    fake::Reset();hop::V2Injector multi;
    const std::vector<hop::V2Display> layout={{0,0,0,1001,501},{15,-800,-600,800,600}};
    const std::vector<hop::V2Display> primary={{0,0,0,1001,501}};
    check(hop::validV2Displays(layout)&&layout[0].screenId()==1&&layout[1].screenId()==16,"native display zero maps to a nonzero wire ID");
    check(!hop::validV2Displays({{0,0,0,100,100},{0,100,0,100,100}})&&
          !hop::validV2Displays({{0,0,0,100,100},{1,99,0,100,100}})&&
          !hop::validV2Displays({{0,0,0,100,100},{1,INT32_MAX,0,1,1}})&&
          !hop::validV2Displays({{0,1,0,100,100}}),"duplicate, overlapping, overflowing and nonprimary-origin layouts rejected");
    check(multi.configure(layout)&&multi.beginBatch(),"verified extended display configuration");
    const uint8_t wrongEnter[]={1,0x10,0,0,0,0,0,0,0,0,0,31,0,0,0,0,0,0,0,0};
    const uint8_t extendedEnter[]={1,0x10,0,0,0,0,0,0,0,0,0,16,0x80,0,0x80,0,0,0,0,0};
    const uint8_t primaryMove[]={1,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0};
    const uint8_t extendedDown[]={1,2,0,0,0,0,0,0,0,0,0,16,0x80,0,0x80,0,1,1,1,0};
    check(callbacks.enter(&multi,wrongEnter,sizeof(wrongEnter),11)==0&&fake::mouse.empty(),"unknown screen cannot inject ENTER");
    check(callbacks.enter(&multi,extendedEnter,sizeof(extendedEnter),11)==1&&fake::mouse.back().display_id==15&&
          fake::mouse.back().x==-400&&fake::mouse.back().y==-300&&fake::mouse.back().display_x==400&&fake::mouse.back().display_y==300,
          "extended focus uses the signed global origin and verified display ID");
    const auto moves=fake::mouse.size();
    check(callbacks.input(&multi,primaryMove,sizeof(primaryMove),11)==0&&
          callbacks.input(&multi,extendedDown,sizeof(extendedDown),12)==0&&fake::mouse.size()==moves,
          "mismatched screen or stale focus epoch cannot inject");
    check(callbacks.input(&multi,extendedDown,sizeof(extendedDown),11)==1&&
          callbacks.input(&multi,key,sizeof(key),11)==1&&fake::key.back().display_id==15,"extended buttons and keys remain on the selected display");
    multi.endBatch();fake::unavailableDisplay=15;
    check(!multi.configure(primary)&&!fake::Empty()&&fake::mouse.back().display_id==15,"unplugged display release debt blocks replacement geometry");
    fake::unavailableDisplay=-2;
    check(multi.configure(primary)&&fake::Empty()&&fake::mouse.back().display_id==15&&fake::key.back().display_id==15,
          "retry releases old display before configuring primary-only fallback");
    const uint8_t primaryEnter[]={1,0x10,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0};
    check(multi.beginBatch()&&callbacks.enter(&multi,extendedEnter,sizeof(extendedEnter),13)==0&&
          callbacks.enter(&multi,primaryEnter,sizeof(primaryEnter),13)==1&&fake::mouse.back().display_id==0&&
          fake::mouse.back().x==1&&fake::mouse.back().y==1,"only primary can be entered after hotplug and zero-coordinate nudge stays inside");
    multi.endBatch();check(multi.release()&&fake::created==fake::destroyed,"hotplug cleanup releases event handles");
    std::cout<<"PASS "<<checks<<" Harmony v2 fake-SDK session/injection checks; no device input\n";
}
