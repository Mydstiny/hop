#include "hop_windows/input_bridge.h"
#include <iostream>
#include <vector>
using namespace hop;
using namespace hop::windows;
int main() {
    int checks=0; auto check=[&](bool ok){++checks;if(!ok){std::cerr<<"FAIL "<<checks<<'\n';std::exit(1);}};
    std::vector<INPUT> events; bool reject=false;
    InputSink sink([&](UINT n,LPINPUT p,int size){ if(reject || n!=1 || size!=sizeof(INPUT)) return UINT(0); events.push_back(*p); return UINT(1); });
    ReceiverInputBridge invalid(sink,7,{-1921,0,1920,1080},{-1920,0,3840,1080}); check(!invalid.valid());
    ReceiverInputBridge bridge(sink,7,{-1920,0,1920,1080},{-1920,0,3840,1080}); check(bridge.valid());
    Frame enter{FrameKind::control,1,43,{1,0x10,0,0,0,0,0,0,0,0,0,7,0,0,0,0,0,0,0,0}};
    check(bridge.enter(enter)); check(events.size()==1 && events.back().mi.dx==0 && events.back().mi.dy==0);
    Frame move{FrameKind::input,2,43,{1,1,0,0,0,0,0,0,0,0,0,7,0xff,0xff,0xff,0xff}};
    check(bridge.input(move)); check(events.back().mi.dx>32000 && events.back().mi.dx<33000 && events.back().mi.dy==65535);
    Frame key{FrameKind::input,3,43,{1,4,0,0,0,4,0,0,1,0,0,0}};
    check(bridge.input(key)); check(sink.held_keys()==1 && events.back().ki.wScan==0x1e);
    key.payload[8]=0; check(bridge.input(key)); check(sink.held_keys()==0 && (events.back().ki.dwFlags&KEYEVENTF_KEYUP));
    Frame button{FrameKind::input,4,43,{1,2,0,0,0,0,0,0,0,0,0,7,0,0,0,0,4,1,8,0}};
    check(bridge.input(button)); check(sink.held_buttons()==1 && (events.back().mi.dwFlags&MOUSEEVENTF_XDOWN));
    button.payload[17]=0; button.payload[18]=0; check(bridge.input(button)); check(sink.held_buttons()==0 && (events.back().mi.dwFlags&MOUSEEVENTF_XUP));
    Frame wheel{FrameKind::input,5,43,{1,3,0,0,0,0,0,120,0xff,0xff,0xff,0x88}};
    check(bridge.input(wheel)); check((events[events.size()-2].mi.dwFlags&MOUSEEVENTF_HWHEEL) && (events.back().mi.dwFlags&MOUSEEVENTF_WHEEL));
    key.payload[8]=1; check(bridge.input(key)); check(bridge.restore() && sink.held_keys()==0);
    Frame wrong=move; wrong.payload[11]=8; check(!bridge.input(wrong));
    Frame pause{FrameKind::input,6,43,{1,4,0,0,0,0x48,0,0,1,0,0,0}}; check(!bridge.input(pause) && sink.last_error()==SinkError::unsupported_sequence);
    reject=true; check(!bridge.input(move)); check(sink.last_error()==SinkError::system_rejected);
    std::cout<<"PASS "<<checks<<" receiver input bridge checks; fake injection only\n";
}
