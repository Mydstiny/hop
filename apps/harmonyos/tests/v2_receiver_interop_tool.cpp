#include "v2_receiver.h"
#include <multimodalinput/oh_input_manager.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <utility>
#include <poll.h>
#include <unistd.h>
namespace {
std::string read(const char* path) { std::ifstream file(path,std::ios::binary);return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()}; }
}
int main(int argc,char** argv) {
    if(argc!=6)return 2;
    const long port=std::strtol(argv[2],nullptr,10);if(port<1||port>65535)return 2;
    auto own=read(argv[3]),key=read(argv[4]),peer=read(argv[5]);if(own.empty()||key.empty()||peer.empty())return 2;
    fake::Reset();std::atomic<bool> candidate{false},pairing{false},paired{false},active{false},released{false},error{false};
    hop::V2Receiver receiver([&](const hop::V2ReceiverEvent& event){
        if(event.state=="candidate" && event.peer_certificate_pem==peer)candidate=true;
        if(event.state=="pairing")pairing=true;
        if(event.state=="paired")paired=true;
        if(event.state=="remote")active=true;
        if(event.state=="ready" && active.load())released=true;
        if(event.state=="error")error=true;
    });
    if(!receiver.startOffer(argv[1],static_cast<std::uint16_t>(port),"Harmony test",own))return 3;
    const auto offerDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(std::chrono::steady_clock::now()<offerDeadline && receiver.isRunning())std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if(!candidate||receiver.isRunning()||error)return 3;
    hop::V2ReceiverConfig config;config.host=argv[1];config.port=static_cast<std::uint16_t>(port);
    config.certificate_pem=std::move(own);config.private_key_pem=std::move(key);config.verified_peer_certificate_pem=std::move(peer);
    config.displays={{6,0,0,1001,501}};config.require_pairing=true;
    if(!receiver.start(std::move(config)))return 3;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    bool confirmed=false,saved=false;
    while(std::chrono::steady_clock::now()<deadline && !released && !error) {
        if(pairing&&!confirmed){receiver.confirmPairing();confirmed=true;}
        if(paired&&!saved){receiver.confirmStorage();saved=true;}
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    // Do not stop the receiver before its queued RELEASE_ACK is sent. The
    // test parent acknowledges receipt over this separate test-only pipe.
    bool parentAcknowledged=false;
    if(released) {pollfd signal{STDIN_FILENO,POLLIN,0};if(poll(&signal,1,3000)>0){char byte=0;parentAcknowledged=::read(STDIN_FILENO,&byte,1)==1 && byte==1;}}
    bool keyDown=false,keyUp=false,wheel=false;
    {std::lock_guard<std::mutex> lock(fake::mutex);
        for(const auto& event:fake::key){keyDown|=event.action==KEY_ACTION_DOWN;keyUp|=event.action==KEY_ACTION_UP;}
        for(const auto& event:fake::mouse)wheel|=(event.action==MOUSE_ACTION_AXIS_BEGIN && event.value!=0);
    }
    receiver.stop();
    if(!confirmed||!saved||!active||!released||!parentAcknowledged||!keyDown||!keyUp||!wheel||!fake::Empty()) {
        std::cerr<<"fake interop failed: active="<<active<<" released="<<released<<" ack="<<parentAcknowledged<<" down="<<keyDown<<" up="<<keyUp<<" wheel="<<wheel<<"\n";return 4;
    }
    std::cout<<"PASS Harmony receiver interop fake SDK: focus, key, wheel and release\n";
    return 0;
}
