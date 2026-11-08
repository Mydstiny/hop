#pragma once
#include "protocol.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <deque>

namespace hop {
struct Config {
    std::string host,uuid,peerId,token,name="HarmonyOS PC";
    int port=24890,width=1,height=1;
};
struct Event {
    std::string kind,state,message,peerId,token;
    uint32_t code=0;
    std::string host,session;
    int port=24890;
    Event(std::string kindValue="",std::string stateValue="",std::string messageValue="",std::string peerValue="",std::string tokenValue="",uint32_t codeValue=0)
        :kind(std::move(kindValue)),state(std::move(stateValue)),message(std::move(messageValue)),peerId(std::move(peerValue)),token(std::move(tokenValue)),code(codeValue){}
};
class Client {
    std::thread thread_; std::atomic<bool> stop_{false};std::atomic<int> fd_{-1};
    std::mutex commandsMutex_,fdMutex_;std::deque<std::string> commands_;
    std::function<void(Event)> event_;
    void Run(Config config) noexcept;
public:
    explicit Client(std::function<void(Event)> event):event_(std::move(event)){}
    ~Client(){Stop();}
    void Start(Config config){Stop();stop_=false;{std::lock_guard<std::mutex> l(commandsMutex_);commands_.clear();}thread_=std::thread([this,config]{Run(config);});}
    void Command(const std::string& cmd){std::lock_guard<std::mutex> l(commandsMutex_);commands_.push_back(cmd);}
    void Stop();
};
} // namespace hop
