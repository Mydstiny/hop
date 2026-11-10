#include "hop_windows/connection_classifier.h"
#include <cstring>
#include <new>
namespace hop::windows {
std::unique_ptr<ConnectionClassifier> ConnectionClassifier::accept(SOCKET socket,bool candidate,std::uint64_t now) {
    if(socket==INVALID_SOCKET)return nullptr;
    sockaddr_storage peer{};int length=sizeof(peer);u_long nonblocking=1;
    if(getpeername(socket,reinterpret_cast<sockaddr*>(&peer),&length)==SOCKET_ERROR || ioctlsocket(socket,FIONBIO,&nonblocking)==SOCKET_ERROR){closesocket(socket);return nullptr;}
    auto result=std::unique_ptr<ConnectionClassifier>(new(std::nothrow) ConnectionClassifier(socket,candidate,now));if(!result)closesocket(socket);return result;
}
ConnectionClassifier::~ConnectionClassifier(){if(socket_!=INVALID_SOCKET)closesocket(socket_);}
PairIo ConnectionClassifier::fail(){failed_=true;if(socket_!=INVALID_SOCKET){closesocket(socket_);socket_=INVALID_SOCKET;}return PairIo::failed;}
PairIo ConnectionClassifier::step(std::uint64_t now) {
    if(failed_ || now<last_now_ || now-started_>=10000)return fail();last_now_=now;if(kind_!=ConnectionKind::unknown)return PairIo::ready;
    char bytes[4]{};const int n=recv(socket_,bytes,sizeof(bytes),MSG_PEEK);
    if(n==SOCKET_ERROR){const auto e=WSAGetLastError();return e==WSAEWOULDBLOCK || e==WSAEINTR ? PairIo::pending : fail();}
    if(n==0)return fail();if(n<4)return PairIo::pending;
    if(std::memcmp(bytes,"HPOP",4)==0 && candidate_)kind_=ConnectionKind::candidate_offer;
    else if(std::memcmp(bytes,"HPSL",4)==0)kind_=ConnectionKind::trusted_selector;
    else return fail();
    return PairIo::ready;
}
SOCKET ConnectionClassifier::release(){if(failed_ || kind_==ConnectionKind::unknown)return INVALID_SOCKET;const auto value=socket_;socket_=INVALID_SOCKET;return value;}
}
