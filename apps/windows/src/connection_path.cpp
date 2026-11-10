#include "hop_windows/connection_path.h"
#include <windows.h>
#include <new>
namespace hop::windows {
namespace {
bool identity_ok(const DeviceIdentity& id) {
    char fp[65]{};return !id.certificate.empty() && !id.private_key.empty() &&
        hop_tls_validate_identity(id.certificate.data(),id.certificate.size(),id.private_key.data(),id.private_key.size())==0 &&
        hop_tls_certificate_fingerprint(id.certificate.data(),id.certificate.size(),fp,sizeof(fp))==0 && id.fingerprint==fp;
}
void wipe(std::string& value) {if(!value.empty())SecureZeroMemory(&value[0],value.size());value.clear();}
struct CredentialsGuard {
    TlsCredentials value;
    ~CredentialsGuard(){wipe(value.private_key_pem);}
};
}
std::unique_ptr<ConnectionPath> ConnectionPath::connect_trusted(const TcpAddress& address,const std::string& fingerprint,DeviceIdentity& identity,TrustStore& store,Session& session,bool requested,std::uint64_t now) {
    if(!requested || !identity_ok(identity))return nullptr;
    std::unique_ptr<ConnectionPath> path(new(std::nothrow) ConnectionPath(identity,session));if(!path)return nullptr;
    if(!store.load_peer(fingerprint,path->peer_))return nullptr;
    path->dialer_=TcpDialer::start(address,true,now);if(!path->dialer_)return nullptr;path->last_now_=now;return path;
}
std::unique_ptr<ConnectionPath> ConnectionPath::connect_candidate(const TcpAddress& address,PairingReview& review,DeviceIdentity& identity,Session& session,bool requested,std::uint64_t now) {
    if(!requested || !identity_ok(identity))return nullptr;
    std::unique_ptr<ConnectionPath> path(new(std::nothrow) ConnectionPath(identity,session));if(!path)return nullptr;
    if(!review.approved_peer(now,path->peer_,path->expires_))return nullptr;
    path->first_pair_=true;path->review_=review;path->dialer_=TcpDialer::start(address,true,now);if(!path->dialer_)return nullptr;path->last_now_=now;return path;
}
std::unique_ptr<ConnectionPath> ConnectionPath::accept(SOCKET socket,DeviceIdentity& identity,TrustStore& store,PairingReview* review,Session& session,bool enabled,std::uint64_t now) {
    if(!enabled || !identity_ok(identity)){if(socket!=INVALID_SOCKET)closesocket(socket);return nullptr;}
    std::unique_ptr<ConnectionPath> path(new(std::nothrow) ConnectionPath(identity,session));if(!path){if(socket!=INVALID_SOCKET)closesocket(socket);return nullptr;}
    path->incoming_=true;path->store_=&store;if(review)path->review_=*review;path->selector_=PeerSelector::receiver(socket,now);if(!path->selector_)return nullptr;path->last_now_=now;return path;
}
ConnectionPath::~ConnectionPath() {if(!failed_ && !connection_ && session_.phase()!=SessionPhase::initial) session_.disconnect();}
bool ConnectionPath::fail() {if(!failed_){failed_=true;connection_.reset();selector_.reset();dialer_.reset();session_.disconnect();}return false;}
bool ConnectionPath::install(SOCKET socket,bool client,std::uint64_t now) {
    CredentialsGuard creds{{identity_.certificate,identity_.private_key,peer_}};
    connection_=Connection::adopt(socket,client,creds.value,session_,first_pair_,now);
    return connection_!=nullptr;
}
bool ConnectionPath::step(std::uint64_t now) {
    if(failed_ || now<last_now_)return fail();last_now_=now;
    try {
        if(first_pair_ && !session_.pairing_confirmed()) {
            VerifiedPeerCertificate temporary;std::uint64_t expires=0;
            if(!review_ || now>=expires_ || !review_->approved_peer(now,temporary,expires) || expires!=expires_ || temporary.pem!=peer_.pem)return fail();
        }
        if(connection_)return connection_->step(now) || fail();
        if(dialer_) {
            const auto progress=dialer_->step(now);if(progress==TcpProgress::failed)return fail();if(progress==TcpProgress::pending)return true;
            selector_=PeerSelector::sender(dialer_->release(),identity_.fingerprint,now);dialer_.reset();if(!selector_)return fail();
        }
        if(!selector_)return fail();
        const auto progress=selector_->step(now);if(progress==PairIo::failed)return fail();if(progress==PairIo::pending)return true;
        if(incoming_) {
            if(!store_->load_peer(selector_->hint(),peer_)) {
                if(!review_ || selector_->hint()!=review_->remote_fingerprint() || !review_->approved_peer(now,peer_,expires_))return fail();
                first_pair_=true;
            }
        }
        const auto socket=selector_->release();selector_.reset();if(socket==INVALID_SOCKET || !install(socket,!incoming_,now))return fail();
        return connection_->step(now) || fail();
    } catch(...) {return fail();}
}
}
