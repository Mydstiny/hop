#include "hop_windows/host_input_bridge.h"
#include "hop_windows/input_bridge.h"
#include "hop_windows/receiver_session_adapter.h"
#include <iostream>
#include <vector>
using namespace hop;
using namespace hop::windows;
int main() {
    int n=0; auto check=[&](bool b){++n;if(!b){std::cerr<<"FAIL "<<n<<'\n';std::exit(1);}};
    std::vector<INPUT> events; InputSink sink([&](UINT count,LPINPUT p,int){if(count!=1)return UINT(0);events.push_back(*p);return UINT(1);});
    ReceiverSessionAdapter receiver_bridge(sink,{{7,{0,0,1920,1080}}},{0,0,1920,1080}); check(receiver_bridge.valid());
    ReceiverSessionAdapter multiple(sink,{{7,{0,0,1920,1080}},{8,{-1920,0,1920,1080}}},{-1920,0,3840,1080}); check(multiple.valid() && multiple.screen_ids().size()==2 && multiple.screen_payload().size()==56);
    ReceiverSessionAdapter duplicate(sink,{{7,{0,0,1920,1080}},{7,{-1920,0,1920,1080}}},{-1920,0,3840,1080}); check(!duplicate.valid());
    int restored=0; Session host(SessionRole::host,42,{},{{},{},[&]{++restored;return true;}});
    Session receiver(SessionRole::receiver,0,receiver_bridge.screen_ids(),receiver_bridge.callbacks());
    auto deliver=[&](Session& from,Session& to){std::vector<std::uint8_t> bytes;check(from.take(true,bytes) && !bytes.empty());check(to.receive(true,bytes.data(),bytes.size(),0));};
    check(host.start(true,0) && receiver.start(true,0)); deliver(host,receiver); deliver(receiver,host);
    check(receiver.advertise_screens(receiver_bridge.screen_payload())); deliver(receiver,host);
    check(host.enter(43,7,0,0)); deliver(host,receiver); deliver(receiver,host);
    PointerSettings settings; settings.movement_scale=2; settings.reverse_vertical=true;
    HostInputBridge bridge(host,7,1920,1080,0,0,true,settings); check(bridge.valid());
    check(bridge.move_relative(100,0)); deliver(host,receiver); check(events.back().mi.dx>6800 && events.back().mi.dx<6900);
    CaptureEvent ctrl{CaptureKind::key,0x1d,true}; check(bridge.captured(ctrl)); deliver(host,receiver); check(events.back().ki.wScan==0x1d);
    CaptureEvent c{CaptureKind::key,0x2e,true}; check(bridge.captured(c)); deliver(host,receiver); check(events.back().ki.wScan==0x2e);
    c.down=false; check(bridge.captured(c)); deliver(host,receiver); ctrl.down=false; check(bridge.captured(ctrl)); deliver(host,receiver); check(sink.held_keys()==0);
    CaptureEvent button{CaptureKind::button,3,true}; check(bridge.captured(button)); deliver(host,receiver); check(sink.held_buttons()==1);
    button.down=false; check(bridge.captured(button)); deliver(host,receiver); check(sink.held_buttons()==0);
    CaptureEvent wheel{CaptureKind::wheel}; wheel.y=120; check(bridge.captured(wheel)); deliver(host,receiver); check(events.back().mi.mouseData==120);
    check(!bridge.set_pointer_settings({0,1,1,false,false}));
    CaptureEvent unknown{CaptureKind::key,0xff,true}; check(!bridge.captured(unknown)); check(host.phase()==SessionPhase::failed && restored==1);
    receiver.disconnect(); check(sink.held_keys()==0 && sink.held_buttons()==0);
    std::cout<<"PASS "<<n<<" host to receiver bridge checks; fake capture and injection only\n";
}
