#pragma once
#include "hop_windows/connection.h"
#include "hop_windows/pairing_review.h"
#include "hop_windows/tcp_endpoint.h"
namespace hop::windows {
// One explicitly initiated TCP path, including the HPSL preface and then exact
// TLS pinning. Session, local identity, and trust store must outlive the path; all methods run on one non-hook worker.
class ConnectionPath {
public:
    static std::unique_ptr<ConnectionPath> connect_trusted(const TcpAddress&,const std::string& peer_fingerprint,
        DeviceIdentity&,TrustStore&,Session&,bool explicitly_requested,std::uint64_t now_ms);
    static std::unique_ptr<ConnectionPath> connect_candidate(const TcpAddress&,PairingReview&,
        DeviceIdentity&,Session&,bool explicitly_requested,std::uint64_t now_ms);
    static std::unique_ptr<ConnectionPath> accept(SOCKET socket,DeviceIdentity&,TrustStore&,PairingReview*,Session&,
        bool explicitly_enabled,std::uint64_t now_ms);
    ~ConnectionPath();
    bool step(std::uint64_t now_ms);
    bool authenticated() const {return connection_ && connection_->authenticated();}
    const TlsSocket* transport() const {return connection_ ? &connection_->transport() : nullptr;}
private:
    ConnectionPath(DeviceIdentity& identity,Session& session) : identity_(identity),session_(session) {}
    bool fail();
    bool install(SOCKET socket,bool tcp_client,std::uint64_t now_ms);
    DeviceIdentity& identity_; Session& session_; TrustStore* store_=nullptr; std::optional<PairingReview> review_;
    std::unique_ptr<TcpDialer> dialer_; std::unique_ptr<PeerSelector> selector_; std::unique_ptr<Connection> connection_;
    VerifiedPeerCertificate peer_; std::uint64_t expires_=0,last_now_=0; bool first_pair_=false,incoming_=false,failed_=false;
};
}
