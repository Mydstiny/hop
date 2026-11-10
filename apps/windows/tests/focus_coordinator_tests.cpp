#include "hop_windows/focus_coordinator.h"
#include <condition_variable>
#include <iostream>
#include <thread>
using namespace hop;using namespace hop::windows;
namespace {
int checks=0;void check(bool b){++checks;if(!b){std::cerr<<"FAIL "<<checks<<'\n';std::exit(1);}}
struct Fake : ClipboardBackend {
    std::mutex mutex;std::condition_variable cv;bool entered=false,unblock=false;
    bool read(std::uint32_t,ClipboardPayload&) override{return false;}
    bool write(const ClipboardPayload&,std::uint32_t,std::uint32_t& result) override {std::unique_lock<std::mutex> lock(mutex);entered=true;cv.notify_all();cv.wait(lock,[&]{return unblock;});result=11;return true;}
};
struct Pair {
    int restored=0,receiver_restored=0;Session host,receiver;
    explicit Pair(std::uint64_t epoch) : host(SessionRole::host,epoch,{},{{},{},[this]{++restored;return true;}}),
      receiver(SessionRole::receiver,0,{7},{[](const Frame&){return true;},[](const Frame&){return true;},[this]{++receiver_restored;return true;}}) {}
    void send(Session& a,Session& b){std::vector<std::uint8_t> bytes;check(a.take(true,bytes) && !bytes.empty());check(b.receive(true,bytes.data(),bytes.size(),0));}
    void ready(){check(host.start(true,0) && receiver.start(true,0));send(host,receiver);send(receiver,host);check(receiver.advertise_screens({1,3,0,0,0,1,0,0,0,0,0,0,0,0,0,7,0,0,0,0,0,0,0,0,0,0,7,128,0,0,4,56}));send(receiver,host);}
    void active(std::uint64_t epoch){ready();check(host.enter(epoch,7,0,0));send(host,receiver);send(receiver,host);}
};
}
int main() {
    {
        Fake blocked;ClipboardActor target(blocked);Pair pair(400);std::uint64_t ticket=0;
        check(pair.receiver.set_release_barrier([&](std::uint64_t now){ticket=target.revoke(now);return ticket!=0;},[&]{return target.acknowledged(ticket);}));
        pair.active(401);auto activation=target.activate("peer",401,10,0);check(activation && target.work_once());
        check(target.submit_remote("peer",401,10,{hop::windows::ClipboardFormat::utf8_text,{'x'}}));
        std::thread pending([&]{check(target.work_once());});
        {std::unique_lock<std::mutex> lock(blocked.mutex);blocked.cv.wait(lock,[&]{return blocked.entered;});}
        check(pair.host.release(0));pair.send(pair.host,pair.receiver);
        check(pair.receiver_restored==1 && pair.receiver.phase()==SessionPhase::release_ack_wait);
        check(pair.receiver.tick(true,100));std::vector<std::uint8_t> output;
        check(pair.receiver.take(true,output) && output.empty() && pair.host.phase()==SessionPhase::release_wait);
        {std::lock_guard<std::mutex> lock(blocked.mutex);blocked.unblock=true;}blocked.cv.notify_all();pending.join();
        check(target.work_once() && target.acknowledged(ticket));check(pair.receiver.tick(true,101));pair.send(pair.receiver,pair.host);
        check(pair.host.phase()==SessionPhase::ready && pair.receiver.phase()==SessionPhase::ready);
    }
    Fake backend;ClipboardActor actor(backend);auto active=actor.activate("peer-a",43,10,0);check(active && actor.work_once());
    check(actor.submit_remote("peer-a",43,10,{hop::windows::ClipboardFormat::utf8_text,{'h','i'}}));
    std::thread writer([&]{check(actor.work_once());});{std::unique_lock<std::mutex> lock(backend.mutex);backend.cv.wait(lock,[&]{return backend.entered;});}
    Pair old(42),next(100);old.active(43);next.ready();bool drained=false;FocusCoordinator switcher(actor,[&]{return drained;});
    check(switcher.begin(old.host,1) && old.restored==1);old.send(old.host,old.receiver);old.send(old.receiver,old.host);
    check(switcher.step(2)==FocusSwitchState::waiting && !switcher.enter_next(next.host,101,7,0,0));
    {std::lock_guard<std::mutex> lock(backend.mutex);backend.unblock=true;}backend.cv.notify_all();writer.join();
    check(actor.work_once());check(switcher.step(3)==FocusSwitchState::waiting);drained=true;check(switcher.step(4)==FocusSwitchState::ready);
    check(switcher.prepare_next(next.host,"peer-b",101,11,true,4));check(!switcher.enter_next(next.host,101,7,0,0));
    check(actor.work_once() && switcher.step(5)==FocusSwitchState::prepared);check(!switcher.enter_next(old.host,101,7,0,0));check(switcher.enter_next(next.host,101,7,0,0));
    next.send(next.host,next.receiver);next.send(next.receiver,next.host);check(next.host.phase()==SessionPhase::active);
    FocusCoordinator timeout(actor,[]{return true;});check(timeout.begin(next.host,11));check(timeout.step(2011)==FocusSwitchState::failed && next.host.phase()==SessionPhase::failed);
    check(!timeout.enter_next(old.host,44,7,0,0));check(actor.work_once());
    Pair old2(200),next2(300);old2.active(201);next2.ready();
    auto before=actor.activate("peer-c",201,12,3000);check(before && actor.work_once());
    FocusCoordinator prepare_timeout(actor,[]{return true;});check(prepare_timeout.begin(old2.host,3001));old2.send(old2.host,old2.receiver);old2.send(old2.receiver,old2.host);
    check(actor.work_once() && prepare_timeout.step(3002)==FocusSwitchState::ready);
    check(prepare_timeout.prepare_next(next2.host,"peer-d",301,12,true,3002));
    check(prepare_timeout.step(5002)==FocusSwitchState::failed && old2.host.phase()==SessionPhase::failed && next2.host.phase()==SessionPhase::failed);
    check(actor.work_once() && !actor.submit_remote("peer-d",301,12,{hop::windows::ClipboardFormat::utf8_text,{'x'}}));
    std::cout<<"PASS "<<checks<<" focus and clipboard barrier checks; fake backend only\n";
}
