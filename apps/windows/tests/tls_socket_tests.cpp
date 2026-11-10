#include "hop_windows/tls_socket.h"
#include "hop_windows/session_pump.h"
#include "hop_windows/trust_store.h"
#include "hop_windows/pair_transport.h"
#include "hop_windows/pairing_review.h"
#include "hop_windows/connection.h"
#include "hop_windows/tcp_endpoint.h"
#include "hop_windows/connection_path.h"
#include "hop_windows/connection_classifier.h"
#include "hop_windows/lan_server.h"
#include "hop_windows/pairing_worker.h"
#include "hop_windows/pairing_runtime.h"
#include "hop_windows/pairing_ui.h"
#include "hop_windows/host_capture_pump.h"
#include "hop_windows/receiver_session_adapter.h"
#include "hop_windows/clipboard_actor.h"
#include <windows.h>
#include <ws2tcpip.h>
#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include <map>
#include <deque>
#include <string>
using namespace hop::windows;
namespace {
int checks=0;
void check(bool value, const char* description) { ++checks; if(!value){std::cerr<<"FAIL "<<description<<'\n';std::exit(1);} }
struct MemoryCredentials : CredentialBackend {
    std::map<std::wstring,std::vector<std::uint8_t>> records; int writes=0; bool fail_read=false, fail_write=false;
    ~MemoryCredentials() { for(auto& record:records) if(!record.second.empty()) SecureZeroMemory(record.second.data(),record.second.size()); }
    CredentialRead read(const std::wstring& name,std::vector<std::uint8_t>& out) override {
        if(fail_read) return CredentialRead::error; auto found=records.find(name);
        if(found==records.end()) return CredentialRead::missing; out=found->second; return CredentialRead::found;
    }
    bool write(const std::wstring& name,const std::vector<std::uint8_t>& value) override {
        ++writes; if(fail_write) return false; records[name]=value; return true;
    }
    bool erase(const std::wstring& name) override {records.erase(name);return true;}
};
struct FakeCapture : CaptureSource {
    std::atomic<bool> running{false},enabled{false},restored{false},held{false},restore_any{false};std::atomic<int> restored_x{0},restored_y{0};std::mutex mutex;std::deque<CaptureEvent> events;
    void push(CaptureEvent event) {std::lock_guard<std::mutex> lock(mutex);events.push_back(event);}
    bool inputs_clear() override {return !held.load();}
    bool prepare(CursorPoint& original,CursorPoint& anchor,bool& caps) override {original={10,20};anchor={500,500};caps=false;return true;}
    bool start() override {running.store(true);return true;} bool activate() override {enabled.store(true);return true;}
    bool deactivate() override {enabled.store(false);return true;} bool active() const override {return enabled.load();}
    bool drained() const override {return true;} bool overflow() override {return false;} bool emergency() override {return false;}
    bool poll(CaptureEvent& event) override {std::lock_guard<std::mutex> lock(mutex);if(events.empty())return false;event=events.front();events.pop_front();return true;}
    bool stop() override {running.store(false);return true;} bool force_recovery() override {enabled.store(false);return true;}
    bool restore_cursor(CursorPoint p) override {restored_x.store(p.x);restored_y.store(p.y);restored.store(restore_any.load() || (p.x==10 && p.y==20));return restored.load();}
};
struct FakeClipboard : ClipboardBackend {
    std::uint32_t sequence=10;int writes=0;ClipboardPayload last{hop::windows::ClipboardFormat::utf8_text,{}};
    bool read(std::uint32_t,ClipboardPayload&) override {return false;}
    bool write(const ClipboardPayload& value,std::uint32_t expected,std::uint32_t& written) override {
        if(expected!=sequence)return false;last=value;++writes;written=++sequence;return true;
    }
};
struct ThreadClipboard : ClipboardBackend {
    std::mutex mutex;std::atomic<std::uint32_t> sequence{10};std::atomic<int> writes{0};
    std::atomic<bool> block_write{false},write_entered{false};
    ClipboardPayload value{hop::windows::ClipboardFormat::utf8_text,{'i','n','i','t'}};
    bool read(std::uint32_t expected,ClipboardPayload& out) override {std::lock_guard<std::mutex> lock(mutex);if(sequence.load()!=expected)return false;out=value;return true;}
    bool write(const ClipboardPayload& incoming,std::uint32_t expected,std::uint32_t& result) override {if(block_write.load()){write_entered.store(true);while(block_write.load())Sleep(1);}std::lock_guard<std::mutex> lock(mutex);if(sequence.load()!=expected)return false;value=incoming;result=++sequence;++writes;return true;}
    void set_text(const std::string& text){std::lock_guard<std::mutex> lock(mutex);value={hop::windows::ClipboardFormat::utf8_text,std::vector<std::uint8_t>(text.begin(),text.end())};++sequence;}
    void set_png(const std::vector<std::uint8_t>& bytes){std::lock_guard<std::mutex> lock(mutex);value={hop::windows::ClipboardFormat::png,bytes};++sequence;}
    bool has_png(const std::vector<std::uint8_t>& bytes){std::lock_guard<std::mutex> lock(mutex);return value.format==hop::windows::ClipboardFormat::png && value.bytes==bytes;}
};
struct Identity {
    std::array<char,8192> cert{}, key{};
    std::array<char,65> fingerprint{};
    Identity() { check(hop_tls_generate_identity(cert.data(),cert.size(),key.data(),key.size(),fingerprint.data(),fingerprint.size())==0,"generate isolated test identity"); }
    ~Identity() { SecureZeroMemory(key.data(),key.size()); }
};
struct SocketPair { SOCKET client = INVALID_SOCKET, server = INVALID_SOCKET; };
SocketPair pair() {
    SocketPair result;
    SOCKET listener=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    check(listener!=INVALID_SOCKET,"create loopback listener");
    sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK); addr.sin_port=0;
    check(bind(listener,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==0 && listen(listener,1)==0,"bind and listen loopback only");
    int length=sizeof(addr); check(getsockname(listener,reinterpret_cast<sockaddr*>(&addr),&length)==0,"query ephemeral port");
    result.client=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP); check(result.client!=INVALID_SOCKET,"create client");
    check(connect(result.client,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==0,"connect loopback");
    result.server=accept(listener,nullptr,nullptr); closesocket(listener);
    check(result.server!=INVALID_SOCKET,"accept loopback");
    return result;
}
TlsCredentials creds(const Identity& me,const Identity& peer) { return {me.cert.data(),me.key.data(),{peer.cert.data()}}; }
bool handshake(TlsSocket& client,TlsSocket& server,bool expected) {
    const auto deadline=GetTickCount64()+10000;
    while(GetTickCount64()<deadline) {
        const auto a=client.handshake(),b=server.handshake();
        if(a==TlsIoStatus::failed || b==TlsIoStatus::failed) return !expected;
        if(a==TlsIoStatus::ready && b==TlsIoStatus::ready) return expected;
        Sleep(1);
    }
    return false;
}
void transfer(TlsSocket& sender,TlsSocket& receiver,const char* message) {
    auto length=std::strlen(message); std::array<std::uint8_t,128> output{};
    const auto deadline=GetTickCount64()+5000; bool sent=false,received=false;
    while(GetTickCount64()<deadline && !received) {
        if(!sent){auto r=sender.write(reinterpret_cast<const std::uint8_t*>(message),length);
            check(r.status==TlsIoStatus::again || r.status==TlsIoStatus::ready,"write progress");
            if(r.status==TlsIoStatus::ready) {check(r.bytes==length,"write size");sent=true;}}
        auto r=receiver.read(output.data(),output.size());
        check(r.status==TlsIoStatus::again || r.status==TlsIoStatus::ready,"read progress");
        if(r.status==TlsIoStatus::ready) {check(r.bytes==length && std::memcmp(output.data(),message,length)==0,"exact payload");received=true;}
        Sleep(1);
    }
    check(sent && received,"round trip deadline");
}
}
int main() {
    WSADATA wsa{}; check(WSAStartup(MAKEWORD(2,2),&wsa)==0,"start Winsock for test");
    Identity a,b,bad;
    {
        MemoryCredentials fake; TrustStore store(fake); DeviceIdentity first,reloaded;
        check(store.load_or_create(first) && first.fingerprint.size()==64 && fake.writes==1,"create test-only device identity");
        check(store.load_or_create(reloaded) && first.certificate==reloaded.certificate && first.private_key==reloaded.private_key && fake.writes==1,"load same test identity");
        auto& stored=fake.records[L"Hop/v2/device-identity"]; const auto certificate_bytes=(std::size_t(stored[4])<<8)|stored[5]; stored[8+certificate_bytes]^=1;
        check(!store.load_or_create(reloaded) && fake.writes==1 && reloaded.private_key.empty(),"corrupt identity rejected without replacement");
        MemoryCredentials other; TrustStore other_store(other); DeviceIdentity other_identity;
        check(other_store.load_or_create(other_identity),"create second independent test identity");
        const auto& cert=first.certificate; const auto& key=other_identity.private_key;
        stored={'H','P','I','1',static_cast<std::uint8_t>(cert.size()>>8),static_cast<std::uint8_t>(cert.size()),static_cast<std::uint8_t>(key.size()>>8),static_cast<std::uint8_t>(key.size())};
        stored.insert(stored.end(),cert.begin(),cert.end()); stored.insert(stored.end(),key.begin(),key.end());
        check(!store.load_or_create(reloaded) && fake.writes==1 && reloaded.private_key.empty(),"mismatched valid certificate and key rejected");
        MemoryCredentials unavailable; unavailable.fail_read=true; TrustStore guarded(unavailable); DeviceIdentity none;
        check(!guarded.load_or_create(none) && unavailable.writes==0,"credential read error never creates identity");
        MemoryCredentials unwritable; unwritable.fail_write=true; TrustStore fail_store(unwritable);
        check(!fail_store.load_or_create(none) && none.private_key.empty(),"credential write failure hides generated key");
    }
    {
        TcpAddress address{}; check(numeric_tcp_address("127.0.0.1",0,address),"parse loopback address");
        check(!TcpListener::open(address,false),"listener requires explicit enablement");
        auto listener=TcpListener::open(address,true); check(listener && listener->local_address(address),"start isolated loopback listener");
        check(!TcpDialer::start(address,false,GetTickCount64()),"dialer requires explicit request");
        auto dialer=TcpDialer::start(address,true,GetTickCount64()); check(dialer!=nullptr,"start loopback dialer");
        SOCKET accepted=INVALID_SOCKET; auto ds=TcpProgress::pending,as=TcpProgress::pending; const auto deadline=GetTickCount64()+2000;
        while(GetTickCount64()<deadline && (ds==TcpProgress::pending || as==TcpProgress::pending)) {if(ds==TcpProgress::pending)ds=dialer->step(GetTickCount64());if(as==TcpProgress::pending)as=listener->accept(accepted);Sleep(1);}
        check(ds==TcpProgress::ready && as==TcpProgress::ready,"nonblocking connect and accept");
        auto c=TlsSocket::adopt(dialer->release(),true,creds(a,b));auto s=TlsSocket::adopt(accepted,false,creds(b,a));
        check(c && s && handshake(*c,*s,true),"accepted loopback streams support pinned TLS");
        check(!numeric_tcp_address("192.0.2.1.example",1,address) && !numeric_tcp_address(std::string("127.0.0.1\0bad",13),1,address),"reject nonnumeric and embedded-NUL addresses");
    }
    {
        DeviceIdentity own_a,own_b; own_a.certificate=a.cert.data();own_a.private_key=a.key.data();own_a.fingerprint=a.fingerprint.data();
        own_b.certificate=b.cert.data();own_b.private_key=b.key.data();own_b.fingerprint=b.fingerprint.data();
        MemoryCredentials credentials_a,credentials_b; TrustStore trust_a(credentials_a),trust_b(credentials_b);
        const std::wstring name_a=std::wstring(L"Hop/v2/peer/")+std::wstring(own_a.fingerprint.begin(),own_a.fingerprint.end());
        const std::wstring name_b=std::wstring(L"Hop/v2/peer/")+std::wstring(own_b.fingerprint.begin(),own_b.fingerprint.end());
        credentials_a.records[name_b]=std::vector<std::uint8_t>(own_b.certificate.begin(),own_b.certificate.end());
        credentials_b.records[name_a]=std::vector<std::uint8_t>(own_a.certificate.begin(),own_a.certificate.end());
        TcpAddress address{};check(numeric_tcp_address("127.0.0.1",0,address),"parse path address");
        auto listener=TcpListener::open(address,true);check(listener && listener->local_address(address),"create path listener");
        Session host(SessionRole::host,42,{},{{},{},[]{return true;}}), receiver(SessionRole::receiver,0,{7},{[](const hop::Frame&){return true;},[](const hop::Frame&){return true;},[]{return true;}});
        const auto now=GetTickCount64(); auto outbound=ConnectionPath::connect_trusted(address,own_b.fingerprint,own_a,trust_a,host,true,now);
        check(outbound!=nullptr,"explicit trusted path start"); SOCKET socket=INVALID_SOCKET; auto accept_state=TcpProgress::pending;const auto deadline=GetTickCount64()+3000;
        while(GetTickCount64()<deadline && accept_state==TcpProgress::pending){check(outbound->step(GetTickCount64()),"outbound path progress");accept_state=listener->accept(socket);Sleep(1);}
        check(accept_state==TcpProgress::ready,"path accepted");auto inbound=ConnectionPath::accept(socket,own_b,trust_b,nullptr,receiver,true,GetTickCount64());check(inbound!=nullptr,"accept path with local trust lookup");
        while(GetTickCount64()<deadline && (host.phase()!=SessionPhase::ready || receiver.phase()!=SessionPhase::ready)){check(outbound->step(GetTickCount64()) && inbound->step(GetTickCount64()),"path TLS progress");Sleep(1);}
        check(outbound->authenticated() && inbound->authenticated() && host.phase()==SessionPhase::ready && receiver.phase()==SessionPhase::ready,"HPSL trust lookup to authenticated v2 session");
        check(!ConnectionPath::connect_trusted(address,own_a.fingerprint,own_a,trust_a,host,true,GetTickCount64()),"unknown trusted peer cannot start path");
    }
    {
        DeviceIdentity own_a,own_b; own_a.certificate=a.cert.data();own_a.private_key=a.key.data();own_a.fingerprint=a.fingerprint.data();
        own_b.certificate=b.cert.data();own_b.private_key=b.key.data();own_b.fingerprint=b.fingerprint.data();
        const auto now=GetTickCount64(); hop::PairOffer offer_a,offer_b; offer_a.role=1;offer_a.name="Host";offer_a.certificate_pem=own_a.certificate;offer_a.nonce[0]=1;
        offer_b.role=2;offer_b.name="Peer";offer_b.certificate_pem=own_b.certificate;offer_b.nonce[0]=2;
        auto review_a=PairingReview::create(own_a.certificate,{offer_b,own_b.fingerprint,now,now+120000},now);
        auto review_b=PairingReview::create(own_b.certificate,{offer_a,own_a.fingerprint,now,now+120000},now);
        check(review_a && review_b,"create first-pair reviews");
        MemoryCredentials credentials_a,credentials_b; TrustStore trust_a(credentials_a),trust_b(credentials_b);
        TcpAddress address{};check(numeric_tcp_address("127.0.0.1",0,address),"parse candidate path address");auto listener=TcpListener::open(address,true);check(listener && listener->local_address(address),"create candidate path listener");
        Session host(SessionRole::host,42,{},{{},{},[]{return true;}}),receiver(SessionRole::receiver,0,{7},{[](const hop::Frame&){return true;},[](const hop::Frame&){return true;},[]{return true;}});
        check(!ConnectionPath::connect_candidate(address,*review_a,own_a,host,true,now),"unapproved candidate cannot connect");
        check(review_a->approve_after_independent_comparison(now) && review_b->approve_after_independent_comparison(now),"approve candidate fingerprints explicitly");
        auto outbound=ConnectionPath::connect_candidate(address,*review_a,own_a,host,true,now);check(outbound!=nullptr,"start approved first-pair path");
        SOCKET socket=INVALID_SOCKET;auto state=TcpProgress::pending;const auto deadline=GetTickCount64()+3000;
        while(GetTickCount64()<deadline && state==TcpProgress::pending){check(outbound->step(GetTickCount64()),"candidate outgoing progress");state=listener->accept(socket);Sleep(1);}
        check(state==TcpProgress::ready,"candidate path accepted");auto inbound=ConnectionPath::accept(socket,own_b,trust_b,&*review_b,receiver,true,GetTickCount64());check(inbound!=nullptr,"candidate selector accepts only approved pin");
        while(GetTickCount64()<deadline && (host.phase()!=SessionPhase::pair_wait || receiver.phase()!=SessionPhase::pair_wait)){check(outbound->step(GetTickCount64()) && inbound->step(GetTickCount64()),"candidate TLS progress");Sleep(1);}
        check(host.phase()==SessionPhase::pair_wait && receiver.phase()==SessionPhase::pair_wait && credentials_a.writes==0 && credentials_b.writes==0,"first pair gated and unpersisted");
        check(host.confirm_pairing() && receiver.confirm_pairing(),"both users confirm inside TLS");
        while(GetTickCount64()<deadline && (!host.pairing_confirmed() || !receiver.pairing_confirmed())){check(outbound->step(GetTickCount64()) && inbound->step(GetTickCount64()),"candidate confirmation progress");Sleep(1);}
        check(host.pairing_confirmed() && receiver.pairing_confirmed(),"mutual candidate confirmation complete");
        check(trust_a.remember(host,*outbound->transport(),own_b.certificate) && trust_b.remember(receiver,*inbound->transport(),own_a.certificate),"only confirmed pins stored in fake backend");
    }
    {
        TcpAddress address{};check(numeric_tcp_address("127.0.0.1",0,address),"parse server address");
        check(!LanServer::start(address,false),"server needs explicit enablement");auto server=LanServer::start(address,true);check(server && server->local_address(address),"create bounded loopback server");
        const auto start=GetTickCount64();check(server->open_pairing_window(start,2000),"explicit short pairing window");
        auto dialer=TcpDialer::start(address,true,start);check(dialer!=nullptr,"dial pairing server");std::unique_ptr<ClassifiedSocket> classified;auto state=TcpProgress::pending;
        const auto deadline=GetTickCount64()+1000;while(GetTickCount64()<deadline && state==TcpProgress::pending){state=dialer->step(GetTickCount64());check(server->poll(GetTickCount64(),classified),"server polling");Sleep(1);}
        check(state==TcpProgress::ready,"pairing server connected");SOCKET client=dialer->release();check(send(client,"HPOP",4,0)==4,"send candidate magic to server");
        while(GetTickCount64()<deadline && !classified){check(server->poll(GetTickCount64(),classified),"classify pairing server stream");Sleep(1);}
        check(classified && classified->kind()==ConnectionKind::candidate_offer,"window classifies candidate");
        SOCKET accepted=classified->release();char magic[4]{};check(recv(accepted,magic,4,0)==4 && std::memcmp(magic,"HPOP",4)==0,"server preserves candidate bytes");closesocket(accepted);closesocket(client);
        check(!server->open_pairing_window(GetTickCount64(),120001),"reject oversized pairing window");server->close_pairing_window();
    }
    {
        auto sockets=pair();const auto now=GetTickCount64();auto classifier=ConnectionClassifier::accept(sockets.server,true,now);check(classifier!=nullptr,"accept explicit pairing classifier");
        check(send(sockets.client,"HP",2,0)==2,"send partial protocol magic");check(classifier->step(GetTickCount64())==PairIo::pending,"partial protocol waits");
        check(send(sockets.client,"OP",2,0)==2,"complete candidate protocol magic");PairIo state=PairIo::pending;const auto deadline=GetTickCount64()+2000;
        while(GetTickCount64()<deadline && state==PairIo::pending){state=classifier->step(GetTickCount64());Sleep(1);}
        check(state==PairIo::ready && classifier->kind()==ConnectionKind::candidate_offer,"classify candidate only while enabled");
        SOCKET accepted=classifier->release();char bytes[4]{};check(recv(accepted,bytes,4,0)==4 && std::memcmp(bytes,"HPOP",4)==0,"classifier does not consume candidate bytes");closesocket(accepted);closesocket(sockets.client);
    }
    {
        auto sockets=pair();auto classifier=ConnectionClassifier::accept(sockets.server,false,GetTickCount64());check(classifier!=nullptr,"accept classifier with candidate disabled");
        check(send(sockets.client,"HPOP",4,0)==4,"send disallowed candidate");PairIo state=PairIo::pending;const auto deadline=GetTickCount64()+2000;
        while(GetTickCount64()<deadline && state==PairIo::pending){state=classifier->step(GetTickCount64());Sleep(1);}
        check(state==PairIo::failed,"candidate rejected when pairing window closed");closesocket(sockets.client);
    }
    {
        auto sockets=pair();auto classifier=ConnectionClassifier::accept(sockets.server,false,GetTickCount64());check(classifier!=nullptr,"accept normal selector classifier");
        check(send(sockets.client,"HPSL",4,0)==4,"send selector magic");PairIo state=PairIo::pending;const auto deadline=GetTickCount64()+2000;
        while(GetTickCount64()<deadline && state==PairIo::pending){state=classifier->step(GetTickCount64());Sleep(1);}
        check(state==PairIo::ready && classifier->kind()==ConnectionKind::trusted_selector,"classify known-peer lookup");
        SOCKET accepted=classifier->release();char bytes[4]{};check(recv(accepted,bytes,4,0)==4 && std::memcmp(bytes,"HPSL",4)==0,"classifier preserves selector bytes");closesocket(accepted);closesocket(sockets.client);
    }
    {
        // End-to-end product components, all on loopback and fake platform
        // backends: no real hook, injected OS input, or clipboard operation.
        DeviceIdentity own_a,own_b;own_a.certificate=a.cert.data();own_a.private_key=a.key.data();own_a.fingerprint=a.fingerprint.data();
        own_b.certificate=b.cert.data();own_b.private_key=b.key.data();own_b.fingerprint=b.fingerprint.data();
        MemoryCredentials ca,cb;TrustStore ta(ca),tb(cb);
        const std::wstring na=std::wstring(L"Hop/v2/peer/")+std::wstring(own_a.fingerprint.begin(),own_a.fingerprint.end());
        const std::wstring nb=std::wstring(L"Hop/v2/peer/")+std::wstring(own_b.fingerprint.begin(),own_b.fingerprint.end());
        ca.records[nb]=std::vector<std::uint8_t>(own_b.certificate.begin(),own_b.certificate.end());cb.records[na]=std::vector<std::uint8_t>(own_a.certificate.begin(),own_a.certificate.end());
        TcpAddress address{};check(numeric_tcp_address("127.0.0.1",0,address),"parse integrated test address");auto listener=TcpListener::open(address,true);check(listener && listener->local_address(address),"integrated loopback listener");
        std::vector<INPUT> injected;InputSink sink([&](UINT n,LPINPUT p,int){if(n!=1)return UINT(0);injected.push_back(*p);return UINT(1);});
        ReceiverSessionAdapter adapter(sink,{{7,{0,0,1920,1080}}},{0,0,1920,1080});check(adapter.valid(),"integrated fake receiver geometry");
        FakeCapture capture;HostCapturePump* host_pump=nullptr;
        Session host(SessionRole::host,42,{},{{},{},[&]{return !host_pump || host_pump->deactivate();}}), receiver(SessionRole::receiver,0,adapter.screen_ids(),adapter.callbacks());
        FakeClipboard clipboard;ClipboardActor actor(clipboard);const auto configured=actor.activate(own_a.fingerprint,43,10,0);check(configured && actor.work_once(),"prepare fake clipboard actor");
        int queued=0;auto sample=[] {return std::uint32_t(10);};
        auto fake_send=[](const hop::ClipboardValue&,std::uint32_t){return true;};
        auto enqueue=[&](const hop::ClipboardValue& value,std::uint32_t baseline){++queued;return actor.submit_remote(own_a.fingerprint,value.epoch,baseline,{hop::windows::ClipboardFormat::utf8_text,value.data});};
        check(host.enable_clipboard(sample,fake_send) && receiver.enable_clipboard(sample,enqueue),"enable fake clipboard session");
        const auto start=GetTickCount64();auto outbound=ConnectionPath::connect_trusted(address,own_b.fingerprint,own_a,ta,host,true,start);check(outbound!=nullptr,"integrated trusted path start");
        SOCKET accepted=INVALID_SOCKET;auto status=TcpProgress::pending;const auto deadline=GetTickCount64()+3000;
        while(GetTickCount64()<deadline && status==TcpProgress::pending){check(outbound->step(GetTickCount64()),"integrated dial");status=listener->accept(accepted);Sleep(1);}
        check(status==TcpProgress::ready,"integrated accepted connection");auto inbound=ConnectionPath::accept(accepted,own_b,tb,nullptr,receiver,true,GetTickCount64());check(inbound!=nullptr,"integrated receiver path");
        auto until=[&](auto predicate){while(GetTickCount64()<deadline && !predicate()){if(!outbound->step(GetTickCount64()) || !inbound->step(GetTickCount64()))return false;Sleep(1);}return predicate();};
        check(until([&]{return host.phase()==SessionPhase::ready && receiver.phase()==SessionPhase::ready;}),"integrated handshake");
        check(receiver.advertise_screens(adapter.screen_payload()),"integrated monitor announcement");bool entered=false;
        check(until([&]{if(!entered)entered=host.enter(43,7,0,0);return entered;}),"integrated focus request");
        check(until([&]{return host.phase()==SessionPhase::active;}),"integrated focus ACK");
        HostCapturePump pump(host,capture);host_pump=&pump;check(pump.begin(7,1920,1080,0,0,{},GetTickCount64()),"start fake capture after ACK");
        capture.push({CaptureKind::move,0,false,false,510,505});capture.push({CaptureKind::key,0x1e,true});capture.push({CaptureKind::key,0x1e,false});
        capture.push({CaptureKind::button,3,true});capture.push({CaptureKind::button,3,false});CaptureEvent wheel{CaptureKind::wheel};wheel.y=120;capture.push(wheel);
        check(pump.step(GetTickCount64()),"queue fake capture events");check(until([&]{return injected.size()>=9;}),"encrypted input reaches fake sink");
        check(sink.held_keys()==0 && sink.held_buttons()==0,"fake receiver releases balanced input");
        check(host.request_clipboard_ready() && receiver.request_clipboard_ready(),"integrated clipboard readiness");
        check(until([&]{return host.clipboard_can_sync() && receiver.clipboard_can_sync();}),"integrated clipboard readiness exchange");
        const std::uint8_t text[]={'h','i'};check(host.clipboard_start(1,hop::ClipboardFormat::text,2) && host.clipboard_chunk(1,text,2),"queue integrated fake clipboard");
        check(until([&]{return queued==1;}) && actor.work_once() && clipboard.writes==1 && clipboard.last.bytes==std::vector<std::uint8_t>(text,text+2),"fake clipboard delivered through bounded actor");
        capture.push({CaptureKind::key,0x1e,true});check(pump.step(GetTickCount64()),"queue held fake key");check(until([&]{return sink.held_keys()==1;}),"receiver has held fake key");
        outbound.reset();check(capture.restored && !capture.active(),"disconnect restores fake local capture");
        bool disconnected=false;while(GetTickCount64()<deadline && !disconnected){disconnected=!inbound->step(GetTickCount64());Sleep(1);}
        check(disconnected && sink.held_keys()==0,"disconnect releases fake remote key");check(pump.drain() && !capture.running,"fake capture drained");host_pump=nullptr;
        auto revoke=actor.revoke(GetTickCount64());check(revoke && actor.work_once() && actor.acknowledged(revoke),"fake clipboard revoked");
    }
    {
        DeviceIdentity own_a,own_b;own_a.certificate=a.cert.data();own_a.private_key=a.key.data();own_a.fingerprint=a.fingerprint.data();
        own_b.certificate=b.cert.data();own_b.private_key=b.key.data();own_b.fingerprint=b.fingerprint.data();
        MemoryCredentials ca,cb;TrustStore ta(ca),tb(cb);
        auto host_callbacks=[] {return SessionCallbacks{{},{},[]{return true;}};};
        auto receiver_callbacks=[] {return SessionCallbacks{[](const hop::Frame&){return true;},[](const hop::Frame&){return true;},[]{return true;}};};
        PairingWorker host(own_a,ta,SessionRole::host,"Host",{},host_callbacks),receiver(own_b,tb,SessionRole::receiver,"Receiver",{7},receiver_callbacks);
        TcpAddress address{};check(numeric_tcp_address("127.0.0.1",0,address),"parse worker loopback address");const auto now=GetTickCount64();
        check(receiver.begin_listening(address,now) && receiver.local_address(address) && host.begin_connecting(address,now),"explicit pairing workers start");
        const auto deadline=GetTickCount64()+5000;
        while(GetTickCount64()<deadline && (host.state()!=PairingState::reviewing || receiver.state()!=PairingState::reviewing)) {check(host.step(GetTickCount64()) && receiver.step(GetTickCount64()),"candidate worker progress");Sleep(1);}
        check(host.state()==PairingState::reviewing && receiver.state()==PairingState::reviewing && !ca.writes && !cb.writes,"candidate requires UI review");
        auto host_review=host.review(),receiver_review=receiver.review();check(host_review && receiver_review && host_review->approve_after_independent_comparison(GetTickCount64()) && receiver_review->approve_after_independent_comparison(GetTickCount64()),"both users explicitly compare fingerprints");
        // The dialer can send HPSL before the listener's next worker tick.
        while(GetTickCount64()<deadline && !host.transport()){check(host.step(GetTickCount64()),"dialer queues approved selector");Sleep(1);}
        check(host.transport() && receiver.step(GetTickCount64()) && receiver.session(),
              "listener applies approval before polling the queued selector");
        while(GetTickCount64()<deadline && (host.state()!=PairingState::confirming || receiver.state()!=PairingState::confirming)) {check(host.step(GetTickCount64()) && receiver.step(GetTickCount64()),"pairing worker TLS progress");Sleep(1);}
        check(host.state()==PairingState::confirming && receiver.state()==PairingState::confirming && !ca.writes && !cb.writes,"TLS requires second explicit confirmation");
        check(host.confirm_authenticated_pair(true) && receiver.confirm_authenticated_pair(true),"both users explicitly confirm TLS");
        while(GetTickCount64()<deadline && (host.state()!=PairingState::connected || receiver.state()!=PairingState::connected)) {check(host.step(GetTickCount64()) && receiver.step(GetTickCount64()),"confirmed pairing worker progress");Sleep(1);}
        check(host.state()==PairingState::connected && receiver.state()==PairingState::connected && ca.writes==1 && cb.writes==1,"workers persist only confirmed pins");
        check(host.step(GetTickCount64()) && receiver.step(GetTickCount64()) && ca.writes==1 && cb.writes==1,"connected workers do not rewrite trust repeatedly");
        PairingWorker listener(own_b,tb,SessionRole::receiver,"Receiver",{7},receiver_callbacks);
        check(numeric_tcp_address("127.0.0.1",0,address) && listener.begin_listening(address,GetTickCount64()) &&
              listener.local_address(address),"open explicit listener for saved peer");
        auto unknown=TcpDialer::start(address,true,GetTickCount64());
        check(unknown!=nullptr,"connect unknown selector");
        auto unknown_progress=unknown->step(GetTickCount64());
        while(GetTickCount64()<deadline && unknown_progress==TcpProgress::pending){Sleep(1);unknown_progress=unknown->step(GetTickCount64());}
        check(unknown_progress==TcpProgress::ready,"unknown selector TCP connection");
        auto unknown_selector=PeerSelector::sender(unknown->release(),own_b.fingerprint,GetTickCount64());
        check(unknown_selector!=nullptr,"prepare unknown selector");
        auto selector_progress=unknown_selector->step(GetTickCount64());
        while(GetTickCount64()<deadline && selector_progress==PairIo::pending){Sleep(1);selector_progress=unknown_selector->step(GetTickCount64());}
        check(selector_progress==PairIo::ready && listener.step(GetTickCount64()) && listener.state()==PairingState::waiting_candidate &&
              !listener.session() && cb.writes==1,"unknown pin cannot authenticate or close pairing window");
        Session reconnect(SessionRole::host,71,{},host_callbacks());
        auto saved=ConnectionPath::connect_trusted(address,own_b.fingerprint,own_a,ta,reconnect,true,GetTickCount64());
        check(saved!=nullptr,"dial saved peer without HPOP");
        while(GetTickCount64()<deadline && (listener.state()!=PairingState::connected || reconnect.phase()!=SessionPhase::ready)){
            check(saved->step(GetTickCount64()) && listener.step(GetTickCount64()),"saved peer reconnection progress");Sleep(1);
        }
        check(saved->authenticated() && listener.state()==PairingState::connected && listener.session() &&
              listener.session()->peer_fingerprint()==own_a.fingerprint && !listener.review() &&
              !listener.session()->pairing_confirmed() && cb.writes==1,"reconnect authenticates existing pin with no new trust write");
    }
    {
        MemoryCredentials a_backend,b_backend;std::atomic<int> denied{0};
        PairingRuntime host(a_backend,[&](PairingRuntimeEvent event){if(event==PairingRuntimeEvent::action_failed)++denied;}),receiver(b_backend,{});
        TcpAddress address{};check(numeric_tcp_address("127.0.0.1",0,address),"parse threaded pairing address");
        FakeCapture capture;std::atomic<int> fake_injections{0};
        InputSink fake_sink([&](UINT n,LPINPUT,int){if(n!=1)return UINT(0);++fake_injections;return UINT(1);});
        ReceiverSessionAdapter fake_receiver(fake_sink,{{7,{0,0,1920,1080}}},{0,0,1920,1080});
        PairingRuntimeConfig rc;rc.listen=true;rc.address=address;rc.role=SessionRole::receiver;rc.name="Receiver";rc.screens={7};
        rc.screen_payload={1,3,0,0,0,1,0,0,0,0,0,0,0,0,0,7,0,0,0,0,0,0,0,0,0,0,7,128,0,0,4,56};
        rc.callbacks=[&] {return fake_receiver.callbacks();};
        check(receiver.start(std::move(rc)),"start explicit receiver runtime");const auto deadline=GetTickCount64()+8000;
        while(GetTickCount64()<deadline && !receiver.listen_address(address))Sleep(1);
        check(receiver.listen_address(address),"receiver runtime provides listen address");
        PairingRuntimeConfig hc;hc.address=address;hc.role=SessionRole::host;hc.name="Host";hc.callbacks=[] {return SessionCallbacks{{},{},[]{return true;}};};hc.host_capture=&capture;
        check(host.start(std::move(hc)),"start explicit host runtime");
        while(GetTickCount64()<deadline && (host.state()!=PairingState::reviewing || receiver.state()!=PairingState::reviewing))Sleep(1);
        auto hr=host.review(),rr=receiver.review();check(hr && rr && host.state()==PairingState::reviewing && receiver.state()==PairingState::reviewing,"threaded runtime awaits explicit fingerprint review");
        check(hr->approve_after_independent_comparison(GetTickCount64()) && rr->approve_after_independent_comparison(GetTickCount64()),"approve fake fingerprints from caller");
        while(GetTickCount64()<deadline && (host.state()!=PairingState::confirming || receiver.state()!=PairingState::confirming))Sleep(1);
        check(host.state()==PairingState::confirming && receiver.state()==PairingState::confirming,"threaded runtime awaits TLS confirmation");
        check(host.confirm_tls(true) && receiver.confirm_tls(true),"marshal confirmation to both workers");
        while(GetTickCount64()<deadline && (host.state()!=PairingState::connected || receiver.state()!=PairingState::connected))Sleep(1);
        check(host.state()==PairingState::connected && receiver.state()==PairingState::connected,"threaded explicit pairing completes");
        while(GetTickCount64()<deadline && host.peer_displays().empty())Sleep(1);
        const auto displays=host.peer_displays();check(displays.size()==1 && displays[0].id==7 && displays[0].width==1920,"threaded runtime exposes verified peer display geometry");
        capture.held.store(true);check(host.request_control(host.peer_fingerprint(),7),"request focus while fake physical key held");
        while(GetTickCount64()<deadline && !denied.load())Sleep(1);
        check(denied.load()==1 && !capture.active() && fake_injections.load()==0,"held physical input prevents ENTER and injection");
        capture.held.store(false);check(host.request_control(host.peer_fingerprint(),7),"explicit host control request");
        while(GetTickCount64()<deadline && !capture.active())Sleep(1);
        check(capture.active(),"fake hook starts only after focus ACK");
        capture.push({CaptureKind::move,0,false,false,510,505});capture.push({CaptureKind::key,0x1e,true});capture.push({CaptureKind::key,0x1e,false});
        while(GetTickCount64()<deadline && fake_injections.load()<4)Sleep(1);
        check(fake_injections.load()>=4,"threaded fake capture reaches fake receiver");
        check(host.request_pause(),"explicit host pause request");while(GetTickCount64()<deadline && capture.active())Sleep(1);
        check(!capture.active() && capture.restored.load(),"pause restores fake cursor and capture");
        host.stop();receiver.stop();check(a_backend.writes==2 && b_backend.writes==2,"only fake identity and confirmed peer saved");
    }
    {
        // One Windows host owner with two independently pinned TLS peers.
        // Both peers advertise screen 7; the fingerprint must scope it.
        MemoryCredentials host_backend,first_backend,second_backend;
        FakeCapture capture;capture.restore_any.store(true);
        std::atomic<int> first_entries{0},second_entries{0},blocked_switches{0},right_edge_moves{0},selection_events{0};
        auto host_board=std::make_shared<ThreadClipboard>();
        PairingRuntime host(host_backend,[&](PairingRuntimeEvent event) {
            if(event==PairingRuntimeEvent::peer_action_failed)++blocked_switches;
            if(event==PairingRuntimeEvent::connected)++selection_events;
        });
        // The fake old receiver uses a manually pumped session. The Windows
        // clipboard worker deliberately has one process-wide owner.
        DeviceIdentity first_identity;first_identity.certificate=b.cert.data();
        first_identity.private_key=b.key.data();first_identity.fingerprint=b.fingerprint.data();
        TrustStore first_trust(first_backend);
        bool first_received_initial=false;
        PairingWorker first(first_identity,first_trust,SessionRole::receiver,"First receiver",{7},[&] {
            return SessionCallbacks{[&](const hop::Frame&){++first_entries;return true;},
                                    [&](const hop::Frame& frame){
                                        if(frame.payload.size()==16 && frame.payload[1]==1 && frame.payload[12]==255 && frame.payload[13]==255)
                                            ++right_edge_moves;
                                        return true;
                                    },[]{return true;}};
        },[&](Session& session) {
            return session.enable_clipboard([]{return std::uint32_t(20);},
                [&](const hop::ClipboardValue& value,std::uint32_t baseline){
                    first_received_initial=baseline==20 && value.data==std::vector<std::uint8_t>({'i','n','i','t'});
                    return true;
                });
        });
        PairingRuntime second(second_backend,{});
        TcpAddress listen{};check(numeric_tcp_address("127.0.0.1",0,listen),"multi-peer host loopback");
        PairingRuntimeConfig host_config;
        host_config.listen=true;host_config.address=listen;host_config.role=SessionRole::host;host_config.name="Host";
        host_config.host_capture=&capture;host_config.callbacks=[] {return SessionCallbacks{{},{},[]{return true;}};};
        host_config.clipboard_backend=host_board;host_config.clipboard_sequence=[host_board]{return host_board->sequence.load();};
        check(host.start(std::move(host_config)),"start one multi-peer host owner");
        const auto deadline=GetTickCount64()+15000;
        bool pump_first=false,first_announced=false,first_clip_ready=false;
        auto wait=[&](auto predicate) {
            while(GetTickCount64()<deadline && !predicate()) {
                if(pump_first) {
                    check(first.step(GetTickCount64()),"pump first receiver");
                    if(first.state()==PairingState::connected) {
                        auto* session=first.session();
                        if(!first_announced) {
                            check(session->advertise_screens({1,3,0,0,0,1,0,0,0,0,0,0,0,0,0,7,0,0,0,0,0,0,0,0,0,0,7,128,0,0,4,56}),
                                  "advertise first receiver screen");
                            first_announced=true;
                        }
                        if(session->phase()==SessionPhase::active) {
                            if(!first_clip_ready){check(session->request_clipboard_ready(),"first receiver clipboard ready");first_clip_ready=true;}
                            if(session->clipboard_initial_done() && !session->clipboard_initial_drained())
                                check(session->mark_clipboard_initial_drained(),"drain first initial clipboard snapshot");
                        } else first_clip_ready=false;
                    }
                }
                Sleep(1);
            }
            return predicate();
        };
        TcpAddress address{};
        check(wait([&]{return host.listen_address(address);}),"read host listener address");
        auto approve_pair=[&](auto& receiver,std::uint16_t width,auto confirm_receiver) {
            const auto reviewing=wait([&]{return host.state()==PairingState::reviewing && receiver.state()==PairingState::reviewing;});
            if(!reviewing)std::cerr<<"multi-peer screen width="<<width<<" host state="<<static_cast<int>(host.state())<<
                " receiver state="<<static_cast<int>(receiver.state())<<" connected peers="<<host.connected_peers().size()<<'\n';
            check(reviewing,"both peers await independent comparison");
            auto hr=host.review(),rr=receiver.review();
            check(hr && rr && hr->remote_fingerprint()==rr->local_fingerprint() &&
                  hr->approve_after_independent_comparison(GetTickCount64()) &&
                  rr->approve_after_independent_comparison(GetTickCount64()),"independently approve the exact new peer");
            check(wait([&]{return host.state()==PairingState::confirming && receiver.state()==PairingState::confirming;}),
                  "new TLS peer awaits explicit confirmation");
            check(host.confirm_tls(true) && confirm_receiver(),"confirm only pending TLS link");
            const auto pin=hr->remote_fingerprint();
            check(wait([&]{const auto current=host.selected_displays();
                return current.peer==pin && current.displays.size()==1 && current.displays[0].width==width;
            }),"publish selected peer and its own screen geometry");
            return pin;
        };
        check(first.begin_connecting(address,GetTickCount64()),"first receiver dials host");
        pump_first=true;
        const auto first_pin=approve_pair(first,1920,[&]{return first.confirm_authenticated_pair(true);});
        check(host.request_add_peer(true,address),"reopen pairing on the existing bound port");
        check(wait([&]{return host.state()==PairingState::waiting_candidate;}),"reopened listener waits for another peer");
        PairingRuntimeConfig second_config;second_config.address=address;second_config.role=SessionRole::receiver;
        second_config.name="Second receiver";second_config.screens={7};
        second_config.screen_payload={1,3,0,0,0,1,0,0,0,0,0,0,0,0,0,7,0,0,0,0,0,0,0,0,0,0,5,0,0,0,4,56};
        second_config.callbacks=[&] {return SessionCallbacks{
            [&](const hop::Frame&){++second_entries;return true;},[](const hop::Frame&){return true;},[]{return true;}};};
        check(second.start(std::move(second_config)),"second receiver dials the same host listener");
        const auto second_pin=approve_pair(second,1280,[&]{return second.confirm_tls(true);});
        check(first_pin!=second_pin && host.connected_peers().size()==2,"retain both independently authenticated links");
        check(!host.request_control(first_pin,7),"reject stale peer pin even when screen ID matches");
        check(host.request_select_peer(first_pin) && wait([&]{return host.peer_fingerprint()==first_pin;}),
              "select first idle peer");
        check(host.request_control(first_pin,7) && wait([&]{return first_entries.load()==1 && capture.active();}),
              "capture only after first peer acknowledged focus");
        check(host.request_select_peer(second_pin) && wait([&]{return blocked_switches.load()==1;}),
              "reject target switch while first peer owns focus");
        check(first_entries.load()==1 && second_entries.load()==0 && capture.active(),"retain exactly one focus");
        check(host.request_pause() && wait([&]{return !capture.running.load() && first.session()->phase()==SessionPhase::ready;}),
              "restore and release first manual focus");
        check(wait([&]{
            if(host.peer_fingerprint()==second_pin)return true;
            host.request_select_peer(second_pin);
            Sleep(10);
            return false;
        }),"manually select second idle peer");
        first_received_initial=false;
        const std::vector<LivePeerScreens> world={{first_pin,{{7,0,0,1920,1080}}},{second_pin,{{7,0,0,1280,1080}}}};
        const std::vector<LiveScreenPlacement> places={{"",1,0,0},{first_pin,7,100,0},{second_pin,7,2020,0}};
        check(host.request_workspace_edges(second_pin,{{1,0,0,100,100}},world,places) &&
              wait([&]{return capture.running.load() && !capture.active();}),"arm edges for the complete workspace");
        capture.push({CaptureKind::move,0,false,false,98,50});
        capture.push({CaptureKind::move,0,false,false,99,50});
        check(wait([&]{return first_entries.load()==2 && capture.active() && host.peer_fingerprint()==first_pin;}),
              "local edge chooses first peer from geometry while the second was selected");
        check(wait([&]{return first_received_initial && first.session()->clipboard_initial_drained();}),
              "old peer receives ordered initial clipboard");
        host_board->block_write.store(true);
        const std::uint8_t text[]={'o','l','d'};
        check(first.session()->clipboard_start(1,hop::ClipboardFormat::text,sizeof(text)) &&
              first.session()->clipboard_chunk(1,text,sizeof(text)),"old receiver sends clipboard change");
        check(wait([&]{return host_board->write_entered.load();}),"old clipboard write is in flight");
        capture.push({CaptureKind::move,0,false,false,4000,500});
        check(wait([&]{return right_edge_moves.load()>0;}),"move to the right edge of the first peer");
        capture.push({CaptureKind::move,0,false,false,501,500});
        check(wait([&]{return !capture.running.load() && first.session()->phase()==SessionPhase::ready;}),
              "peer-to-peer edge restores capture and receives old release while clipboard drains");
        check(host.peer_fingerprint()==first_pin && second_entries.load()==0,
              "pending peer-to-peer handoff cannot enter before clipboard revocation");
        host_board->block_write.store(false);
        check(wait([&]{return host.peer_fingerprint()==second_pin && second_entries.load()==1 && capture.active();}),
              "automatic handoff enters second only after release acknowledgement and clipboard revocation");
        check(host.request_pause() && wait([&]{return !capture.running.load();}),"disable automatic workspace edges");
        const auto last_selection=selection_events.load();
        check(wait([&]{
            if(selection_events.load()>last_selection)return true;
            host.request_select_peer(second_pin);
            Sleep(10);
            return false;
        }),"second link becomes eligible for local selection after release");
        check(host.request_control(second_pin,7) &&
              wait([&]{return second_entries.load()==2 && capture.active();}),"start explicit second focus after pausing edges");
        first.cancel();pump_first=false;
        check(wait([&]{return host.connected_peers().size()==1;}) && capture.active() && host.peer_fingerprint()==second_pin,
              "losing a standby peer does not revoke the selected capture");
        check(host.request_pause() && wait([&]{return !capture.running.load();}),"release the second focus");
        host.stop();second.stop();
    }
    {
        // Real host runtime, fake receiver and fake local clipboard. A copy
        // made before entering must be sent once as the initial snapshot.
        auto clipboard=std::make_shared<ThreadClipboard>();MemoryCredentials host_backend,receiver_backend;
        DeviceIdentity own;own.certificate=b.cert.data();own.private_key=b.key.data();own.fingerprint=b.fingerprint.data();TrustStore trust(receiver_backend);
        std::uint32_t target_sequence=20,applied_baseline=0;std::vector<std::uint8_t> applied;
        auto callbacks=[] {return SessionCallbacks{[](const hop::Frame&){return true;},[](const hop::Frame&){return true;},[]{return true;}};};
        auto configure=[&](Session& session){return session.enable_clipboard([&]{return target_sequence;},[&](const hop::ClipboardValue& value,std::uint32_t baseline){applied=value.data;applied_baseline=baseline;return true;});};
        PairingWorker receiver(own,trust,SessionRole::receiver,"Receiver",{7},callbacks,configure);
        TcpAddress address{};check(numeric_tcp_address("127.0.0.1",0,address) && receiver.begin_listening(address,GetTickCount64()) && receiver.local_address(address),"host snapshot fake receiver listens");
        FakeCapture capture;capture.restore_any.store(true);std::atomic<int> stale_rejected{0},armed_count{0};PairingRuntime host(host_backend,[&](PairingRuntimeEvent event){if(event==PairingRuntimeEvent::action_failed)++stale_rejected;if(event==PairingRuntimeEvent::edge_monitor_started)++armed_count;});PairingRuntimeConfig hc;hc.address=address;hc.role=SessionRole::host;hc.name="Host";
        hc.callbacks=[] {return SessionCallbacks{{},{},[]{return true;}};};hc.host_capture=&capture;hc.clipboard_backend=clipboard;hc.clipboard_sequence=[clipboard]{return clipboard->sequence.load();};
        std::atomic<bool> local_geometry_current{false};
        hc.verify_local_geometry=[&](const std::vector<LiveScreen>& local) {
            return local_geometry_current.load() && local.size()==1 && local[0].id==1;
        };
        check(host.start(std::move(hc)),"start host snapshot runtime");const auto deadline=GetTickCount64()+8000;auto tick=[&]{return receiver.step(GetTickCount64());};
        while(GetTickCount64()<deadline && (host.state()!=PairingState::reviewing || receiver.state()!=PairingState::reviewing)){check(tick(),"host snapshot candidate progress");Sleep(1);}
        auto hr=host.review(),rr=receiver.review();check(hr && rr && hr->approve_after_independent_comparison(GetTickCount64()) && rr->approve_after_independent_comparison(GetTickCount64()),"host snapshot candidates approved");
        while(GetTickCount64()<deadline && (host.state()!=PairingState::confirming || receiver.state()!=PairingState::confirming)){check(tick(),"host snapshot TLS progress");Sleep(1);}
        check(host.confirm_tls(true) && receiver.confirm_authenticated_pair(true),"host snapshot TLS confirmed");
        while(GetTickCount64()<deadline && (host.state()!=PairingState::connected || receiver.state()!=PairingState::connected)){check(tick(),"host snapshot session progress");Sleep(1);}
        check(receiver.session() && receiver.session()->advertise_screens({1,3,0,0,0,1,0,0,0,0,0,0,0,0,0,7,0,0,0,0,0,0,0,0,0,0,7,128,0,0,4,56}),"host snapshot screen announcement");
        while(GetTickCount64()<deadline && host.peer_displays().empty()){check(tick(),"host snapshot screen progress");Sleep(1);}
        check(host.request_edge_monitor(host.peer_fingerprint(),{{1,0,0,100,100}},{{7,0,0,1919,1080}},{{false,1,0,0},{true,7,100,0}}),"submit stale edge geometry");
        while(GetTickCount64()<deadline && !stale_rejected.load()){check(tick(),"stale edge check");Sleep(1);}
        check(stale_rejected.load()==1 && !capture.running.load(),"stale peer geometry never starts hook");
        check(host.request_edge_monitor(host.peer_fingerprint(),{{1,0,0,100,100}},{{7,0,0,1920,1080}},{{false,1,0,0},{true,7,100,0}}),"submit stale local geometry");
        while(GetTickCount64()<deadline && stale_rejected.load()<2){check(tick(),"stale local check");Sleep(1);}
        check(stale_rejected.load()==2 && !capture.running.load(),"changed local geometry never starts hook");
        local_geometry_current.store(true);
        check(host.request_edge_monitor(host.peer_fingerprint(),{{1,0,0,100,100}},{{7,0,0,1920,1080}},{{false,1,0,0},{true,7,100,0}}),"explicitly arm fake edge monitor");
        while(GetTickCount64()<deadline && !capture.running.load()){check(tick(),"fake passive hook progress");Sleep(1);}
        check(capture.running.load() && !capture.active(),"fake passive hook does not suppress input");
        capture.push({CaptureKind::move,0,false,false,98,50});capture.push({CaptureKind::move,0,false,false,99,50});
        while(GetTickCount64()<deadline && receiver.session()->phase()!=SessionPhase::active){check(tick(),"host snapshot enter progress");Sleep(1);}
        check(receiver.session()->phase()==SessionPhase::active,"fake edge crossing enters target");target_sequence=21;
        check(receiver.session()->request_clipboard_ready(),"host snapshot receiver ready");
        while(GetTickCount64()<deadline && !receiver.session()->clipboard_initial_done()){check(tick(),"host snapshot transfer progress");Sleep(1);}
        check(receiver.session()->clipboard_initial_done() && applied==std::vector<std::uint8_t>({'i','n','i','t'}) && applied_baseline==20,"host sends preexisting snapshot with ENTER baseline and ordered marker");
        while(GetTickCount64()<deadline && !capture.active()){check(tick(),"edge capture activation");Sleep(1);}
        check(capture.active(),"fake edge capture activated after ACK");
        capture.held.store(true);capture.push({CaptureKind::move,0,false,false,499,500});
        for(int i=0;i<30;++i){check(tick(),"held return edge progress");Sleep(1);}
        check(capture.active() && receiver.session()->phase()==SessionPhase::active,"held key prevents return crossing");
        capture.held.store(false);capture.push({CaptureKind::move,0,false,false,499,500});
        while(GetTickCount64()<deadline && receiver.session()->phase()!=SessionPhase::ready){check(tick(),"return edge progress");Sleep(1);}
        check(receiver.session()->phase()==SessionPhase::ready && capture.restored.load() && capture.restored_x.load()==98,"return edge restores local cursor and releases remote focus");
        while(GetTickCount64()<deadline && armed_count.load()<2){check(tick(),"edge rearm after release barriers");Sleep(1);}
        check(armed_count.load()==2 && capture.running.load() && !capture.active(),"edge monitor rearms after local and remote release");
        capture.push({CaptureKind::move,0,false,false,98,50});capture.push({CaptureKind::move,0,false,false,99,50});
        while(GetTickCount64()<deadline && receiver.session()->phase()!=SessionPhase::active){check(tick(),"second edge crossing progress");Sleep(1);}
        check(receiver.session()->phase()==SessionPhase::active,"automatic monitor handles a second crossing");
        while(GetTickCount64()<deadline && !capture.active()){check(tick(),"second edge capture activation");Sleep(1);}
        check(capture.active(),"second edge capture active");
        local_geometry_current.store(false);
        capture.push({CaptureKind::move,0,false,false,499,500});
        while(GetTickCount64()<deadline && stale_rejected.load()<3){check(tick(),"reject stale rearm geometry");Sleep(1);}
        check(stale_rejected.load()==3 && armed_count.load()==2 && !capture.running.load(),"changed local geometry prevents automatic rearm");
        host.stop();receiver.cancel();
    }
    {
        // One real runtime clipboard actor and a fake remote session. The
        // backend is entirely in memory; no Windows clipboard is opened.
        auto clipboard=std::make_shared<ThreadClipboard>();MemoryCredentials receiver_backend,host_backend;std::atomic<int> restores{0};
        PairingRuntime receiver(receiver_backend,{});TcpAddress address{};check(numeric_tcp_address("127.0.0.1",0,address),"clipboard runtime address");
        PairingRuntimeConfig rc;rc.listen=true;rc.address=address;rc.role=SessionRole::receiver;rc.name="Receiver";rc.screens={7};
        rc.screen_payload={1,3,0,0,0,1,0,0,0,0,0,0,0,0,0,7,0,0,0,0,0,0,0,0,0,0,7,128,0,0,4,56};
        rc.callbacks=[&] {return SessionCallbacks{[](const hop::Frame&){return true;},[](const hop::Frame&){return true;},[&]{++restores;return true;}};};
        rc.clipboard_backend=clipboard;rc.clipboard_sequence=[clipboard]{return clipboard->sequence.load();};
        check(receiver.start(std::move(rc)),"start receiver clipboard runtime");const auto deadline=GetTickCount64()+8000;
        while(GetTickCount64()<deadline && !receiver.listen_address(address))Sleep(1);check(receiver.listen_address(address),"clipboard runtime listening");
        DeviceIdentity own;own.certificate=a.cert.data();own.private_key=a.key.data();own.fingerprint=a.fingerprint.data();TrustStore trust(host_backend);
        int received=0;std::vector<std::uint8_t> got;
        auto callbacks=[] {return SessionCallbacks{{},{},[]{return true;}};};
        auto configure=[&](Session& session){return session.enable_clipboard([]{return std::uint32_t(5);},[&](const hop::ClipboardValue& value,std::uint32_t baseline){if(baseline!=5)return false;++received;got=value.data;return true;});};
        PairingWorker host(own,trust,SessionRole::host,"Host",{},callbacks,configure);check(host.begin_connecting(address,GetTickCount64()),"start fake remote clipboard peer");
        auto tick=[&]{return host.step(GetTickCount64());};
        while(GetTickCount64()<deadline && (host.state()!=PairingState::reviewing || receiver.state()!=PairingState::reviewing)){check(tick(),"clipboard candidate progress");Sleep(1);}
        auto hr=host.review(),rr=receiver.review();check(hr && rr && hr->approve_after_independent_comparison(GetTickCount64()) && rr->approve_after_independent_comparison(GetTickCount64()),"clipboard candidate approved");
        while(GetTickCount64()<deadline && (host.state()!=PairingState::confirming || receiver.state()!=PairingState::confirming)){check(tick(),"clipboard TLS progress");Sleep(1);}
        check(host.confirm_authenticated_pair(true) && receiver.confirm_tls(true),"clipboard pair confirmed");
        while(GetTickCount64()<deadline && (host.state()!=PairingState::connected || receiver.state()!=PairingState::connected || host.peer_displays().empty())){check(tick(),"clipboard session progress");Sleep(1);}
        check(host.state()==PairingState::connected && !host.peer_displays().empty() && host.request_focus(7,0,0),"enter clipboard target");
        while(GetTickCount64()<deadline && host.session()->phase()!=SessionPhase::active){check(tick(),"clipboard focus progress");Sleep(1);}
        check(host.session()->phase()==SessionPhase::active && host.session()->request_clipboard_ready(),"host clipboard actor ready");
        while(GetTickCount64()<deadline && !host.session()->clipboard_can_sync()){check(tick(),"clipboard ready progress");Sleep(1);}
        check(host.session()->clipboard_can_sync(),"both clipboard actors ready");
        const std::uint8_t message[]={'h','i'};check(host.session()->clipboard_start(1,hop::ClipboardFormat::text,2) && host.session()->clipboard_chunk(1,message,2) && host.session()->send_clipboard_initial_done(),"send fake remote text and initial marker");
        while(GetTickCount64()<deadline && clipboard->writes.load()==0){check(tick(),"clipboard inbound progress");Sleep(1);}
        check(clipboard->writes.load()==1,"runtime applies remote text to fake backend");
        clipboard->set_text("/tmp/example");
        while(GetTickCount64()<deadline && received==0){check(tick(),"clipboard outbound progress");Sleep(1);}
        check(received==1 && std::string(got.begin(),got.end())=="/tmp/example","runtime sends ordinary path text");
        clipboard->block_write.store(true);const std::uint8_t next[]={'o','k'};
        check(host.session()->clipboard_start(2,hop::ClipboardFormat::text,2) && host.session()->clipboard_chunk(2,next,2),"send blocked fake write");
        while(GetTickCount64()<deadline && !clipboard->write_entered.load()){check(tick(),"blocked write progress");Sleep(1);}
        check(clipboard->write_entered.load() && host.session()->release(0),"release with write in flight");
        for(int i=0;i<100;++i){check(tick(),"release barrier progress");Sleep(1);}
        check(restores.load()>0 && host.session()->phase()==SessionPhase::release_wait,"receiver restores input before acknowledging blocked clipboard");
        clipboard->block_write.store(false);
        while(GetTickCount64()<deadline && host.session()->phase()!=SessionPhase::ready){check(tick(),"release barrier completion");Sleep(1);}
        check(host.session()->phase()==SessionPhase::ready && host.request_focus(7,0,0),"enter new focus after barrier");
        while(GetTickCount64()<deadline && host.session()->phase()!=SessionPhase::active){check(tick(),"new focus progress");Sleep(1);}
        check(host.session()->phase()==SessionPhase::active && host.session()->request_clipboard_ready(),"new focus clipboard ready");
        while(GetTickCount64()<deadline && !host.session()->clipboard_can_sync()){check(tick(),"new focus ready progress");Sleep(1);}
        check(host.session()->clipboard_can_sync(),"new focus synchronizes after barrier");
        check(host.session()->send_clipboard_initial_done(),"skip new focus initial snapshot");
        const std::vector<std::uint8_t> png={0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0,0,0,0x0d,0x49,0x48,0x44,0x52,0,0,0,1,0,0,0,1,8,6,0,0,0,0x1f,0x15,0xc4,0x89,0,0,0,0x0b,0x49,0x44,0x41,0x54,0x78,0x9c,0x63,0x60,0,2,0,0,5,0,1,0x7a,0x5e,0xab,0x3f,0,0,0,0,0x49,0x45,0x4e,0x44,0xae,0x42,0x60,0x82};
        check(host.session()->clipboard_start(3,hop::ClipboardFormat::png,static_cast<std::uint32_t>(png.size())) && host.session()->clipboard_chunk(3,png.data(),png.size()),"send fake remote PNG");
        while(GetTickCount64()<deadline && !clipboard->has_png(png)){check(tick(),"PNG inbound progress");Sleep(1);}
        check(clipboard->has_png(png),"runtime applies fake remote PNG");clipboard->set_png(png);
        while(GetTickCount64()<deadline && received<2){check(tick(),"PNG outbound progress");Sleep(1);}
        check(received==2 && got==png,"runtime sends fake local PNG");
        check(host.session()->pause_clipboard(),"pause clipboard for focus");for(int i=0;i<30;++i){check(tick(),"clipboard pause progress");Sleep(2);}
        clipboard->set_text("later");for(int i=0;i<100;++i){check(tick(),"paused clipboard poll");Sleep(2);}check(received==2,"paused focus never sends later clipboard");
        host.cancel();receiver.stop();
    }
    {
        auto sockets=pair(); auto rejected=CandidateExchange::start(sockets.client,false,true,1,"Host",a.cert.data(),GetTickCount64());
        check(!rejected,"candidate exchange requires explicit user action"); closesocket(sockets.server);
    }
    {
        auto sockets=pair(); const auto now=GetTickCount64();
        auto host=CandidateExchange::start(sockets.client,true,true,1,"Host",a.cert.data(),now);
        auto peer=CandidateExchange::start(sockets.server,true,false,2,"Receiver",b.cert.data(),now);
        check(host && peer,"start user-requested candidate exchange"); PairIo h=PairIo::pending,r=PairIo::pending; const auto deadline=GetTickCount64()+2000;
        while(GetTickCount64()<deadline && (h==PairIo::pending || r==PairIo::pending)) { if(h==PairIo::pending) h=host->step(GetTickCount64()); if(r==PairIo::pending) r=peer->step(GetTickCount64()); Sleep(1); }
        check(h==PairIo::ready && r==PairIo::ready,"two bounded HPOP candidates");
        auto hc=host->take(GetTickCount64()),rc=peer->take(GetTickCount64()); check(hc && rc && hc->offer.name=="Receiver" && rc->offer.name=="Host","candidate names are hints");
        check(hc->fingerprint==b.fingerprint.data() && rc->fingerprint==a.fingerprint.data(),"recomputed candidate fingerprints");
        check(hc->valid_at(now+119999) && !hc->valid_at(now+120000) && !host->take(now+1),"candidate expiry and one-shot delivery");
        const auto prompt=pairing_prompt_text(UiLanguage::chinese,a.fingerprint.data(),b.fingerprint.data(),false);
        check(!prompt.empty() && prompt.find(std::wstring(a.fingerprint.data(),a.fingerprint.data()+16))!=std::wstring::npos && prompt.find(std::wstring(b.fingerprint.data()+48,b.fingerprint.data()+64))!=std::wstring::npos,"fingerprint UI renders full compared values");
        check(pairing_prompt_text(UiLanguage::english,"bad",b.fingerprint.data(),false).empty(),"invalid fingerprint UI rejected");
        auto review=PairingReview::create(a.cert.data(),std::move(*hc),now+1); check(review && review->local_fingerprint()==a.fingerprint.data() && review->remote_fingerprint()==b.fingerprint.data(),"display full fingerprints for manual comparison");
        MemoryCredentials fake; TrustStore trust(fake); VerifiedPeerCertificate selected;
        check(!review->resolve_hint(b.fingerprint.data(),now+1,trust,selected),"candidate not trusted before user confirmation");
        check(review->approve_after_independent_comparison(now+1) && review->resolve_hint(b.fingerprint.data(),now+2,trust,selected) && selected.pem==b.cert.data(),"manual confirmation allows temporary pin only");
        check(fake.writes==0 && !review->resolve_hint(b.fingerprint.data(),now+120000,trust,selected),"temporary pin expires and creates no trust");
        review->cancel(); check(!review->resolve_hint(b.fingerprint.data(),now+3,trust,selected),"cancel removes temporary pin");
    }
    {
        auto sockets=pair(); auto now=GetTickCount64();
        auto host=CandidateExchange::start(sockets.client,true,true,1,"Host",a.cert.data(),now);
        auto peer=CandidateExchange::start(sockets.server,true,false,1,"Other host",b.cert.data(),now);
        check(host && peer,"start wrong-role exchange"); PairIo h=PairIo::pending,r=PairIo::pending; const auto deadline=GetTickCount64()+2000;
        while(GetTickCount64()<deadline && h==PairIo::pending && r==PairIo::pending) {h=host->step(GetTickCount64());r=peer->step(GetTickCount64());Sleep(1);}
        check(h==PairIo::failed || r==PairIo::failed,"same-role candidate rejected");
    }
    {
        auto sockets=pair(); const auto now=GetTickCount64();
        auto host=CandidateExchange::start(sockets.client,true,true,1,"Host",a.cert.data(),now);
        hop::PairOffer offer; offer.role=2; offer.name="Peer"; offer.certificate_pem=b.cert.data(); offer.nonce[0]=1;
        auto invalid=hop::encode_pair_offer(offer); invalid.push_back(0x7f);
        check(host && send(sockets.server,reinterpret_cast<const char*>(invalid.data()),static_cast<int>(invalid.size()),0)==static_cast<int>(invalid.size()),"send oversized candidate");
        check(shutdown(sockets.server,SD_SEND)==0,"finish invalid offer"); auto state=PairIo::pending; const auto deadline=GetTickCount64()+2000;
        while(GetTickCount64()<deadline && state==PairIo::pending) {state=host->step(GetTickCount64());Sleep(1);}
        check(state==PairIo::failed && !host->take(GetTickCount64()),"extra candidate data rejected"); closesocket(sockets.server);
    }
    {
        auto sockets=pair();const auto now=GetTickCount64();auto responder=CandidateExchange::start(sockets.server,true,false,2,"Receiver",b.cert.data(),now);
        hop::PairOffer offer;offer.role=1;offer.name="Host";offer.certificate_pem=a.cert.data();offer.nonce[0]=1;const auto bytes=hop::encode_pair_offer(offer);
        check(responder && send(sockets.client,reinterpret_cast<const char*>(bytes.data()),static_cast<int>(bytes.size()),0)==static_cast<int>(bytes.size()),"send complete offer without EOF");
        for(int i=0;i<30;++i){check(responder->step(GetTickCount64())==PairIo::pending,"responder waits for EOF before replying");Sleep(1);}
        u_long nonblocking=1;check(ioctlsocket(sockets.client,FIONBIO,&nonblocking)==0,"nonblocking candidate probe");char probe=0;
        check(recv(sockets.client,&probe,1,MSG_PEEK)==SOCKET_ERROR && WSAGetLastError()==WSAEWOULDBLOCK,"no response before input EOF");
        const char extra=0x7f;check(send(sockets.client,&extra,1,0)==1 && shutdown(sockets.client,SD_SEND)==0,"send delayed trailing byte");
        auto state=PairIo::pending;const auto deadline=GetTickCount64()+2000;while(GetTickCount64()<deadline && state==PairIo::pending){state=responder->step(GetTickCount64());Sleep(1);}
        check(state==PairIo::failed && !responder->take(GetTickCount64()),"responder rejects delayed trailing byte without candidate");closesocket(sockets.client);
    }
    {
        auto sockets=pair(); const auto now=GetTickCount64();
        auto sender=PeerSelector::sender(sockets.client,a.fingerprint.data(),now); auto receiver=PeerSelector::receiver(sockets.server,now);
        check(sender && receiver,"start bounded HPSL exchange"); auto h=PairIo::pending,r=PairIo::pending; const auto deadline=GetTickCount64()+2000;
        while(GetTickCount64()<deadline && (h==PairIo::pending || r==PairIo::pending)) {if(h==PairIo::pending) h=sender->step(GetTickCount64());if(r==PairIo::pending) r=receiver->step(GetTickCount64());Sleep(1);}
        check(h==PairIo::ready && r==PairIo::ready && receiver->hint()==a.fingerprint.data(),"selector identifies sender only as lookup hint");
        auto c=TlsSocket::adopt(sender->release(),true,creds(a,b)); auto s=TlsSocket::adopt(receiver->release(),false,creds(b,a));
        check(c && s && handshake(*c,*s,true),"selector stream hands off to pinned TLS");
    }
    {
        auto sockets=pair(); const auto now=GetTickCount64(); auto receiver=PeerSelector::receiver(sockets.server,now);
        check(receiver!=nullptr,"selector receiver created"); auto bytes=hop::encode_peer_selector(a.fingerprint.data()); bytes.push_back(0x5a);
        check(send(sockets.client,reinterpret_cast<const char*>(bytes.data()),static_cast<int>(bytes.size()),0)==static_cast<int>(bytes.size()),"send selector and following byte");
        auto state=PairIo::pending; const auto deadline=GetTickCount64()+2000;
        while(GetTickCount64()<deadline && state==PairIo::pending) {state=receiver->step(GetTickCount64());Sleep(1);}
        check(state==PairIo::ready,"selector prefix accepted"); SOCKET returned=receiver->release(); char extra=0;
        check(recv(returned,&extra,1,0)==1 && extra==0x5a,"selector never consumes following TLS byte");
        closesocket(returned); closesocket(sockets.client);
    }
    {
        auto sockets=pair(); auto exchange=CandidateExchange::start(sockets.client,true,true,1,"Host",a.cert.data(),0);
        check(exchange && exchange->step(30000)==PairIo::failed,"candidate expires and closes"); closesocket(sockets.server);
    }
    {
        auto sockets=pair(); auto c=TlsSocket::adopt(sockets.client,true,creds(a,b)); auto s=TlsSocket::adopt(sockets.server,false,creds(b,a));
        check(c && s,"create nonblocking TLS endpoints");
        int nodelay=0, nodelay_len=sizeof(nodelay);
        check(getsockopt(sockets.client,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<char*>(&nodelay),&nodelay_len)==0 && nodelay==1,"TCP_NODELAY enabled on owned socket");
        std::uint8_t data=1; check(c->write(&data,1).status==TlsIoStatus::failed && s->read(&data,1).status==TlsIoStatus::failed,"no pre-auth data");
        check(handshake(*c,*s,true),"mutual certificate pin authentication");
        check(c->authenticated() && s->authenticated(),"both endpoints authenticated");
        transfer(*c,*s,"client to server"); transfer(*s,*c,"server to client");
        std::array<std::uint8_t,128> empty{}; check(s->read(empty.data(),empty.size()).status==TlsIoStatus::again,"empty socket is nonblocking");
        s.reset();
        const auto deadline=GetTickCount64()+3000; TlsIoResult result{TlsIoStatus::again};
        while(GetTickCount64()<deadline && result.status==TlsIoStatus::again) { result=c->read(empty.data(),empty.size()); Sleep(1); }
        check(result.status==TlsIoStatus::closed || result.status==TlsIoStatus::failed,"peer closure ends stream");
    }
    {
        auto sockets=pair(); auto c=TlsSocket::adopt(sockets.client,true,creds(a,bad)); auto s=TlsSocket::adopt(sockets.server,false,creds(b,a));
        check(c && s && handshake(*c,*s,false),"wrong server pin rejected");
        check(!c->authenticated(),"wrong server is never authenticated");
    }
    {
        auto sockets=pair(); auto c=TlsSocket::adopt(sockets.client,true,creds(bad,b)); auto s=TlsSocket::adopt(sockets.server,false,creds(b,a));
        check(c && s && handshake(*c,*s,false),"untrusted client rejected");
        check(!s->authenticated(),"untrusted client is never authenticated");
    }
    {
        auto sockets=pair(); auto invalid=creds(a,b); invalid.peer.pem.clear();
        auto c=TlsSocket::adopt(sockets.client,true,invalid);
        check(!c,"missing verified pin rejected");
        int opt=0, optlen=sizeof(opt);
        check(getsockopt(sockets.client,SOL_SOCKET,SO_TYPE,reinterpret_cast<char*>(&opt),&optlen)==SOCKET_ERROR &&
              WSAGetLastError()==WSAENOTSOCK,"failed adopt consumes the socket");
        closesocket(sockets.server);
    }
    {
        SOCKET s=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP); check(s!=INVALID_SOCKET,"create unconnected test socket");
        check(!TlsSocket::adopt(s,true,creds(a,b)),"unconnected socket rejected");
    }
    {
        auto sockets=pair(); int small=4096;
        check(setsockopt(sockets.client,SOL_SOCKET,SO_SNDBUF,reinterpret_cast<const char*>(&small),sizeof(small))==0,"limit test send buffer");
        auto c=TlsSocket::adopt(sockets.client,true,creds(a,b)); auto s=TlsSocket::adopt(sockets.server,false,creds(b,a));
        check(c && s && handshake(*c,*s,true),"backpressure test handshake");
        std::array<std::uint8_t,16384> payload{}; payload.fill(0x41);
        bool blocked=false; const auto deadline=GetTickCount64()+5000;
        for(int i=0;i<2048 && GetTickCount64()<deadline;++i) {
            auto result=c->write(payload.data(),payload.size());
            if(result.status==TlsIoStatus::again) { blocked=true; break; }
            check(result.status==TlsIoStatus::ready && result.bytes>0,"write while applying backpressure");
        }
        check(blocked,"nonblocking write reaches backpressure");
        payload[0]=0x42;
        check(c->write(payload.data(),payload.size()).status==TlsIoStatus::failed && !c->authenticated(),"changed retry buffer fails closed");
    }
    {
        auto sockets=pair(); auto c=TlsSocket::adopt(sockets.client,true,creds(a,b)); auto s=TlsSocket::adopt(sockets.server,false,creds(b,a));
        check(c && s && handshake(*c,*s,true),"session pinned TLS handshake");
        int restored_host=0, restored_receiver=0, injected=0, clipboard_received=0;
        Session host(SessionRole::host,42,{},{{},{},[&]{++restored_host; return true;}});
        Session receiver(SessionRole::receiver,0,{7},{[](const hop::Frame&){return true;},[&](const hop::Frame&){++injected; return true;},[&]{++restored_receiver; return true;}});
        auto sequence=[] { return std::uint32_t(99); };
        auto clipboard=[&](const hop::ClipboardValue& value,std::uint32_t baseline) { ++clipboard_received; return baseline==99 && value.data==std::vector<std::uint8_t>({'h','i'}); };
        check(host.enable_clipboard(sequence,clipboard) && receiver.enable_clipboard(sequence,clipboard),"enable fake clipboard transport");
        auto start=GetTickCount64(); check(host.start(c->authenticated(),start) && receiver.start(s->authenticated(),start),"start authenticated sessions");
        SessionPump hp(*c,host), rp(*s,receiver);
        auto until=[&](auto predicate) {
            const auto deadline=GetTickCount64()+2000;
            while(GetTickCount64()<deadline && !predicate()) { auto now=GetTickCount64(); if (!hp.step(now) || !rp.step(now)) return false; Sleep(1); }
            return predicate();
        };
        check(until([&]{return host.phase()==SessionPhase::ready && receiver.phase()==SessionPhase::ready;}),"HELLO and ACK over pinned TLS");
        std::vector<std::uint8_t> screens={1,3,0,0,0,1,0,0,0,0,0,0,0,0,0,7,0,0,0,0,0,0,0,0,0,0,7,128,0,0,4,56};
        check(receiver.advertise_screens(screens),"queue screen announcement");
        // Pump the announcement before requesting focus.
        bool entered=false;
        check(until([&]{if (!entered) entered=host.enter(43,7,0,0); return entered;}),"screen announcement and enter");
        check(until([&]{return host.phase()==SessionPhase::active;}),"focus acknowledged over TLS");
        check(host.send_input({1,4,0,0,0,6,0,0,1,0,0,0}),"queue fake key event");
        check(until([&]{return injected==1;}),"fake input delivered over TLS");
        check(host.request_clipboard_ready() && receiver.request_clipboard_ready(),"queue clipboard readiness");
        check(until([&]{return host.clipboard_can_sync() && receiver.clipboard_can_sync();}),"clipboard ready over pinned TLS");
        const std::uint8_t text[]={'h','i'};
        check(host.clipboard_start(1,hop::ClipboardFormat::text,2) && host.clipboard_chunk(1,text,2),"queue fake clipboard transfer");
        check(until([&]{return clipboard_received==1;}),"fake clipboard transfer over pinned TLS");
        check(host.release(0),"release focus");
        check(until([&]{return host.phase()==SessionPhase::ready && receiver.phase()==SessionPhase::ready;}),"release ACK over TLS");
        check(restored_host==1 && restored_receiver==1,"both sides restored");
        check(host.enter(44,7,0,0),"start second focus");
        check(until([&]{return host.phase()==SessionPhase::active;}),"second focus active");
        check(!host.send_input({1,4,0,0,0,6,0,0,0,0,0,0}),"unmatched key release fails");
        check(!hp.step(GetTickCount64()) && !c->authenticated(),"failed sender closes authenticated socket");
        const auto close_deadline=GetTickCount64()+2000; bool receiver_closed=false;
        while(GetTickCount64()<close_deadline && !(receiver_closed=!rp.step(GetTickCount64()))) Sleep(1);
        check(receiver_closed && restored_receiver==2,"peer closure restores receiver input");
    }
    {
        auto sockets=pair(); auto c=TlsSocket::adopt(sockets.client,true,creds(a,b)); auto s=TlsSocket::adopt(sockets.server,false,creds(b,a));
        check(c && s && handshake(*c,*s,true),"pair confirmation pinned TLS handshake");
        int restored_host=0,restored_peer=0;
        Session host(SessionRole::host,42,{},{{},{},[&]{++restored_host;return true;}});
        Session receiver(SessionRole::receiver,0,{7},{[](const hop::Frame&){return true;},[](const hop::Frame&){return true;},[&]{++restored_peer;return true;}});
        check(host.bind_verified_peer(c->pinned_peer_fingerprint()) && receiver.bind_verified_peer(s->pinned_peer_fingerprint()),"bind compared peer fingerprints");
        check(host.require_pairing_confirmation(),"require first-pair confirmation");
        auto now=GetTickCount64(); check(host.start(true,now) && receiver.start(true,now),"start pair sessions"); SessionPump hp(*c,host),rp(*s,receiver);
        auto until=[&](auto predicate) {const auto deadline=GetTickCount64()+2000; while(GetTickCount64()<deadline && !predicate()) { auto t=GetTickCount64(); if(!hp.step(t)||!rp.step(t)) return false; Sleep(1);} return predicate();};
        check(until([&]{return host.phase()==SessionPhase::pair_wait && receiver.phase()==SessionPhase::pair_wait;}),"both await human confirmation");
        MemoryCredentials fake; TrustStore trust(fake); VerifiedPeerCertificate loaded;
        check(!trust.remember(host,*c,b.cert.data()) && fake.writes==0,"unconfirmed peer is never stored");
        check(host.confirm_pairing(),"host confirms compared fingerprints");
        for(int i=0;i<20;++i){check(hp.step(GetTickCount64()) && rp.step(GetTickCount64()),"pump first confirmation");Sleep(1);}
        check(!host.pairing_confirmed() && !receiver.pairing_confirmed(),"one confirmation cannot establish trust");
        check(receiver.confirm_pairing(),"peer confirms compared fingerprints");
        check(until([&]{return host.pairing_confirmed() && receiver.pairing_confirmed();}),"both confirmations arrive under TLS");
        check(host.phase()==SessionPhase::ready && receiver.phase()==SessionPhase::ready && !restored_host && !restored_peer,"pair completes without real input");
        const auto peer_fp=c->pinned_peer_fingerprint();
        check(!trust.remember(host,*c,a.cert.data()) && fake.writes==0,"different certificate rejected");
        check(trust.remember(host,*c,b.cert.data()) && trust.load_peer(peer_fp,loaded) && loaded.pem==b.cert.data(),"confirmed pinned peer persisted in fake backend");
        host.disconnect(); check(!trust.remember(host,*c,b.cert.data()),"disconnected pairing cannot save trust");
        check(trust.forget(peer_fp) && !trust.load_peer(peer_fp,loaded),"forget removes peer trust");
    }
    {
        auto sockets=pair(); int restored=0;
        Session host(SessionRole::host,42,{},{{},{},[&]{++restored;return true;}});
        Session receiver(SessionRole::receiver,0,{7},{[](const hop::Frame&){return true;},[](const hop::Frame&){return true;},[]{return true;}});
        auto now=GetTickCount64(); auto c=Connection::adopt(sockets.client,true,creds(a,b),host,true,now);
        auto s=Connection::adopt(sockets.server,false,creds(b,a),receiver,false,now);
        check(c && s && host.phase()==SessionPhase::initial && receiver.phase()==SessionPhase::initial,"connection waits for TLS before session");
        const auto deadline=GetTickCount64()+2000;
        while(GetTickCount64()<deadline && (host.phase()!=SessionPhase::pair_wait || receiver.phase()!=SessionPhase::pair_wait)) {check(c->step(GetTickCount64()) && s->step(GetTickCount64()),"connection handshake progress");Sleep(1);}
        check(c->authenticated() && s->authenticated() && host.phase()==SessionPhase::pair_wait && receiver.phase()==SessionPhase::pair_wait,"connection requires both confirmations");
        check(host.confirm_pairing() && receiver.confirm_pairing(),"confirm through authenticated connection");
        const auto ready_deadline=GetTickCount64()+2000;
        while(GetTickCount64()<ready_deadline && (!host.pairing_confirmed() || !receiver.pairing_confirmed())) {check(c->step(GetTickCount64()) && s->step(GetTickCount64()),"pair confirm progress");Sleep(1);}
        check(host.pairing_confirmed() && receiver.pairing_confirmed(),"connection establishes confirmed pairing");
        c.reset(); check(restored==1 && host.phase()==SessionPhase::failed,"connection destruction closes and restores");
    }
    {
        // Real encrypted loopback, synthetic macOS peer: verify translated
        // frames seen after TLS decryption, without sending OS input.
        auto sockets=pair(); auto c=TlsSocket::adopt(sockets.client,true,creds(a,b)); auto s=TlsSocket::adopt(sockets.server,false,creds(b,a));
        check(c && s && handshake(*c,*s,true),"shortcut TLS handshake");
        int restored=0; Session host(SessionRole::host,42,{},{{},{},[&]{++restored; return true;}});
        check(host.start(c->authenticated(),GetTickCount64()),"start shortcut session"); SessionPump pump(*c,host);
        auto put=[&](const hop::Frame& frame) {
            auto bytes=hop::encode_frame(frame); const auto deadline=GetTickCount64()+2000;
            while(GetTickCount64()<deadline) { auto result=s->write(bytes.data(),bytes.size());
                if(result.status==TlsIoStatus::ready) return result.bytes==bytes.size();
                if(result.status!=TlsIoStatus::again) return false; Sleep(1); }
            return false;
        };
        check(put({hop::FrameKind::control,1,42,{1,2,0,0,2,1,0,0,0,0,0,0,0,0,0,25}}),"synthetic Mac ACK");
        check(put({hop::FrameKind::control,2,42,{1,3,0,0,0,1,0,0,0,0,0,0,0,0,0,7,0,0,0,0,0,0,0,0,0,0,7,128,0,0,4,56}}),"synthetic Mac screens");
        bool entered=false; auto deadline=GetTickCount64()+2000;
        while(GetTickCount64()<deadline && !entered) { check(pump.step(GetTickCount64()),"pump synthetic peer"); entered=host.enter(43,7,0,0); Sleep(1); }
        check(entered && pump.step(GetTickCount64()),"send enter to synthetic Mac");
        check(put({hop::FrameKind::control,3,43,{1,0x11,0,0,0,0,0,0,0,0,0,7}}),"synthetic enter ACK");
        deadline=GetTickCount64()+2000; while(GetTickCount64()<deadline && host.phase()!=SessionPhase::active) {check(pump.step(GetTickCount64()),"pump enter ACK");Sleep(1);}
        check(host.phase()==SessionPhase::active,"synthetic Mac active");
        check(host.send_input({1,4,0,0,0,0xe0,1,4,1,0,0,0}) && host.send_input({1,4,0,0,0,6,1,4,1,0,0,0}),"queue Ctrl+C with Caps Lock");
        std::vector<hop::Frame> got; hop::FrameDecoder decoder([&](hop::Frame&& frame){got.push_back(std::move(frame)); return true;});
        std::array<std::uint8_t,4096> bytes{}; deadline=GetTickCount64()+2000;
        while(GetTickCount64()<deadline && got.size()<4) {
            check(pump.step(GetTickCount64()),"pump translated keys"); auto r=s->read(bytes.data(),bytes.size());
            if(r.status==TlsIoStatus::ready) check(decoder.feed(bytes.data(),r.bytes),"decode encrypted frames");
            else check(r.status==TlsIoStatus::again,"read synthetic peer"); Sleep(1);
        }
        check(got.size()>=4 && got[2].kind==hop::FrameKind::input && got[2].payload[5]==0xe3 && got[2].payload[6]==1 && got[2].payload[7]==0x40,"translated Command preserves Caps Lock over TLS");
        check(got[3].kind==hop::FrameKind::input && got[3].payload[5]==6 && got[3].payload[7]==0x40,"translated C follows Command over TLS");
    }
    WSACleanup();
    std::cout<<"PASS "<<checks<<" Windows nonblocking pinned TLS socket checks; loopback and temporary test identities only\n";
}
