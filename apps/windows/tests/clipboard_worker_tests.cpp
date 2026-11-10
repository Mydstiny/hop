#include "hop_windows/clipboard_worker.h"
#include <condition_variable>
#include <iostream>
#include <thread>
using namespace hop::windows;
namespace {
struct Blocking : ClipboardBackend {
    std::mutex mutex;std::condition_variable cv;bool entered=false,unblock=false;
    bool read(std::uint32_t,ClipboardPayload&) override{return false;}
    bool write(const ClipboardPayload&,std::uint32_t,std::uint32_t& out) override {std::unique_lock<std::mutex> lock(mutex);entered=true;cv.notify_all();cv.wait(lock,[&]{return unblock;});out=11;return true;}
};
}
int main(){
    int n=0;auto check=[&](bool b){++n;if(!b){std::cerr<<"FAIL "<<n<<'\n';std::exit(1);}};
    auto backend=std::make_shared<Blocking>();auto worker=ClipboardWorker::start(backend);check(worker!=nullptr);
    auto active=worker->actor().activate("peer",7,10,0);check(active!=0);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(std::chrono::steady_clock::now()<deadline && !worker->actor().acknowledged(active))std::this_thread::sleep_for(std::chrono::milliseconds(1));
    check(worker->actor().acknowledged(active));check(worker->actor().submit_remote("peer",7,10,{ClipboardFormat::utf8_text,{'x'}}));
    {std::unique_lock<std::mutex> lock(backend->mutex);backend->cv.wait(lock,[&]{return backend->entered;});}
    check(worker->request_stop(1));check(!worker->poll_stop(2000));check(worker->poll_stop(2001));
    auto blocked=ClipboardWorker::start(backend);check(!blocked);worker.reset();
    {std::lock_guard<std::mutex> lock(backend->mutex);backend->unblock=true;}backend->cv.notify_all();
    std::unique_ptr<ClipboardWorker> next;while(std::chrono::steady_clock::now()<deadline && !next){next=ClipboardWorker::start(backend);if(!next)std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    check(next!=nullptr);check(next->request_stop(3));
    while(std::chrono::steady_clock::now()<deadline && !next->poll_stop(4))std::this_thread::sleep_for(std::chrono::milliseconds(1));
    check(next->poll_stop(4));std::cout<<"PASS "<<n<<" clipboard worker timeout checks; fake backend only\n";
}
