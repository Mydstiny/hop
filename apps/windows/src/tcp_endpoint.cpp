#include "hop_windows/tcp_endpoint.h"
#include <ws2tcpip.h>
#include <new>
namespace hop::windows {
namespace {
bool valid(const TcpAddress& a) {
    return (a.value.ss_family==AF_INET && a.size==sizeof(sockaddr_in)) || (a.value.ss_family==AF_INET6 && a.size==sizeof(sockaddr_in6));
}
SOCKET nonblocking(int family) {
    const auto socket=::socket(family,SOCK_STREAM,IPPROTO_TCP); if(socket==INVALID_SOCKET)return INVALID_SOCKET;
    u_long enabled=1;if(ioctlsocket(socket,FIONBIO,&enabled)==SOCKET_ERROR){closesocket(socket);return INVALID_SOCKET;}return socket;
}
}
bool numeric_tcp_address(const std::string& text,std::uint16_t port,TcpAddress& out,std::uint32_t scope) {
    out={}; if(text.empty() || text.find('\0')!=std::string::npos)return false; sockaddr_in v4{};v4.sin_family=AF_INET;v4.sin_port=htons(port);
    if(InetPtonA(AF_INET,text.c_str(),&v4.sin_addr)==1){if(scope)return false;*reinterpret_cast<sockaddr_in*>(&out.value)=v4;out.size=sizeof(v4);return true;}
    sockaddr_in6 v6{};v6.sin6_family=AF_INET6;v6.sin6_port=htons(port);v6.sin6_scope_id=scope;
    if(InetPtonA(AF_INET6,text.c_str(),&v6.sin6_addr)==1){*reinterpret_cast<sockaddr_in6*>(&out.value)=v6;out.size=sizeof(v6);return true;}
    return false;
}
std::unique_ptr<TcpListener> TcpListener::open(const TcpAddress& address,bool enabled) {
    if(!enabled || !valid(address))return nullptr;const auto socket=nonblocking(address.value.ss_family);if(socket==INVALID_SOCKET)return nullptr;
    BOOL exclusive=TRUE;
    if(setsockopt(socket,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char*>(&exclusive),sizeof(exclusive))==SOCKET_ERROR ||
       bind(socket,reinterpret_cast<const sockaddr*>(&address.value),address.size)==SOCKET_ERROR || listen(socket,4)==SOCKET_ERROR){closesocket(socket);return nullptr;}
    auto result=std::unique_ptr<TcpListener>(new(std::nothrow) TcpListener(socket));if(!result)closesocket(socket);return result;
}
TcpListener::~TcpListener(){if(socket_!=INVALID_SOCKET)closesocket(socket_);}
bool TcpListener::local_address(TcpAddress& out) const {out={};out.size=sizeof(out.value);return getsockname(socket_,reinterpret_cast<sockaddr*>(&out.value),&out.size)==0 && valid(out);}
TcpProgress TcpListener::accept(SOCKET& out) {
    out=INVALID_SOCKET;const auto value=::accept(socket_,nullptr,nullptr);
    if(value==INVALID_SOCKET)return WSAGetLastError()==WSAEWOULDBLOCK?TcpProgress::pending:TcpProgress::failed;
    u_long enabled=1;if(ioctlsocket(value,FIONBIO,&enabled)==SOCKET_ERROR){closesocket(value);return TcpProgress::failed;}
    out=value;return TcpProgress::ready;
}
std::unique_ptr<TcpDialer> TcpDialer::start(const TcpAddress& address,bool requested,std::uint64_t now) {
    if(!requested || !valid(address))return nullptr;const auto socket=nonblocking(address.value.ss_family);if(socket==INVALID_SOCKET)return nullptr;
    const int rc=::connect(socket,reinterpret_cast<const sockaddr*>(&address.value),address.size);
    if(rc==SOCKET_ERROR && WSAGetLastError()!=WSAEWOULDBLOCK){closesocket(socket);return nullptr;}
    auto out=std::unique_ptr<TcpDialer>(new(std::nothrow) TcpDialer(socket,now,rc==0));if(!out)closesocket(socket);return out;
}
TcpDialer::~TcpDialer(){if(socket_!=INVALID_SOCKET)closesocket(socket_);}
TcpProgress TcpDialer::step(std::uint64_t now) {
    if(failed_ || now<last_now_ || now-started_>=10000){failed_=true;return TcpProgress::failed;}last_now_=now;if(ready_)return TcpProgress::ready;
    fd_set write_set,error_set;FD_ZERO(&write_set);FD_ZERO(&error_set);FD_SET(socket_,&write_set);FD_SET(socket_,&error_set);timeval timeout{};
    const int selected=select(0,nullptr,&write_set,&error_set,&timeout);if(selected==SOCKET_ERROR){failed_=true;return TcpProgress::failed;}
    if(!selected)return TcpProgress::pending;
    int error=0,length=sizeof(error);if(FD_ISSET(socket_,&error_set) || getsockopt(socket_,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&length)==SOCKET_ERROR || error){failed_=true;return TcpProgress::failed;}
    ready_=true;return TcpProgress::ready;
}
SOCKET TcpDialer::release(){if(!ready_ || failed_)return INVALID_SOCKET;auto value=socket_;socket_=INVALID_SOCKET;return value;}
}
