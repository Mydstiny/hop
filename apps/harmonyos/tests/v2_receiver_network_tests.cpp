#include "v2_receiver.h"
#include "hop/posix_session_pump.h"
#include "hop/secure_channel_c.h"
#include "hop/pair_offer.h"
#include <multimodalinput/oh_input_manager.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
using namespace hop;
namespace {
int checks=0;void check(bool ok,const char* why){++checks;if(!ok){std::cerr<<"FAIL "<<why<<'\n';std::exit(1);}}
struct Identity{std::array<char,8192> cert{},key{};std::array<char,65> fp{};Identity(){check(hop_tls_generate_identity(cert.data(),cert.size(),key.data(),key.size(),fp.data(),fp.size())==0,"test identity");}~Identity(){volatile char* p=key.data();for(std::size_t i=0;i<key.size();++i)p[i]=0;}};
std::uint64_t ms(std::chrono::steady_clock::time_point start){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count());}
}
int main(){
    fake::Reset();Identity hostId,receiverId;
    int listener=socket(AF_INET,SOCK_STREAM,0);check(listener>=0,"listener");sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    check(bind(listener,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==0&&listen(listener,1)==0,"bind localhost");socklen_t len=sizeof(addr);check(getsockname(listener,reinterpret_cast<sockaddr*>(&addr),&len)==0,"port");
    std::mutex mutex;std::vector<std::string> states;
    V2Receiver receiver([&](const V2ReceiverEvent& event){std::lock_guard<std::mutex> lock(mutex);states.push_back(event.state);});
    check(receiver.startOffer("127.0.0.1",ntohs(addr.sin_port),"Harmony test",receiverId.cert.data()),"start explicit candidate offer");
    pollfd offerPoll{listener,POLLIN,0};check(poll(&offerPoll,1,5000)>0,"incoming candidate offer");
    int offerFd=accept(listener,nullptr,nullptr);check(offerFd>=0,"accept offer");
    std::array<uint8_t,28> header{};std::size_t used=0;
    while(used<header.size()){auto n=recv(offerFd,header.data()+used,header.size()-used,0);check(n>0,"read offer header");used+=static_cast<std::size_t>(n);}
    const auto nameSize=(std::size_t(header[6])<<8)|header[7];
    const auto certSize=(std::size_t(header[8])<<24)|(std::size_t(header[9])<<16)|(std::size_t(header[10])<<8)|header[11];
    check(nameSize<=128&&certSize<=16384,"offer bounds");std::vector<uint8_t> rest(nameSize+certSize);used=0;
    while(used<rest.size()){auto n=recv(offerFd,rest.data()+used,rest.size()-used,0);check(n>0,"read offer body");used+=static_cast<std::size_t>(n);}
    PairOfferDecoder offerDecoder;check(offerDecoder.feed(header.data(),header.size())&&offerDecoder.feed(rest.data(),rest.size()),"parse receiver offer");
    auto offered=offerDecoder.finish();check(offered&&offered->role==2&&offered->certificate_pem==receiverId.cert.data(),"receiver candidate is public identity");
    PairOffer hostOffer;hostOffer.role=1;hostOffer.nonce.fill(1);hostOffer.name="Host test";hostOffer.certificate_pem=hostId.cert.data();auto offerBytes=encode_pair_offer(hostOffer);
    used=0;while(used<offerBytes.size()){auto n=send(offerFd,offerBytes.data()+used,offerBytes.size()-used,0);check(n>0,"send offer response");used+=static_cast<std::size_t>(n);}close(offerFd);
    for(int i=0;i<1000&&receiver.isRunning();++i)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    check(!receiver.isRunning(),"candidate worker completed");
    bool candidate=false;{std::lock_guard<std::mutex> lock(mutex);for(const auto& state:states)if(state=="candidate")candidate=true;}
    check(candidate,"candidate delivered for independent comparison");
    V2ReceiverConfig config;config.host="127.0.0.1";config.port=ntohs(addr.sin_port);config.certificate_pem=receiverId.cert.data();config.private_key_pem=receiverId.key.data();config.verified_peer_certificate_pem=hostId.cert.data();config.displays={{6,0,0,1001,501},{15,-800,-100,800,600}};config.require_pairing=true;
    auto mailbox=std::make_shared<V2ClipboardMailbox>(9);
    std::atomic<std::uint32_t> clipboardSequence{11}, samples{0};
    config.request_id=9;config.clipboard=mailbox;
    config.sample_clipboard=[&]{++samples;return clipboardSequence.load();};
    check(receiver.start(std::move(config)),"start receiver worker");
    pollfd p{listener,POLLIN,0};check(poll(&p,1,5000)>0,"incoming connection");int fd=accept(listener,nullptr,nullptr);close(listener);check(fd>=0,"accept");
    std::array<char,68> selector{};std::size_t got=0;while(got<selector.size()){const auto n=recv(fd,selector.data()+got,selector.size()-got,0);check(n>0,"read selector");got+=static_cast<std::size_t>(n);}
    check(std::memcmp(selector.data(),"HPSL",4)==0&&std::memcmp(selector.data()+4,receiverId.fp.data(),64)==0,"selector is receiver public fingerprint");
    auto transport=PosixTlsSocket::adopt(fd,false,{hostId.cert.data(),hostId.key.data(),{receiverId.cert.data()}});check(bool(transport),"host TLS adapter");
    auto started=std::chrono::steady_clock::now();bool ready=false;
    for(int i=0;i<5000;++i){auto progress=transport->handshake();check(progress!=SocketProgress::failed,"TLS handshake");if(progress==SocketProgress::ready){ready=true;break;}std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    check(ready,"pinned handshake complete");int restores=0;
    v2::Session host(v2::SessionRole::host,42,{},{{},{},[&]{++restores;return true;},[]{}},Platform::macos);
    const std::vector<std::uint8_t> reply={'r','e','p','l','y'};
    bool gotReply=false;
    check(host.enable_clipboard([]{return 8;},[&](const ClipboardValue& value,std::uint64_t baseline){
        gotReply=value.format==ClipboardFormat::text && value.epoch==43 && baseline==8 && value.data==reply;
        return gotReply;
    }),"enable host clipboard hooks");
    bool edgeRequested=false;
    check(host.set_leave_handler([&](std::uint64_t id,std::uint16_t x,std::uint16_t){edgeRequested=true;return id==16&&x==0;}),"set edge handler");
    check(host.bind_verified_peer(receiverId.fp.data())&&host.require_pairing_confirmation()&&host.start(true,0),"start host pair session");
    PosixSessionPump pump(*transport,host);bool hostConfirmed=false,receiverConfirmed=false,storageConfirmed=false,entered=false;
    for(int i=0;i<5000;++i){
        check(pump.step(ms(started)),"session progress");
        if(host.phase()==v2::SessionPhase::pair_wait&&!hostConfirmed){check(host.confirm_pairing(),"host confirms");hostConfirmed=true;}
        bool pairing=false;{std::lock_guard<std::mutex> lock(mutex);for(const auto& state:states)if(state=="pairing")pairing=true;}
        if(pairing&&!receiverConfirmed){receiver.confirmPairing();receiverConfirmed=true;}
        bool paired=false;{std::lock_guard<std::mutex> lock(mutex);for(const auto& state:states)if(state=="paired")paired=true;}
        if(paired&&!storageConfirmed){receiver.confirmStorage();storageConfirmed=true;}
        if(host.phase()==v2::SessionPhase::ready&&!entered){check(samples==0,"no Harmony clipboard sampling before ENTER");entered=host.enter(43,16,32768,32768);}
        if(entered&&host.phase()==v2::SessionPhase::active)break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(hostConfirmed&&receiverConfirmed&&storageConfirmed&&host.phase()==v2::SessionPhase::active,"pair confirmation, storage acknowledgement and focus entry");
    const auto advertised=host.peer_screen_snapshot();
    check(advertised.size()==2&&advertised[0].id==7&&advertised[1].id==16&&advertised[1].x==-800&&advertised[1].y==-100&&
          advertised[1].width==800&&advertised[1].height==600,"TLS SCREENS carries both verified display rectangles");
    {std::lock_guard<std::mutex> lock(fake::mutex);const auto& move=fake::mouse.front();
     check(move.display_id==15&&move.x==-400&&move.y==200&&move.display_x==400&&move.display_y==300,"ENTER on extended display injects signed global and local coordinates");}
    auto waitFor=[&](auto condition,const char* reason){
        for(int i=0;i<1200&&!condition();++i){if(!pump.step(ms(started)))break;std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        check(condition(),reason);
    };
    waitFor([&]{return mailbox->state().active;},"mailbox begins after authenticated focus");
    check(samples==1&&mailbox->state().baseline==11&&!mailbox->permitted(43)&&!mailbox->activate(42),"sample baseline only, and deny actor until matching epoch activation");
    check(mailbox->activate(43)&&host.clipboard_ready(),"both actors confirm readiness");
    waitFor([&]{return host.clipboard_available();},"receiver READY travels over TLS");
    clipboardSequence=19;
    const std::vector<std::uint8_t> initial(70000,'x');
    check(host.clipboard_start(1,ClipboardFormat::text,initial.size())&&host.clipboard_chunk(1,initial.data(),40000)&&
          host.clipboard_chunk(1,initial.data()+40000,30000)&&host.clipboard_initial_done(),"queue fragmented initial snapshot and ordered marker");
    std::optional<V2ClipboardMailbox::Packet> received;
    waitFor([&]{if(!received){if(auto value=mailbox->takeIncoming(43))received.emplace(std::move(*value));}return received.has_value();},"initial snapshot passed to actor");
    check(received->bytes==initial&&received->baseline==11&&clipboardSequence==19,"initial snapshot carries ENTER baseline despite a newer local edit");
    check(!mailbox->state().poll,"receiver cannot poll while initial apply remains in progress");
    mailbox->finishIncoming(43);received.reset();
    waitFor([&]{return mailbox->state().poll;},"initial marker and actor drain unlock receiver polling");
    V2ClipboardMailbox::Packet outgoing;outgoing.epoch=43;outgoing.bytes=reply;
    check(mailbox->submit(std::move(outgoing)),"submit a new local copy from active receiver");
    waitFor([&]{return gotReply;},"receiver copy sent back to current host");
    check(host.send_input({1,4,0,0,0,0xe3,0,64,1,0,0,0})&&host.send_input({1,4,0,0,0,6,0,64,1,0,0,0}),"send translated shortcut");
    bool injected=false;
    for(int i=0;i<2000;++i){check(pump.step(ms(started)),"input progress");{std::lock_guard<std::mutex> lock(fake::mutex);injected=!fake::keys.empty();}if(injected)break;std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    check(injected,"input reached fake Harmony SDK over TLS");
    const std::vector<std::uint8_t> leftEdge={1,1,0,0,0,0,0,0,0,0,0,16,0,0,0x80,0};
    check(host.send_input(leftEdge),"queue edge movement");
    for(int i=0;i<2000&&!edgeRequested;++i){check(pump.step(ms(started)),"edge progress");std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    check(edgeRequested,"receiver requests return edge over TLS");
    const std::vector<std::uint8_t> next={'y'};
    check(host.clipboard_start(2,ClipboardFormat::text,next.size())&&host.clipboard_chunk(2,next.data(),next.size()),"queue another update");
    waitFor([&]{if(!received){if(auto value=mailbox->takeIncoming(43))received.emplace(std::move(*value));}return received.has_value();},"a second clipboard apply is running");
    check(received->baseline==19,"subsequent update uses current receiver clipboard sequence");
    check(host.release(0)&&host.complete_release(ms(started)),"restore local ownership without waiting for blocked receiver actor");
    waitFor([&]{return mailbox->state().draining && fake::Empty();},"receiver releases keys and revokes clipboard permit before ACK");
    check(!mailbox->permitted(43)&&!mailbox->acknowledge(43)&&host.phase()==v2::SessionPhase::release_wait,"release remains pending while an actor is in flight");
    mailbox->finishIncoming(43);
    check(mailbox->acknowledge(43),"only drained actor may acknowledge revocation");
    waitFor([&]{return host.phase()==v2::SessionPhase::ready;},"receiver drain returns the host to ready over TLS");
    check(host.enter(44,7,32768,32768),"enter again after release acknowledgement");
    waitFor([&]{return mailbox->state().active&&mailbox->state().epoch==44;},"new mailbox focus is isolated from the old epoch");
    {std::lock_guard<std::mutex> lock(fake::mutex);const auto& move=fake::mouse.back();
     check(move.display_id==6&&move.x==500&&move.y==250,"new focus moves on primary only after release acknowledgement");}
    check(!mailbox->activate(43)&&!mailbox->takeIncoming(43)&&!mailbox->permitted(43),"revoked actor cannot act on subsequent focus");
    waitFor([&]{return host.phase()==v2::SessionPhase::active;},"second focus acknowledged");
    check(host.send_input({1,4,0,0,0,4,0,0,1,0,0,0})&&host.send_input({1,2,0,0,0,0,0,0,0,0,0,7,0x80,0,0x80,0,1,1,1,0}),
          "queue a held key and button on the new display");
    waitFor([&]{std::lock_guard<std::mutex> lock(fake::mutex);return !fake::keys.empty()&&!fake::buttons.empty();},"both inputs held before topology stop");
    receiver.stop();check(fake::Empty(),"receiver worker stop releases both held inputs");
    {std::lock_guard<std::mutex> lock(fake::mutex);
     check(fake::mouse.back().action==MOUSE_ACTION_BUTTON_UP&&fake::mouse.back().display_id==6&&
           fake::key.back().action==KEY_ACTION_UP&&fake::key.back().display_id==6,"topology stop releases on the old focus display");}
    bool closed=false;for(int i=0;i<1000;++i){if(!pump.step(ms(started))){closed=true;break;}std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    check(closed&&restores>0,"host detects receiver shutdown");
    std::cout<<"PASS "<<checks<<" Harmony receiver worker localhost TLS checks; fake SDK only\n";
}
