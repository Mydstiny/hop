#pragma once
#include <winsock2.h>
#include <cstdint>
#include <memory>
#include <string>
namespace hop::windows {
constexpr std::uint16_t default_v2_port=24891; // discovered service port takes precedence
struct TcpAddress { sockaddr_storage value{}; int size=0; };
// Numeric addresses only; resolution/discovery is a separate untrusted layer.
bool numeric_tcp_address(const std::string& address,std::uint16_t port,TcpAddress& out,std::uint32_t ipv6_scope_id=0);
enum class TcpProgress { pending, ready, failed };
// WSAStartup must already have succeeded. Both classes own sockets; opening a
// listener or connection requires an explicit action from the product owner.
class TcpListener {
public:
    static std::unique_ptr<TcpListener> open(const TcpAddress& address,bool explicitly_enabled);
    ~TcpListener();
    bool local_address(TcpAddress& out) const;
    TcpProgress accept(SOCKET& out);
private:
    explicit TcpListener(SOCKET socket) : socket_(socket) {}
    SOCKET socket_=INVALID_SOCKET;
};
class TcpDialer {
public:
    static std::unique_ptr<TcpDialer> start(const TcpAddress& address,bool explicitly_requested,std::uint64_t now_ms);
    ~TcpDialer();
    TcpProgress step(std::uint64_t now_ms);
    SOCKET release();
private:
    TcpDialer(SOCKET socket,std::uint64_t now,bool ready) : socket_(socket),started_(now),last_now_(now),ready_(ready) {}
    SOCKET socket_=INVALID_SOCKET; std::uint64_t started_,last_now_; bool ready_=false,failed_=false;
};
}
