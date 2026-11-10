#pragma once
#include "hop_windows/connection_classifier.h"
#include "hop_windows/tcp_endpoint.h"
#include <memory>
#include <vector>
namespace hop::windows {
class ClassifiedSocket {
public:
    ClassifiedSocket(SOCKET socket,ConnectionKind kind) : socket_(socket),kind_(kind) {}
    ~ClassifiedSocket(){if(socket_!=INVALID_SOCKET)closesocket(socket_);}
    ClassifiedSocket(const ClassifiedSocket&)=delete;ClassifiedSocket& operator=(const ClassifiedSocket&)=delete;
    ConnectionKind kind() const {return kind_;}
    SOCKET release(){auto s=socket_;socket_=INVALID_SOCKET;return s;}
private: SOCKET socket_;ConnectionKind kind_;
};
// One user-enabled listener; all candidates remain untrusted. Bounded pending
// classifiers prevent untrusted peers from opening unbounded resources.
class LanServer {
public:
    static std::unique_ptr<LanServer> start(const TcpAddress& address,bool explicitly_enabled);
    bool local_address(TcpAddress& out) const {return listener_->local_address(out);}
    bool open_pairing_window(std::uint64_t now_ms,std::uint64_t duration_ms);
    void close_pairing_window(){pair_until_=0;pending_.clear();}
    // Poll from one worker; at most one result returned per call. The caller
    // routes HPOP to CandidateExchange and HPSL to ConnectionPath.
    bool poll(std::uint64_t now_ms,std::unique_ptr<ClassifiedSocket>& result);
private:
    explicit LanServer(std::unique_ptr<TcpListener> listener) : listener_(std::move(listener)) {}
    std::unique_ptr<TcpListener> listener_;std::vector<std::unique_ptr<ConnectionClassifier>> pending_;
    std::uint64_t pair_until_=0,last_now_=0;
};
}
