#include "hop_windows/clipboard_worker.h"
#include <chrono>
#include <new>
namespace hop::windows {
namespace {std::atomic<bool> occupied{false};}
std::unique_ptr<ClipboardWorker> ClipboardWorker::start(std::shared_ptr<ClipboardBackend> backend) {
    if(!backend)return nullptr;bool expected=false;if(!occupied.compare_exchange_strong(expected,true))return nullptr;
    try {
        auto state=std::make_shared<State>(std::move(backend));auto worker=std::unique_ptr<ClipboardWorker>(new(std::nothrow) ClipboardWorker(state));
        if(!worker){occupied.store(false);return nullptr;}
        worker->thread_=std::thread([state]{
            try {while(!state->stop.load()) {if(!state->actor.work_once()){state->failed.store(true);break;}std::this_thread::sleep_for(std::chrono::milliseconds(5));}}
            catch(...) {state->failed.store(true);}
            state->done.store(true);occupied.store(false);
        });return worker;
    }catch(...){occupied.store(false);return nullptr;}
}
ClipboardWorker::~ClipboardWorker() {
    if(!state_)return;
    if(!stop_ticket_)stop_ticket_=state_->actor.revoke(0);
    state_->stop.store(true);
    if(thread_.joinable()) {if(state_->done.load())thread_.join();else thread_.detach();}
}
bool ClipboardWorker::request_stop(std::uint64_t now) {
    if(stop_ticket_)return true;stop_ticket_=state_->actor.revoke(now);return stop_ticket_!=0;
}
bool ClipboardWorker::poll_stop(std::uint64_t now) {
    if(!stop_ticket_)return false;
    const bool expired=state_->actor.timed_out(stop_ticket_,now);
    if(state_->actor.acknowledged(stop_ticket_) || expired || state_->failed.load())state_->stop.store(true);
    if(state_->done.load()) {if(thread_.joinable())thread_.join();return true;}
    if(expired){if(thread_.joinable())thread_.detach();return true;}
    return false;
}
}
