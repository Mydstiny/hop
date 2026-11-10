#include "hop_windows/pair_transport.h"
#include "hop/secure_channel_c.h"
#include <ws2tcpip.h>
#include <algorithm>
#include <array>
#include <new>
#include <limits>
#include <utility>
namespace hop::windows {
namespace {
bool prepare(SOCKET socket) {
    if (socket==INVALID_SOCKET) return false;
    sockaddr_storage peer{}; int length=sizeof(peer); if (getpeername(socket,reinterpret_cast<sockaddr*>(&peer),&length)==SOCKET_ERROR) return false;
    u_long nonblocking=1; return ioctlsocket(socket,FIONBIO,&nonblocking)!=SOCKET_ERROR;
}
bool temporary_error() { auto error=WSAGetLastError(); return error==WSAEWOULDBLOCK || error==WSAEINTR; }
}
std::unique_ptr<CandidateExchange> CandidateExchange::start(SOCKET socket,bool requested,bool initiator,std::uint8_t role,std::string name,std::string certificate,std::uint64_t now) {
    std::unique_ptr<CandidateExchange> out(new (std::nothrow) CandidateExchange(socket));
    if (!out) { if (socket!=INVALID_SOCKET) closesocket(socket); return nullptr; }
    if (!requested || now>std::numeric_limits<std::uint64_t>::max()-120000 || !prepare(socket)) return nullptr;
    try {
    hop::PairOffer offer; offer.role=role; offer.name=std::move(name); offer.certificate_pem=std::move(certificate);
    if (hop_tls_random_bytes(offer.nonce.data(),offer.nonce.size())!=0) return nullptr;
    out->outgoing_=hop::encode_pair_offer(offer); if (out->outgoing_.empty()) return nullptr;
    out->role_=role;out->initiator_=initiator;out->started_=out->last_now_=now; return out;
    } catch (...) {return nullptr;}
}
CandidateExchange::~CandidateExchange() { if (socket_!=INVALID_SOCKET) closesocket(socket_); }
PairIo CandidateExchange::fail() {
    failed_=true; candidate_.reset(); if (socket_!=INVALID_SOCKET) { closesocket(socket_); socket_=INVALID_SOCKET; } return PairIo::failed;
}
PairIo CandidateExchange::step(std::uint64_t now) {
    try {
    if (failed_ || now<last_now_) return fail();
    if (done_) return PairIo::ready;
    last_now_=now; if (now-started_>=30000) return fail();
    if (!sent_ && (initiator_ || received_)) {
        const int n=send(socket_,reinterpret_cast<const char*>(outgoing_.data()+sent_bytes_),static_cast<int>(outgoing_.size()-sent_bytes_),0);
        if (n>0) sent_bytes_+=static_cast<std::size_t>(n);
        else if (!n || !temporary_error()) return fail();
        if (sent_bytes_==outgoing_.size()) { if (shutdown(socket_,SD_SEND)==SOCKET_ERROR) return fail(); sent_=true; }
    }
    if (!received_ && (!initiator_ || sent_)) {
        std::array<std::uint8_t,4096> bytes{}; const int n=recv(socket_,reinterpret_cast<char*>(bytes.data()),static_cast<int>(bytes.size()),0);
        if (n>0) { if (!decoder_.feed(bytes.data(),static_cast<std::size_t>(n))) return fail(); }
        else if (!n) {
            auto offer=decoder_.finish(); if (!offer || offer->role==role_) return fail();
            char fp[65]{}; if (hop_tls_certificate_fingerprint(offer->certificate_pem.data(),offer->certificate_pem.size(),fp,sizeof(fp))!=0) return fail();
            candidate_=UntrustedCandidate{std::move(*offer),fp,started_,started_+120000}; received_=true;
        } else if (!temporary_error()) return fail();
    }
    if (sent_ && received_) { closesocket(socket_); socket_=INVALID_SOCKET; done_=true; return PairIo::ready; }
    return PairIo::pending;
    } catch (...) {return fail();}
}
std::optional<UntrustedCandidate> CandidateExchange::take(std::uint64_t now) {
    if (!done_ || !candidate_ || !candidate_->valid_at(now)) {candidate_.reset();return std::nullopt;}
    return std::exchange(candidate_,std::nullopt);
}
std::unique_ptr<PeerSelector> PeerSelector::sender(SOCKET socket,const std::string& own,std::uint64_t now) {
    std::unique_ptr<PeerSelector> out(new (std::nothrow) PeerSelector(socket,true,now)); if (!out) { if(socket!=INVALID_SOCKET) closesocket(socket); return nullptr; } if (!prepare(socket)) return nullptr;
    try { auto bytes=hop::encode_peer_selector(own); if (bytes.size()!=out->bytes_.size()) return nullptr;
    std::copy(bytes.begin(),bytes.end(),out->bytes_.begin()); return out;
    } catch (...) {return nullptr;}
}
std::unique_ptr<PeerSelector> PeerSelector::receiver(SOCKET socket,std::uint64_t now) {
    std::unique_ptr<PeerSelector> out(new (std::nothrow) PeerSelector(socket,false,now)); if (!out) { if(socket!=INVALID_SOCKET) closesocket(socket); return nullptr; } if (!prepare(socket)) return nullptr; return out;
}
PeerSelector::~PeerSelector() { if (socket_!=INVALID_SOCKET) closesocket(socket_); }
PairIo PeerSelector::fail() { failed_=true; if(socket_!=INVALID_SOCKET) {closesocket(socket_);socket_=INVALID_SOCKET;} return PairIo::failed; }
PairIo PeerSelector::step(std::uint64_t now) {
    try {
    if (failed_ || now<last_now_) return fail(); if (ready_) return PairIo::ready;
    last_now_=now; if (now-started_>=10000) return fail();
    const auto remaining=static_cast<int>(bytes_.size()-used_);
    const int n=sending_ ? send(socket_,reinterpret_cast<const char*>(bytes_.data()+used_),remaining,0) : recv(socket_,reinterpret_cast<char*>(bytes_.data()+used_),remaining,0);
    if (n>0) used_+=static_cast<std::size_t>(n); else if (!n || !temporary_error()) return fail();
    if (used_==bytes_.size()) {
        if (!sending_) { auto hint=hop::decode_peer_selector(bytes_.data(),bytes_.size()); if (!hint) return fail(); hint_=std::move(*hint); }
        ready_=true; return PairIo::ready;
    }
    return PairIo::pending;
    } catch (...) {return fail();}
}
SOCKET PeerSelector::release() { if (!ready_ || failed_) return INVALID_SOCKET; auto out=socket_; socket_=INVALID_SOCKET; return out; }
}
