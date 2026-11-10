#include "hop_windows/connection.h"
#include <new>
namespace hop::windows {
std::unique_ptr<Connection> Connection::adopt(SOCKET socket,bool client,const TlsCredentials& credentials,Session& session,bool pairing,std::uint64_t now) {
    auto transport=TlsSocket::adopt(socket,client,credentials); if(!transport)return nullptr;
    if(!session.bind_verified_peer(transport->pinned_peer_fingerprint()) || (pairing && !session.require_pairing_confirmation())) return nullptr;
    std::unique_ptr<Connection> result(new (std::nothrow) Connection(std::move(transport),session,now));
    return result;
}
Connection::~Connection() {fail();}
bool Connection::fail() {
    if(!failed_) {failed_=true;session_.disconnect();if(transport_)transport_->close();}
    return false;
}
bool Connection::step(std::uint64_t now) {
    if(failed_ || now<last_now_)return fail();last_now_=now;
    try {
        if(!pump_) {
            if(now-started_>=10000)return fail();
            const auto state=transport_->handshake();
            if(state==TlsIoStatus::again)return true;
            if(state!=TlsIoStatus::ready || !session_.start(transport_->authenticated(),now))return fail();
            pump_=std::make_unique<SessionPump>(*transport_,session_);
        }
        if(!pump_->step(now))return fail();return true;
    } catch(...) {return fail();}
}
}
