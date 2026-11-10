#pragma once
#include "v2_clipboard_mailbox.h"
#include "v2_display.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <chrono>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
namespace hop {
class V2Injector;
struct V2ReceiverConfig {
    V2ReceiverConfig() = default;
    ~V2ReceiverConfig();
    V2ReceiverConfig(const V2ReceiverConfig&) = delete;
    V2ReceiverConfig& operator=(const V2ReceiverConfig&) = delete;
    V2ReceiverConfig(V2ReceiverConfig&& other) noexcept;
    V2ReceiverConfig& operator=(V2ReceiverConfig&& other) noexcept;
    std::string host, certificate_pem, private_key_pem, verified_peer_certificate_pem;
    std::uint16_t port=0;
    std::uint32_t request_id=0;
    std::vector<V2Display> displays;
    bool require_pairing=false;
    std::shared_ptr<V2ClipboardMailbox> clipboard;
    std::function<std::uint32_t()> sample_clipboard;
};
struct V2ReceiverEvent { std::string state, message, peer_fingerprint, peer_certificate_pem, peer_name; std::uint32_t request_id=0; };
// Candidate certificates must already have been compared by the user before
// start. Callback is called from worker and must not re-enter this object.
class V2Receiver {
public:
    using Sink=std::function<void(const V2ReceiverEvent&)>;
    explicit V2Receiver(Sink sink);
    ~V2Receiver();
    bool start(V2ReceiverConfig config);
    bool startOffer(const std::string& host,std::uint16_t port,const std::string& name,const std::string& certificate,std::uint32_t request_id=0);
    void confirmPairing(){confirm_=true;}
    void cancelPairing();
    void confirmStorage(){saved_=true;}
    void stop();
    bool isRunning() const { return running_; }
private:
    void run(V2ReceiverConfig config) noexcept;
    void runOffer(std::string host,std::uint16_t port,std::string name,std::string certificate,std::uint32_t request_id) noexcept;
    void clearCandidate();
    Sink sink_;
    std::unique_ptr<V2Injector> injector_;
    std::mutex candidateMutex_;
    std::string candidateCertificate_;
    std::chrono::steady_clock::time_point candidateExpiry_{};
    std::thread thread_;
    std::atomic<bool> stopped_{false},running_{false},confirm_{false},cancel_{false},saved_{false};
};
}
