#pragma once
#include "hop_windows/clipboard_actor.h"
#include <atomic>
#include <memory>
#include <thread>
namespace hop::windows {
// Dedicated clipboard actor worker. If the OS blocks beyond the revoke
// deadline, the caller can detach safely: the backend/actor remain owned by
// the worker, and another clipboard worker is rejected until it finally exits.
class ClipboardWorker {
public:
    static std::unique_ptr<ClipboardWorker> start(std::shared_ptr<ClipboardBackend> backend);
    ~ClipboardWorker();
    ClipboardActor& actor() {return state_->actor;}
    bool healthy() const {return !state_->failed.load();}
    bool request_stop(std::uint64_t now_ms);
    bool poll_stop(std::uint64_t now_ms); // nonblocking; true once safely stopped or quarantined
private:
    struct State {
        explicit State(std::shared_ptr<ClipboardBackend> b):backend(std::move(b)),actor(*backend){}
        std::shared_ptr<ClipboardBackend> backend;ClipboardActor actor;std::atomic<bool> stop{false},done{false},failed{false};
    };
    explicit ClipboardWorker(std::shared_ptr<State> state):state_(std::move(state)){}
    std::shared_ptr<State> state_;std::thread thread_;std::uint64_t stop_ticket_=0;
};
}
