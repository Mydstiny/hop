#include "v2_receiver.h"
#include "v2_injector.h"
#include "hop/posix_session_pump.h"
#include "hop/tls_identity.h"
#include "hop/pair_offer.h"
#include "hop/secure_channel_c.h"
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <utility>
namespace hop {
namespace {
using Clock=std::chrono::steady_clock;
void clear(std::string& s){if(!s.empty()){volatile char* p=&s[0];for(std::size_t i=0;i<s.size();++i)p[i]=0;s.clear();}}
struct CredentialsWiper { PosixTlsCredentials& c; ~CredentialsWiper(){clear(c.private_key_pem);} };
struct Fd {int value=-1;~Fd(){if(value>=0)close(value);} int take(){int v=value;value=-1;return v;}};
bool waitFd(int fd,short events,const std::atomic<bool>& stopped,Clock::time_point deadline){
    while(!stopped && Clock::now()<deadline){pollfd p{fd,events,0};int n=poll(&p,1,25);if(n>0)return (p.revents&(events|POLLERR|POLLHUP))!=0;if(n<0&&errno!=EINTR)return false;}return false;
}
int openPeer(const V2ReceiverConfig& c,const std::atomic<bool>& stopped){
    sockaddr_storage address{};socklen_t length=0;int family=AF_UNSPEC;
    sockaddr_in v4{};v4.sin_family=AF_INET;v4.sin_port=htons(c.port);
    if(inet_pton(AF_INET,c.host.c_str(),&v4.sin_addr)==1){std::memcpy(&address,&v4,sizeof(v4));length=sizeof(v4);family=AF_INET;}
    else {
        sockaddr_in6 v6{};v6.sin6_family=AF_INET6;v6.sin6_port=htons(c.port);
        std::string host=c.host;auto percent=host.find('%');
        if(percent!=std::string::npos){std::string iface=host.substr(percent+1);host.resize(percent);v6.sin6_scope_id=if_nametoindex(iface.c_str());if(!v6.sin6_scope_id)return -1;}
        if(inet_pton(AF_INET6,host.c_str(),&v6.sin6_addr)!=1)return -1;
        std::memcpy(&address,&v6,sizeof(v6));length=sizeof(v6);family=AF_INET6;
    }
    Fd fd;fd.value=socket(family,SOCK_STREAM,0);if(fd.value<0)return -1;
    int flags=fcntl(fd.value,F_GETFL,0),desc=fcntl(fd.value,F_GETFD,0),one=1;
    if(flags<0||desc<0||fcntl(fd.value,F_SETFL,flags|O_NONBLOCK)<0||fcntl(fd.value,F_SETFD,desc|FD_CLOEXEC)<0||setsockopt(fd.value,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one))!=0)return -1;
#ifdef SO_NOSIGPIPE
    if(setsockopt(fd.value,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof(one))!=0)return -1;
#endif
    if(connect(fd.value,reinterpret_cast<sockaddr*>(&address),length)!=0){
        if(errno!=EINPROGRESS||!waitFd(fd.value,POLLOUT,stopped,Clock::now()+std::chrono::seconds(5)))return -1;
        int error=0;socklen_t n=sizeof(error);if(getsockopt(fd.value,SOL_SOCKET,SO_ERROR,&error,&n)!=0||error)return -1;
    }
    return fd.take();
}
bool selector(int fd,const std::string& fingerprint,const std::atomic<bool>& stopped){
    const auto data=encode_peer_selector(fingerprint);if(data.empty())return false;
    auto deadline=Clock::now()+std::chrono::seconds(5);std::size_t done=0;
    while(done<data.size()&&!stopped&&Clock::now()<deadline){
        int flags=0;
#ifdef MSG_NOSIGNAL
        flags|=MSG_NOSIGNAL;
#endif
        const auto n=send(fd,data.data()+done,data.size()-done,flags);
        if(n>0){done+=static_cast<std::size_t>(n);continue;}
        if(n<0&&(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK)){if(!waitFd(fd,POLLOUT,stopped,deadline))return false;continue;}
        return false;
    }
    return done==data.size();
}
void u32(std::vector<std::uint8_t>& out,std::uint32_t n){for(int i=3;i>=0;--i)out.push_back(static_cast<std::uint8_t>(n>>(8*i)));}
void u64(std::vector<std::uint8_t>& out,std::uint64_t n){for(int i=7;i>=0;--i)out.push_back(static_cast<std::uint8_t>(n>>(8*i)));}
std::vector<std::uint8_t> screenPayload(const std::vector<V2Display>& displays) {
    std::vector<std::uint8_t> p={1,3,0,0,0,static_cast<std::uint8_t>(displays.size()),0,0};
    for(const auto& d:displays){u64(p,d.screenId());u32(p,static_cast<std::uint32_t>(d.x));u32(p,static_cast<std::uint32_t>(d.y));u32(p,d.width);u32(p,d.height);}
    return p;
}
}
V2Receiver::V2Receiver(Sink sink):sink_(std::move(sink)),injector_(std::make_unique<V2Injector>()){}
V2Receiver::~V2Receiver(){stop();}
V2ReceiverConfig::~V2ReceiverConfig(){clear(private_key_pem);}
V2ReceiverConfig::V2ReceiverConfig(V2ReceiverConfig&& other) noexcept { *this=std::move(other); }
V2ReceiverConfig& V2ReceiverConfig::operator=(V2ReceiverConfig&& other) noexcept {
    if(this!=&other){clear(private_key_pem);host=std::move(other.host);certificate_pem=std::move(other.certificate_pem);
        private_key_pem=std::move(other.private_key_pem);verified_peer_certificate_pem=std::move(other.verified_peer_certificate_pem);
        port=other.port;request_id=other.request_id;displays=std::move(other.displays);require_pairing=other.require_pairing;
        clipboard=std::move(other.clipboard);sample_clipboard=std::move(other.sample_clipboard);}
    return *this;
}
bool V2Receiver::start(V2ReceiverConfig c){
    if(running_ || !c.port || !validV2Displays(c.displays) || c.host.empty() || c.host.find('\0')!=std::string::npos) {clear(c.private_key_pem);return false;}
    if(thread_.joinable())thread_.join();
    if(c.require_pairing){
        std::lock_guard<std::mutex> lock(candidateMutex_);
        if(candidateCertificate_.empty() || Clock::now()>=candidateExpiry_ || candidateCertificate_!=c.verified_peer_certificate_pem) {clear(c.private_key_pem);return false;}
        candidateCertificate_.clear();
    }
    stopped_=false;confirm_=false;cancel_=false;saved_=false;running_=true;
    try{thread_=std::thread([this,config=std::move(c)]() mutable {run(std::move(config));running_=false;});return true;}catch(...){running_=false;clear(c.private_key_pem);return false;}
}
void V2Receiver::clearCandidate(){std::lock_guard<std::mutex> lock(candidateMutex_);candidateCertificate_.clear();}
void V2Receiver::cancelPairing(){cancel_=true;clearCandidate();}
void V2Receiver::stop(){stopped_=true;if(thread_.joinable())thread_.join();clearCandidate();}
bool V2Receiver::startOffer(const std::string& host,std::uint16_t port,const std::string& name,const std::string& certificate,std::uint32_t request_id){
    if(running_||host.empty()||host.find('\0')!=std::string::npos||!port||name.empty()||certificate.empty())return false;
    if(thread_.joinable())thread_.join();clearCandidate();stopped_=false;cancel_=false;confirm_=false;saved_=false;running_=true;
    try{thread_=std::thread([this,host,port,name,certificate,request_id]{runOffer(host,port,name,certificate,request_id);running_=false;});return true;}catch(...){running_=false;return false;}
}
void V2Receiver::runOffer(std::string host,std::uint16_t port,std::string name,std::string certificate,std::uint32_t request_id) noexcept {
    auto emit=[this,request_id](V2ReceiverEvent event){event.request_id=request_id;try{if(sink_)sink_(event);}catch(...){}};
    try {
        PairOffer local;local.role=2;local.name=std::move(name);local.certificate_pem=std::move(certificate);
        if(hop_tls_random_bytes(local.nonce.data(),local.nonce.size())!=0){emit({"error","Cannot create pairing request","","",""});return;}
        auto bytes=encode_pair_offer(local);std::string localFingerprint;
        if(bytes.empty()||!tls_certificate_fingerprint(local.certificate_pem,localFingerprint)){emit({"error","Invalid local pairing identity","","",""});return;}
        V2ReceiverConfig address;address.host=std::move(host);address.port=port;Fd fd;fd.value=openPeer(address,stopped_);
        if(fd.value<0){if(!stopped_)emit({"error","Cannot reach pairing service","","",""});return;}
        const auto deadline=Clock::now()+std::chrono::seconds(10);std::size_t sent=0;
        while(sent<bytes.size()&&!stopped_&&!cancel_&&Clock::now()<deadline){
            int flags=0;
#ifdef MSG_NOSIGNAL
            flags|=MSG_NOSIGNAL;
#endif
            auto n=send(fd.value,bytes.data()+sent,bytes.size()-sent,flags);
            if(n>0){sent+=static_cast<std::size_t>(n);continue;}
            if(n<0&&(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK)){if(!waitFd(fd.value,POLLOUT,stopped_,deadline))break;continue;}break;
        }
        if(sent!=bytes.size()){if(!stopped_)emit({"error","Pairing request failed","","",""});return;}
        // End the single public offer before reading the response. The peer
        // must observe EOF to reject any trailing bytes without guessing.
        if(shutdown(fd.value,SHUT_WR)!=0){if(!stopped_)emit({"error","Pairing request failed","","",""});return;}
        PairOfferDecoder decoder;std::array<std::uint8_t,4096> buffer{};bool eof=false;
        while(!stopped_&&!cancel_&&Clock::now()<deadline){
            auto n=recv(fd.value,buffer.data(),buffer.size(),0);
            if(n>0){if(!decoder.feed(buffer.data(),static_cast<std::size_t>(n)))break;continue;}
            if(n==0){eof=true;break;}
            if(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK){if(!waitFd(fd.value,POLLIN,stopped_,deadline))break;continue;}break;
        }
        if(stopped_||cancel_)return;
        auto candidate=eof?decoder.finish():std::nullopt;std::string fingerprint;
        if(!candidate||candidate->role!=1||!tls_certificate_fingerprint(candidate->certificate_pem,fingerprint)||fingerprint==localFingerprint){emit({"error","Invalid pairing candidate","","",""});return;}
        {std::lock_guard<std::mutex> lock(candidateMutex_);if(stopped_||cancel_)return;candidateCertificate_=candidate->certificate_pem;candidateExpiry_=Clock::now()+std::chrono::seconds(120);}
        emit({"candidate","Compare both device fingerprints",fingerprint,candidate->certificate_pem,candidate->name});
    }catch(...){emit({"error","Pairing request failed","","",""});}
}
void V2Receiver::run(V2ReceiverConfig c) noexcept {
    auto emit=[this,&c](const char* state,const char* message,const std::string& fingerprint=std::string()){try{if(sink_)sink_({state,message,fingerprint,"","",c.request_id});}catch(...){}};
    try {
        std::string localFingerprint,peerFingerprint;
        if(!tls_certificate_fingerprint(c.certificate_pem,localFingerprint)||!tls_certificate_fingerprint(c.verified_peer_certificate_pem,peerFingerprint)||
           localFingerprint==peerFingerprint||!validate_tls_identity(c.certificate_pem,c.private_key_pem)){emit("error","Invalid device identity or peer certificate");return;}
        emit("connecting","Connecting to verified peer",peerFingerprint);
        Fd fd;fd.value=openPeer(c,stopped_);
        if(fd.value<0||!selector(fd.value,localFingerprint,stopped_)){if(!stopped_)emit("error","Cannot connect to peer",peerFingerprint);return;}
        PosixTlsCredentials credentials{c.certificate_pem,std::move(c.private_key_pem),{c.verified_peer_certificate_pem}};
        CredentialsWiper credentialsWiper{credentials};
        auto transport=PosixTlsSocket::adopt(fd.take(),true,credentials);clear(credentials.private_key_pem);
        if(!transport){emit("error","Cannot configure secure channel",peerFingerprint);return;}
        const auto deadline=Clock::now()+std::chrono::seconds(10);
        while(!stopped_&&Clock::now()<deadline){auto p=transport->handshake();if(p==SocketProgress::ready)break;if(p==SocketProgress::failed){emit("error","Secure authentication failed",peerFingerprint);return;}
            const short interest=transport->io_want()==hop_tls_want_write_error()?POLLOUT:POLLIN;
            if(!waitFd(transport->native_fd(),interest,stopped_,deadline))break;}
        if(stopped_)return;
        if(!transport->authenticated()){emit("error","Secure authentication timed out",peerFingerprint);return;}
        auto& injector=*injector_;
        if(!injector.configure(c.displays)){emit("error","Previous input could not be released",peerFingerprint);return;}
        auto cb=V2Injector::callbacks();
        std::vector<std::uint64_t> screenIds;
        for(const auto& display:c.displays)screenIds.push_back(display.screenId());
        const auto& clip=c.clipboard;
        std::uint32_t clipboardBaseline=0;
        bool announced=false;
        v2::Session session(v2::SessionRole::receiver,0,std::move(screenIds),{
            [&](const Frame& f){return !stopped_&&announced&&cb.enter(&injector,f.payload.data(),f.payload.size(),f.epoch)==1;},
            [&](const Frame& f){return !stopped_&&cb.input(&injector,f.payload.data(),f.payload.size(),f.epoch)==1;},
            [&]{return cb.restore(&injector)==1;},
            [&]{if(clip)clip->revoke();}},Platform::harmonyos);
        if(clip && (!c.sample_clipboard || !session.enable_clipboard(
            [&]() -> std::optional<std::uint64_t> { clipboardBaseline=c.sample_clipboard();return clipboardBaseline; },
            [&](const ClipboardValue& value,std::uint64_t baseline){return clip->incoming(value,baseline);}))) {
            emit("error","Cannot start clipboard channel",peerFingerprint);return;
        }
        if(!session.bind_verified_peer(peerFingerprint)||(c.require_pairing&&!session.require_pairing_confirmation())||!session.start(true,0)){emit("error","Cannot start authenticated session",peerFingerprint);return;}
        PosixSessionPump pump(*transport,session,[&]{return !stopped_&&injector.beginBatch();},[&]{injector.endBatch();});
        const auto started=Clock::now();auto previous=v2::SessionPhase::initial;bool pairedNotified=false,storageConfirmed=false;
        std::uint64_t edgeEpoch=0, nextEdgeAt=0;
        std::uint64_t clipboardEpoch=0,clipboardStarted=0,transferId=0;
        bool sentReady=false;
        std::size_t offset=0;
        std::optional<V2ClipboardMailbox::Packet> outgoing;
        while(!stopped_){
            const auto now=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-started).count());
            if(cancel_.exchange(false)){if(session.phase()==v2::SessionPhase::pair_wait)session.cancel_pairing();else break;}
            if(confirm_.exchange(false) && (session.phase()!=v2::SessionPhase::pair_wait || !session.confirm_pairing())){emit("error","Pairing confirmation is no longer valid",peerFingerprint);break;}
            if(saved_.exchange(false)) {
                if(!session.pairing_confirmed()){emit("error","Pairing storage confirmation is invalid",peerFingerprint);break;}
                storageConfirmed=true;
            }
            if(!pump.step(now)){if(!stopped_)emit("error","Secure session disconnected",peerFingerprint);break;}
            auto phase=session.phase();
            if(clip && session.clipboard_formats()){
                if(phase==v2::SessionPhase::active && session.focus_epoch()!=clipboardEpoch){
                    clipboardEpoch=session.focus_epoch();clipboardStarted=now;sentReady=false;
                    clip->begin(clipboardEpoch,session.clipboard_formats(),clipboardBaseline);
                }
                if(phase==v2::SessionPhase::active && clip->state().active){
                    if(clip->pauseRequested()||(!sentReady && now-clipboardStarted>=1500)){
                        session.clipboard_pause();
                    } else if(!sentReady && clip->activated(clipboardEpoch)){
                        sentReady=session.clipboard_ready();
                    }
                    if(session.clipboard_initial_received() && clip->initialDrained(clipboardEpoch))session.clipboard_initial_drained();
                    if(!clip->permitted(clipboardEpoch)||!session.clipboard_available())outgoing.reset();
                    else if(!outgoing && session.clipboard_has_room(20)){
                        if(auto packet=clip->takeOutgoing()){
                            if(transferId==UINT64_MAX)break;
                            if(session.clipboard_start(++transferId,packet->format,static_cast<std::uint32_t>(packet->bytes.size()))&&!packet->bytes.empty()){
                                outgoing.emplace(std::move(*packet));offset=0;
                            }
                        }
                    }
                    if(outgoing){
                        auto remaining=outgoing->bytes.size()-offset;
                        auto count=std::min(remaining,max_clipboard_chunk-16);
                        if(session.clipboard_has_room(count+16)){
                            if(!session.clipboard_chunk(transferId,outgoing->bytes.data()+offset,count))break;
                            offset+=count;if(offset==outgoing->bytes.size())outgoing.reset();
                        }
                    }
                } else outgoing.reset();
                if(phase==v2::SessionPhase::release_ack_wait && clip->drained() && !session.complete_release(now))break;
            }
            if(phase==v2::SessionPhase::active) {
                if(edgeEpoch!=session.focus_epoch()) {edgeEpoch=session.focus_epoch();nextEdgeAt=0;}
                std::uint16_t edgeX=0,edgeY=0;
                // Do not consume a pending edge until it can be sent.
                if(now>=nextEdgeAt && injector.takeEdge(edgeX,edgeY)) {
                    if(!session.request_leave(edgeX,edgeY)){emit("error","Cannot request edge transition",peerFingerprint);break;}
                    nextEdgeAt=now+120;
                }
            }
            if(phase!=previous){
                if(phase==v2::SessionPhase::pair_wait)emit("pairing","Confirm both device fingerprints",peerFingerprint);
                else if(phase==v2::SessionPhase::ready)emit("ready","Secure session ready",peerFingerprint);
                else if(phase==v2::SessionPhase::active)emit("remote","Receiving input",peerFingerprint);
                previous=phase;
            }
            if(session.pairing_confirmed()&&!pairedNotified){pairedNotified=true;emit("paired","Both devices confirmed",peerFingerprint);}
            if(phase==v2::SessionPhase::ready&&!announced&&(!session.pairing_confirmed()||storageConfirmed)){if(!session.advertise_screens(screenPayload(c.displays))){emit("error","Cannot advertise screens",peerFingerprint);break;}announced=true;}
            pollfd p{transport->native_fd(),POLLIN,0};int n=poll(&p,1,20);if(n<0&&errno!=EINTR){emit("error","Network polling failed",peerFingerprint);break;}
        }
        pump.stop();
        if(!injector.release())emit("error","Input release is incomplete",peerFingerprint);
        else emit("disconnected","Secure session closed",peerFingerprint);
    } catch (...) {emit("error","Secure session failed");}
}
}
