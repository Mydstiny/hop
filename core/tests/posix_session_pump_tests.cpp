#include "hop/posix_session_pump.h"
#include "hop/secure_channel_c.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
using namespace hop;
namespace {
int checks=0;
void check(bool ok,const char* msg){++checks;if(!ok){std::cerr<<"FAIL "<<msg<<'\n';std::exit(1);}}
struct Identity{std::array<char,8192> cert{},key{};std::array<char,65> fp{};Identity(){check(hop_tls_generate_identity(cert.data(),cert.size(),key.data(),key.size(),fp.data(),fp.size())==0,"generate isolated identity");}~Identity(){volatile char* p=key.data();for(std::size_t i=0;i<key.size();++i)p[i]=0;}};
struct Pair{int a,b;};
Pair sockets(){int l=socket(AF_INET,SOCK_STREAM,0);check(l>=0,"listener");sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);check(bind(l,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==0&&listen(l,1)==0,"bind localhost");socklen_t n=sizeof(addr);check(getsockname(l,reinterpret_cast<sockaddr*>(&addr),&n)==0,"port");int a=socket(AF_INET,SOCK_STREAM,0);check(a>=0&&connect(a,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==0,"connect");int b=accept(l,nullptr,nullptr);close(l);check(b>=0,"accept");return {a,b};}
void handshake(PosixTlsSocket& a,PosixTlsSocket& b){auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);while(std::chrono::steady_clock::now()<deadline){auto x=a.handshake(),y=b.handshake();check(x!=SocketProgress::failed&&y!=SocketProgress::failed,"TLS handshake progress");if(x==SocketProgress::ready&&y==SocketProgress::ready)return;std::this_thread::sleep_for(std::chrono::milliseconds(1));}check(false,"TLS handshake timeout");}
std::vector<std::uint8_t> screens(){return {1,3,0,0,0,1,0,0,0,0,0,0,0,0,0,7,0,0,0,0,0,0,0,0,0,0,3,233,0,0,1,245};}
}
int main(){
    Identity a,b;auto fd=sockets();
    auto client=PosixTlsSocket::adopt(fd.a,true,{a.cert.data(),a.key.data(),{b.cert.data()}});
    auto server=PosixTlsSocket::adopt(fd.b,false,{b.cert.data(),b.key.data(),{a.cert.data()}});
    check(client&&server,"adopt TLS endpoints");handshake(*client,*server);
    int host_restore=0,remote_restore=0,enters=0,inputs=0,begin=0,end=0;
    std::vector<Frame> received;
    v2::Session host(v2::SessionRole::host,42,{},{{},{},[&]{++host_restore;return true;}},Platform::macos);
    v2::Session remote(v2::SessionRole::receiver,0,{7},{[&](const Frame&){++enters;return true;},[&](const Frame& f){++inputs;received.push_back(f);return true;},[&]{++remote_restore;return true;}},Platform::harmonyos);
    check(host.bind_verified_peer(b.fp.data())&&remote.bind_verified_peer(a.fp.data()),"bind exact peer fingerprints");
    check(remote.start(true,0)&&host.start(true,0),"start after auth");
    PosixSessionPump hp(*client,host),rp(*server,remote,[&]{++begin;return true;},[&]{++end;});
    const auto start=std::chrono::steady_clock::now();
    auto until=[&](auto done){for(int i=0;i<5000;++i){auto now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();if(!hp.step(now)||!rp.step(now))return false;if(done())return true;std::this_thread::sleep_for(std::chrono::milliseconds(1));}return false;};
    check(until([&]{return host.phase()==v2::SessionPhase::ready&&remote.phase()==v2::SessionPhase::ready;}),"encrypted hello");
    check(remote.advertise_screens(screens()),"advertise remote screen");
    bool entered=false;
    check(until([&]{if(!entered)entered=host.enter(43,7,32768,32768);return entered&&host.phase()==v2::SessionPhase::active;}),"encrypted focus entry");
    check(enters==1&&begin==end&&begin>0,"bounded receiver authorization batches");
    check(host.send_input({1,4,0,0,0,0xe3,0,64,1,0,0,0})&&host.send_input({1,4,0,0,0,6,0,64,1,0,0,0}),"queue Mac shortcut");
    check(until([&]{return inputs==2;}),"encrypted input reaches receiver");
    check(received[0].payload[5]==0xe0&&received[1].payload[5]==6&&received[1].payload[7]==4,"Mac Command+C translates to Harmony Control+C");
    hp.stop();
    bool disconnected=false;
    for(int i=0;i<1000;++i){auto now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();if(!rp.step(now)){disconnected=true;break;}std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    check(disconnected&&host_restore>=1&&remote_restore>=1,"disconnect restores both ends");
    std::cout<<"PASS "<<checks<<" POSIX TLS/session pump checks; temporary identities and fake input only\n";
}
