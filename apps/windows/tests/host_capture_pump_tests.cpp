#include "hop_windows/host_capture_pump.h"
#include <deque>
#include <iostream>
using namespace hop;
using namespace hop::windows;
namespace {
struct Fake : CaptureSource {
    bool running=false,is_active=false,is_drained=true,overflowed=false,panic=false,cursor_restored=false,fail_start=false;
    std::deque<CaptureEvent> events;
    bool inputs_clear() override {return true;}
    bool prepare(CursorPoint& original,CursorPoint& anchor,bool& caps) override { original={10,20};anchor={500,500};caps=false;return true; }
    bool start() override {if(fail_start)return false;running=true;return true;}
    bool activate() override {is_active=true;return true;}
    bool deactivate() override {is_active=false;return true;}
    bool active() const override {return is_active;}
    bool drained() const override {return is_drained;}
    bool overflow() override {return overflowed;}
    bool emergency() override {return panic;}
    bool poll(CaptureEvent& event) override {if(events.empty())return false;event=events.front();events.pop_front();return true;}
    bool stop() override {running=false;return true;}
    bool force_recovery() override {is_active=false;is_drained=true;return true;}
    bool restore_cursor(CursorPoint p) override {cursor_restored=p.x==10 && p.y==20;return cursor_restored;}
};
}
int main() {
    int checks=0; auto check=[&](bool b){++checks;if(!b){std::cerr<<"FAIL "<<checks<<'\n';std::exit(1);}};
    Fake source; HostCapturePump* pump=nullptr;
    Session host(SessionRole::host,42,{},{{},{},[&]{return !pump || pump->deactivate();}});
    Session receiver(SessionRole::receiver,0,{7},{[](const Frame&){return true;},[](const Frame&){return true;},[]{return true;}});
    auto deliver=[&](Session& from,Session& to){std::vector<std::uint8_t> b;check(from.take(true,b) && !b.empty());check(to.receive(true,b.data(),b.size(),0));};
    check(host.start(true,0) && receiver.start(true,0));deliver(host,receiver);deliver(receiver,host);
    check(receiver.advertise_screens({1,3,0,0,0,1,0,0,0,0,0,0,0,0,0,7,0,0,0,0,0,0,0,0,0,0,7,128,0,0,4,56}));deliver(receiver,host);
    check(host.enter(43,7,0,0));deliver(host,receiver);deliver(receiver,host);
    HostCapturePump worker(host,source);pump=&worker;
    check(worker.begin(7,1920,1080,0,0,{},0) && source.running && source.active());
    source.events.push_back({CaptureKind::move,0,false,false,510,505});
    source.events.push_back({CaptureKind::key,0x1e,true});
    source.events.push_back({CaptureKind::key,0x1e,false});
    check(worker.step(1));deliver(host,receiver);deliver(host,receiver);deliver(host,receiver);
    source.events.push_back({CaptureKind::move,0,false,false,510,500});source.events.push_back({CaptureKind::move,0,false,false,510,500});
    check(worker.step(2));std::vector<std::uint8_t> empty;check(host.take(true,empty) && empty.empty());
    source.events.push_back({CaptureKind::key,0x30,true});check(worker.step(3));deliver(host,receiver);deliver(host,receiver);
    source.events.push_back({CaptureKind::key,0x30,false});check(worker.step(4));deliver(host,receiver);
    source.overflowed=true;check(!worker.step(5));check(source.cursor_restored && !source.active());
    check(host.phase()==SessionPhase::release_wait);deliver(host,receiver);deliver(receiver,host);check(host.phase()==SessionPhase::ready);
    check(worker.drain() && !source.running);
    source.overflowed=false;source.cursor_restored=false;source.fail_start=true;
    check(host.enter(44,7,0,0));deliver(host,receiver);deliver(receiver,host);
    check(!worker.begin(7,1920,1080,0,0,{},6)); check(source.cursor_restored && host.phase()==SessionPhase::release_wait);
    deliver(host,receiver);deliver(receiver,host);check(host.phase()==SessionPhase::ready);
    std::cout<<"PASS "<<checks<<" host capture worker checks; fake source only\n";
}
