#pragma once
#include "hop_windows/connection_path.h"
#include "hop_windows/lan_server.h"
#include <functional>
#include <memory>
#include <optional>
namespace hop::windows {
enum class PairingState { idle, waiting_candidate, reviewing, connecting, confirming, connected, failed };
// One explicitly initiated pairing, driven serially by an owner worker. The UI
// receives copies of PairingReview and must independently compare fingerprints;
// session and UI actions must be marshalled to this worker. No OS input,
// clipboard, listener, or credential store is touched before begin_* is called.
class PairingWorker {
public:
    PairingWorker(DeviceIdentity& identity,TrustStore& store,SessionRole role,std::string device_name,
                  std::vector<std::uint64_t> screens,std::function<SessionCallbacks()> callbacks,
                  std::function<bool(Session&)> configure_session={});
    ~PairingWorker();
    bool begin_listening(const TcpAddress& address,std::uint64_t now_ms);
    // The serial runtime can retain a bound listener while earlier TLS
    // sessions keep using their already accepted sockets.
    bool begin_listening(std::shared_ptr<LanServer> listener,std::uint64_t now_ms);
    bool begin_connecting(const TcpAddress& peer,std::uint64_t now_ms);
    bool local_address(TcpAddress& out) const;
    bool step(std::uint64_t now_ms);
    PairingState state() const {return state_;}
    std::optional<PairingReview> review() const {return review_;}
    // Must be called after a separate UI confirmation over authenticated TLS.
    bool confirm_authenticated_pair(bool user_confirmed);
    Session* session() const {return session_.get();}
    std::vector<RemoteScreen> peer_displays() const;
    bool request_focus(std::uint64_t screen,std::uint16_t x,std::uint16_t y);
    const TlsSocket* transport() const {return path_ ? path_->transport() : nullptr;}
    void cancel();
private:
    bool fail();bool make_session();bool start_candidate(SOCKET socket,std::uint64_t now);
    DeviceIdentity& identity_;TrustStore& store_;SessionRole role_;std::string name_;
    std::vector<std::uint64_t> screens_;std::function<SessionCallbacks()> callbacks_;std::function<bool(Session&)> configure_session_;
    std::shared_ptr<LanServer> server_;std::unique_ptr<TcpDialer> dialer_;std::unique_ptr<CandidateExchange> exchange_;
    std::optional<PairingReview> review_;std::unique_ptr<Session> session_;std::unique_ptr<ConnectionPath> path_;
    TcpAddress remote_{};std::uint64_t started_=0,last_now_=0,retry_after_=0,next_focus_epoch_=0;bool outgoing_=false,local_confirmed_=false;
    PairingState state_=PairingState::idle;
};
}
