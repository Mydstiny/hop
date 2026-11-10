#include "hop_windows/clipboard_actor.h"
#include <condition_variable>
#include <iostream>
#include <thread>
using namespace hop::windows;
namespace {
struct Fake : ClipboardBackend {
    std::mutex mutex;std::condition_variable cv;bool entered=false,unblock=false;int writes=0;std::uint32_t current=10;
    bool read(std::uint32_t,ClipboardPayload&) override{return false;}
    bool write(const ClipboardPayload&,std::uint32_t expected,std::uint32_t& result) override {
        std::unique_lock<std::mutex> lock(mutex);entered=true;cv.notify_all();cv.wait(lock,[&]{return unblock;});
        ++writes;if(current!=expected)return false;result=++current;return true;
    }
};
struct ReadWrite : ClipboardBackend {
    int reads=0,writes=0;std::uint32_t current=20;ClipboardPayload value{ClipboardFormat::utf8_text,{'t','e','x','t'}};
    bool read(std::uint32_t expected,ClipboardPayload& out) override {++reads;if(expected!=current)return false;out=value;return true;}
    bool write(const ClipboardPayload& in,std::uint32_t expected,std::uint32_t& result) override {++writes;if(expected!=current)return false;value=in;result=++current;return true;}
    ClipboardWriteResult write_guarded(const ClipboardPayload& in,std::uint32_t expected,std::uint32_t& result) override {
        if(expected!=current)return ClipboardWriteResult::conflict;
        return write(in,expected,result) ? ClipboardWriteResult::written : ClipboardWriteResult::error;
    }
};
}
int main() {
    int checks=0;auto check=[&](bool ok){++checks;if(!ok){std::cerr<<"FAIL "<<checks<<'\n';std::exit(1);}};
    Fake backend;ClipboardActor actor(backend); ClipboardPayload text{ClipboardFormat::utf8_text,{'h','i'}};
    auto first=actor.activate("peer-a",7,10,0);check(first && !actor.acknowledged(first));check(actor.work_once() && actor.acknowledged(first));
    check(!actor.activate("peer-b",8,10,0));check(actor.submit_remote("peer-a",7,10,text));
    bool worker_ok=false;std::thread worker([&]{worker_ok=actor.work_once();});
    {std::unique_lock<std::mutex> lock(backend.mutex);backend.cv.wait(lock,[&]{return backend.entered;});}
    const auto revoke=actor.revoke(1);check(revoke && !actor.acknowledged(revoke));
    check(!actor.submit_remote("peer-a",7,10,text));check(!actor.activate("peer-b",8,10,1));
    check(!actor.timed_out(revoke,2000) && actor.timed_out(revoke,2001));
    {std::lock_guard<std::mutex> lock(backend.mutex);backend.unblock=true;}backend.cv.notify_all();worker.join();
    check(worker_ok && backend.writes==1 && !actor.acknowledged(revoke));
    check(actor.work_once() && actor.acknowledged(revoke));
    auto second=actor.activate("peer-b",8,11,10);check(second && actor.work_once() && actor.acknowledged(second));
    check(!actor.submit_remote("peer-a",7,11,text));
    check(actor.submit_remote("peer-b",8,11,text));auto cancel=actor.revoke(11);check(cancel && actor.work_once() && actor.acknowledged(cancel));check(backend.writes==1);
    ReadWrite rw;ClipboardActor local(rw);auto t=local.activate("peer",9,20,0);check(t && local.work_once());
    check(!local.observe_local(8,20));rw.current=21;check(local.observe_local(9,21) && local.work_once());
    check(!local.take_local(8));auto copied=local.take_local(9);check(copied && copied->bytes==rw.value.bytes && rw.reads==1);
    check(local.submit_remote("peer",9,21,{ClipboardFormat::utf8_text,{'r'}}) && local.work_once() && rw.writes==1);
    check(local.observe_local(9,22) && local.work_once() && !local.take_local(9)); // suppress local write echo
    rw.current=23;check(local.observe_local(9,23) && local.work_once());auto revoked=local.revoke(1);check(revoked && local.work_once() && !local.take_local(9));
    ReadWrite initial;ClipboardActor snapshots(initial);auto config=snapshots.activate("peer",10,20,0);check(config && snapshots.work_once());
    check(!snapshots.request_initial_snapshot(9,20));check(snapshots.request_initial_snapshot(10,20) && snapshots.work_once());
    auto first_snapshot=snapshots.take_local(10);check(first_snapshot && first_snapshot->bytes==initial.value.bytes && initial.reads==1);
    check(snapshots.request_initial_snapshot(10,20) && snapshots.work_once() && !snapshots.take_local(10) && initial.reads==1);
    auto reset=snapshots.revoke(1);check(reset && snapshots.work_once());config=snapshots.activate("peer",11,20,2);check(config && snapshots.work_once());
    check(snapshots.request_initial_snapshot(11,20));reset=snapshots.revoke(3);check(reset && snapshots.work_once() && !snapshots.take_local(11) && initial.reads==1);
    ReadWrite conflict;ClipboardActor destination(conflict);config=destination.activate("peer",12,20,0);check(config && destination.work_once());
    conflict.current=21;conflict.value={ClipboardFormat::utf8_text,{'n','e','w'}};
    check(destination.submit_remote("peer",12,20,{ClipboardFormat::utf8_text,{'o','l','d'}}));
    check(!destination.quiescent(12) && destination.work_once() && destination.quiescent(12));
    check(conflict.writes==0 && destination.observe_local(12,21) && destination.work_once());
    auto preserved=destination.take_local(12);check(preserved && preserved->bytes==conflict.value.bytes);
    std::cout<<"PASS "<<checks<<" clipboard actor ordering checks; fake backend only\n";
}
