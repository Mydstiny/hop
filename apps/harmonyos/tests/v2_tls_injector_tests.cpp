#include "v2_injector.h"
#include "hop/posix_session_pump.h"
#include "hop/secure_channel_c.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
using namespace hop;
namespace {
int checks=0;void check(bool ok,const char* m){++checks;if(!ok){std::cerr<<"FAIL "<<m<<'\n';std::exit(1);}}
struct Identity{std::array<char,8192> cert{},key{};std::array<char,65> fp{};Identity(){check(hop_tls_generate_identity(cert.data(),cert.size(),key.data(),key.size(),fp.data(),fp.size())==0,"test identity");}~Identity(){volatile char* p=key.data();for(std::size_t i=0;i<key.size();++i)p[i]=0;}};
struct Pair{int a,b;};
Pair sockets(){int l=socket(AF_INET,SOCK_STREAM,0);check(l>=0,"listener");sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);check(bind(l,reinterpret_cast<sockaddr*>(&a),sizeof(a))==0&&listen(l,1)==0,"bind");socklen_t n=sizeof(a);check(getsockname(l,reinterpret_cast<sockaddr*>(&a),&n)==0,"port");int c=socket(AF_INET,SOCK_STREAM,0);check(c>=0&&connect(c,reinterpret_cast<sockaddr*>(&a),sizeof(a))==0,"connect");int s=accept(l,nullptr,nullptr);close(l);check(s>=0,"accept");return {c,s};}
void handshake(PosixTlsSocket& a,PosixTlsSocket& b){for(int i=0;i<5000;++i){auto x=a.handshake(),y=b.handshake();check(x!=SocketProgress::failed&&y!=SocketProgress::failed,"TLS handshake");if(x==SocketProgress::ready&&y==SocketProgress::ready)return;std::this_thread::sleep_for(std::chrono::milliseconds(1));}check(false,"TLS timeout");}
std::vector<uint8_t> screens(){return {1,3,0,0,0,1,0,0,0,0,0,0,0,0,0,7,0,0,0,0,0,0,0,0,0,0,3,233,0,0,1,245};}
}
int main(){
    fake::Reset();Identity a,b;auto fd=sockets();
    auto c=PosixTlsSocket::adopt(fd.a,true,{a.cert.data(),a.key.data(),{b.cert.data()}});
    auto s=PosixTlsSocket::adopt(fd.b,false,{b.cert.data(),b.key.data(),{a.cert.data()}});
    check(c&&s,"TLS endpoints");handshake(*c,*s);
    int hostRestores=0;V2Injector injector;check(injector.configure({{6,0,0,1001,501}}),"configure primary display");auto cb=V2Injector::callbacks();
    v2::Session host(v2::SessionRole::host,42,{},{{},{},[&]{++hostRestores;return true;}},Platform::macos);
    v2::Session remote(v2::SessionRole::receiver,0,{7},{
        [&](const Frame& f){return cb.enter(&injector,f.payload.data(),f.payload.size(),f.epoch)==1;},
        [&](const Frame& f){return cb.input(&injector,f.payload.data(),f.payload.size(),f.epoch)==1;},
        [&]{return cb.restore(&injector)==1;}},Platform::harmonyos);
    check(host.bind_verified_peer(b.fp.data())&&remote.bind_verified_peer(a.fp.data()),"bind exact TLS pins");
    check(host.start(true,0)&&remote.start(true,0),"start sessions");
    PosixSessionPump hp(*c,host),rp(*s,remote,[&]{return injector.beginBatch();},[&]{injector.endBatch();});
    auto started=std::chrono::steady_clock::now();
    auto until=[&](auto done){for(int i=0;i<5000;++i){auto n=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();if(!hp.step(n)||!rp.step(n))return false;if(done())return true;std::this_thread::sleep_for(std::chrono::milliseconds(1));}return false;};
    check(until([&]{return host.phase()==v2::SessionPhase::ready&&remote.phase()==v2::SessionPhase::ready;}),"TLS hello");
    check(remote.advertise_screens(screens()),"screens");bool entering=false;
    check(until([&]{if(!entering)entering=host.enter(43,7,32768,32768);return entering&&host.phase()==v2::SessionPhase::active;}),"TLS focus");
    check(host.send_input({1,4,0,0,0,0xe3,0,64,1,0,0,0})&&host.send_input({1,4,0,0,0,6,0,64,1,0,0,0}),"queue shortcut");
    check(until([&]{return fake::keys.count(MapHidKey(0x06))!=0;}),"TLS to fake Harmony injection");
    check(fake::keys.count(MapHidKey(0xe0))==1,"translated Control held");
    check(host.send_input({1,3,0,0,0,0,0,0,0,0,0,120}),"queue wheel");
    check(until([&]{for(const auto& event:fake::mouse)if(event.action==MOUSE_ACTION_AXIS_BEGIN)return true;return false;}),"TLS to fake wheel");
    bool expectedScale=false;for(const auto& event:fake::mouse)if(event.action==MOUSE_ACTION_AXIS_BEGIN&&event.value==-22.5f)expectedScale=true;
    check(expectedScale,"1.5x wheel uses expected injected value");
    fake::authorization=UNAUTHORIZED;
    check(host.send_input({1,4,0,0,0,6,0,64,0,0,0,0}),"queue release under revoked permission");
    bool revoked=false;for(int i=0;i<1000;++i){auto n=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();if(!hp.step(n)||!rp.step(n)){revoked=true;break;}std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    check(revoked,"permission loss closes session");
    fake::authorization=AUTHORIZED;check(injector.release()&&fake::Empty(),"best effort cleanup retries after authorization returns");
    for(int i=0;i<1000 && hostRestores==0;++i){auto n=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();if(!hp.step(n))break;std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    check(hostRestores>=1,"disconnect restores host after permission loss");
    std::cout<<"PASS "<<checks<<" TLS-to-Harmony fake-SDK checks; no real device input\n";
}
