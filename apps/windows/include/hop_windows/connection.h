#pragma once
#include "hop_windows/session_pump.h"
#include <memory>
namespace hop::windows {
// Serial worker for one HPSL-selected, already connected TCP stream. The peer
// certificate must be from the trust store or an explicitly approved temporary
// candidate. adopt consumes the socket on all outcomes. No fallback or discovery.
class Connection {
public:
    static std::unique_ptr<Connection> adopt(SOCKET socket,bool tcp_client,const TlsCredentials& credentials,
                                             Session& session,bool needs_pair_confirmation,std::uint64_t now_ms);
    ~Connection();
    bool step(std::uint64_t now_ms);
    bool authenticated() const {return transport_ && transport_->authenticated();}
    const TlsSocket& transport() const {return *transport_;}
private:
    Connection(std::unique_ptr<TlsSocket> transport,Session& session,std::uint64_t now)
      : transport_(std::move(transport)),session_(session),started_(now),last_now_(now) {}
    bool fail();
    std::unique_ptr<TlsSocket> transport_;
    Session& session_; std::unique_ptr<SessionPump> pump_;
    std::uint64_t started_,last_now_; bool failed_=false;
};
}
