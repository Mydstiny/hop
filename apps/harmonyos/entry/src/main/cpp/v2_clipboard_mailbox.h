#pragma once
#include "hop/clipboard_transfer.h"
#include <mutex>
#include <optional>
#include <utility>

namespace hop {
// Single slot each way. The TLS worker never waits on an ArkTS job. The
// clipboard actor must acknowledge revocation only once all writes have ended.
class V2ClipboardMailbox {
public:
    struct Packet {
        ClipboardFormat format=ClipboardFormat::text;
        std::uint64_t epoch=0;
        std::uint32_t baseline=0;
        std::vector<std::uint8_t> bytes;
        Packet()=default;
        Packet(Packet&&) noexcept=default;
        Packet& operator=(Packet&&)=delete;
        Packet(const Packet&)=delete;
        ~Packet(){volatile std::uint8_t* p=bytes.data();for(std::size_t i=0;i<bytes.size();++i)p[i]=0;}
    };
    struct State { std::uint64_t epoch=0,formats=0; std::uint32_t baseline=0; bool active=false,poll=false,draining=false; };
    explicit V2ClipboardMailbox(std::uint32_t request):request_(request){}
    std::uint32_t request() const {return request_;}
    State state() const {
        std::lock_guard<std::mutex> lock(mutex_);
        State snapshot=state_;
        snapshot.poll=snapshot.poll&&!outgoing_&&!paused_;
        return snapshot;
    }
    void begin(std::uint64_t epoch,std::uint64_t formats,std::uint32_t baseline) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_={epoch,formats,baseline,true,false,false};activated_=false;applying_=false;paused_=false;
    }
    bool activate(std::uint64_t epoch) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!valid(epoch))return false;
        activated_=true;return true;
    }
    bool activated(std::uint64_t epoch) const {
        std::lock_guard<std::mutex> lock(mutex_);return valid(epoch)&&activated_;
    }
    bool permitted(std::uint64_t epoch) const {
        std::lock_guard<std::mutex> lock(mutex_);return valid(epoch)&&activated_;
    }
    bool incoming(const ClipboardValue& value,std::uint64_t baseline) {
        Packet copy;copy.format=value.format;copy.epoch=value.epoch;copy.baseline=static_cast<std::uint32_t>(baseline);copy.bytes=value.data;
        std::lock_guard<std::mutex> lock(mutex_);
        if(!valid(value.epoch)||!activated_||incoming_)return false;
        incoming_.emplace(std::move(copy));return true;
    }
    std::optional<Packet> takeIncoming(std::uint64_t epoch) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!valid(epoch)||applying_||!incoming_)return std::nullopt;
        std::optional<Packet> out(std::move(incoming_));incoming_.reset();applying_=true;return out;
    }
    void finishIncoming(std::uint64_t epoch) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(state_.epoch==epoch)applying_=false;
    }
    bool initialDrained(std::uint64_t epoch) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!valid(epoch)||!activated_||state_.poll||incoming_||applying_)return false;
        state_.poll=true;return true;
    }
    bool submit(Packet packet) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto mask=packet.format==ClipboardFormat::text?2u:4u;
        if(!valid(packet.epoch)||!state_.poll||!(state_.formats&mask)||outgoing_)return false;
        outgoing_.emplace(std::move(packet));return true;
    }
    std::optional<Packet> takeOutgoing() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::optional<Packet> out(std::move(outgoing_));outgoing_.reset();return out;
    }
    void pause(std::uint64_t epoch) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(state_.epoch==epoch && state_.active)paused_=true;
    }
    bool pauseRequested() const {
        std::lock_guard<std::mutex> lock(mutex_);return paused_;
    }
    void revoke() {
        // Move payloads out before wiping, so other threads never wait for a
        // large clipboard allocation or wipe under the control lock.
        std::optional<Packet> incoming,outgoing;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(state_.active){state_.active=false;state_.poll=false;state_.draining=activated_;}
            paused_=false;
            if(incoming_)incoming.emplace(std::move(*incoming_));
            if(outgoing_)outgoing.emplace(std::move(*outgoing_));
            incoming_.reset();outgoing_.reset();
        }
    }
    bool acknowledge(std::uint64_t epoch) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(state_.epoch!=epoch||state_.active||applying_)return false;
        state_.draining=false;return true;
    }
    bool drained() const {
        std::lock_guard<std::mutex> lock(mutex_);return !state_.draining;
    }
private:
    bool valid(std::uint64_t epoch) const {return state_.active && !paused_ && epoch && state_.epoch==epoch;}
    const std::uint32_t request_;
    mutable std::mutex mutex_;
    State state_;
    bool activated_=false,applying_=false,paused_=false;
    std::optional<Packet> incoming_,outgoing_;
};
} // namespace hop
