#include "client.h"
#include "injector.h"
#include "pair_state.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <memory>

namespace hop {
struct InjectionFailure : std::runtime_error { using std::runtime_error::runtime_error; };
struct NetworkFailure : std::runtime_error { using std::runtime_error::runtime_error; };
struct PeerBye : std::runtime_error { using std::runtime_error::runtime_error; };
static void InjectRequire(bool ok,const char* reason){if(!ok)throw InjectionFailure(reason);}
static uint64_t Now(){return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
static void Random(uint8_t* out,size_t n){int f=open("/dev/urandom",O_RDONLY|O_CLOEXEC);Require(f>=0,"secure random unavailable");size_t i=0;while(i<n){ssize_t r=read(f,out+i,n-i);if(r<0&&errno==EINTR)continue;if(r<=0){close(f);throw std::runtime_error("secure random read failed");}i+=size_t(r);}close(f);}
static void Write(int fd,const Message& m){auto b=m.frame();size_t pos=0;uint64_t until=Now()+150;while(pos<b.size()){
    ssize_t n=send(fd,b.data()+pos,b.size()-pos,MSG_NOSIGNAL);if(n>0){pos+=size_t(n);continue;}if(n<0&&errno==EINTR)continue;
    if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK)&&Now()<until){pollfd p{fd,POLLOUT,0};poll(&p,1,10);continue;}throw NetworkFailure("TCP short write/timeout");}}
void Client::Stop(){stop_=true;{std::lock_guard<std::mutex> lock(fdMutex_);int f=fd_.load();if(f>=0)shutdown(f,SHUT_RDWR);}if(thread_.joinable())thread_.join();}
void Client::Run(Config cfg) noexcept {
    Injector input;int fd=-1;bool authenticated=false;PairState pairing;pairing.resuming=!cfg.token.empty();
    auto& resuming=pairing.resuming;auto& code=pairing.code;auto& direction=pairing.direction;
    auto& nonce=pairing.nonce;auto& peer=pairing.peer;auto& token=pairing.token;
    enum class Mode{Handshake,Local,Remote,Leaving};Mode mode=Mode::Handshake;
    uint32_t epoch=0,lastEpoch=0;bool armed=false;int enterX=0;
    uint64_t connected=0,lastReceive=0,lastSent=0,lastKeep=0,leaveDeadline=0;
    std::array<uint8_t,16> id{};
    auto notify=[&](const std::string& state,const std::string& message=""){event_({"state",state,message,"","",0});};
    auto hello=[&](Phase phase){Message m(Type::Hello);auto* p=m.data();p[0]=uint8_t(phase);p[1]=2;p[2]=direction;
        if(phase==Phase::Confirm||phase==Phase::Commit)Put32(p+4,code);Put32(p+8,cfg.width);Put32(p+12,cfg.height);
        std::copy(id.begin(),id.end(),p+16);std::copy(nonce.begin(),nonce.end(),p+32);
        if(phase==Phase::Resume||phase==Phase::Commit)std::copy(token.begin(),token.end(),p+48);
        size_t length=std::min<size_t>(31,cfg.name.size());while(length&&length<cfg.name.size()&&(uint8_t(cfg.name[length])&0xC0)==0x80)--length;
        std::copy_n(cfg.name.data(),length,p+80);return m;};
    auto bye=[&](uint8_t reason){input.Release();if(fd>=0){try{Message b(Type::Bye);b.data()[0]=reason;Write(fd,b);}catch(...){}}};
    try {
        Require(Injector::Authorization()==AUTHORIZED,"Injection permission must be granted before connection");
        Require(cfg.width>=1&&cfg.width<=65535&&cfg.height>=1&&cfg.height<=65535&&cfg.port>=1&&cfg.port<=65535,"display/port range");
        auto userId=Unhex(cfg.uuid,16);std::copy(userId.begin(),userId.end(),id.begin());Require(!Zero(id.data(),16),"empty local UUID");Random(nonce.data(),nonce.size());
        if(resuming){auto t=Unhex(cfg.token,32),p=Unhex(cfg.peerId,16);std::copy(t.begin(),t.end(),token.begin());std::copy(p.begin(),p.end(),peer.begin());Require(!Zero(token.data(),32)&&!Zero(peer.data(),16),"invalid binding");}
        input.Dimensions(cfg.width,cfg.height);notify("connecting");
        addrinfo hint{};hint.ai_family=AF_UNSPEC;hint.ai_socktype=SOCK_STREAM;hint.ai_flags=AI_NUMERICHOST;addrinfo* addresses=nullptr;
        Require(getaddrinfo(cfg.host.c_str(),std::to_string(cfg.port).c_str(),&hint,&addresses)==0,"Unable to resolve Mac address");
        std::unique_ptr<addrinfo,decltype(&freeaddrinfo)> addressGuard(addresses,freeaddrinfo);
        for(auto* a=addresses;a&&!stop_;a=a->ai_next){fd=socket(a->ai_family,a->ai_socktype,0);if(fd<0)continue;{std::lock_guard<std::mutex> lock(fdMutex_);fd_=fd;}fcntl(fd,F_SETFL,fcntl(fd,F_GETFL,0)|O_NONBLOCK);
            int yes=1;setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&yes,sizeof(yes));
            int r=connect(fd,a->ai_addr,a->ai_addrlen);if(r!=0&&errno==EINPROGRESS){uint64_t end=Now()+3000;while(!stop_&&Now()<end){pollfd p{fd,POLLOUT,0};int ready=poll(&p,1,50);if(ready>0){int err=0;socklen_t len=sizeof(err);getsockopt(fd,SOL_SOCKET,SO_ERROR,&err,&len);r=err? -1:0;break;}}}
            if(r==0)break;{std::lock_guard<std::mutex> lock(fdMutex_);close(fd);fd=-1;fd_=-1;}
        }
        Require(fd>=0&&!stop_,"Unable to connect to Mac");connected=lastReceive=lastSent=Now();Write(fd,hello(resuming?Phase::Resume:Phase::Pair));notify("handshake");
        Decoder decoder;
        auto handle=[&](const Message& m){const uint8_t* p=m.data();uint64_t now=Now();
            if(m.type()==Type::Keepalive){Require(!lastKeep||now-lastKeep>=100,"heartbeat rate");lastKeep=now;lastReceive=now;return;}
            if(m.type()==Type::Bye){input.Release();if(p[0]==4&&authenticated){event_({"unpair","","Mac unpaired this device","","",0});}throw PeerBye(p[0]==4?"Mac unpaired":"Mac disconnected");}
            if(m.type()==Type::Hello){Require(mode==Mode::Handshake,"unexpected HELLO");
                auto action=pairing.Receive(m);
                if(action==PairState::Action::Reject){if(resuming)event_({"unpair","","Stored pairing rejected; pair again explicitly","","",0});throw std::runtime_error("Mac rejected pairing or token");}
                if(action==PairState::Action::Challenge)event_({"pairing","pairing","Compare this code on both devices",Hex(peer.data(),16),"",code});
                else if(action==PairState::Action::Persist){Event e{"persist","saving","Both confirmed; storing binding",Hex(peer.data(),16),Hex(token.data(),32),code};e.host=cfg.host;e.port=cfg.port;e.session=Hex(nonce.data(),16);event_(e);}
                else if(action==PairState::Action::CommitAck)Write(fd,hello(Phase::Commit));
                else if(action==PairState::Action::Ready){authenticated=true;mode=Mode::Local;notify("local","Connected · input remains on Mac");}
                lastReceive=now;return;
            }
            Require(authenticated,"input before authentication");Require(Injector::Authorization()==AUTHORIZED,"Injection authorization revoked");
            if(m.type()==Type::Enter){Require(mode==Mode::Local&&m.epoch()!=lastEpoch,"unexpected ENTER");if(lastEpoch)Require(int32_t(m.epoch()-lastEpoch)>0,"stale epoch");epoch=lastEpoch=m.epoch();
                InjectRequire(input.Position(U16(p),U16(p+2))&&input.Modifiers(U16(p+4))&&input.Buttons(U16(p+6)),"ENTER injection failed");
                enterX=input.x();armed=false;mode=Mode::Remote;Write(fd,m);notify("remote","Receiving Mac keyboard and mouse");
            }else if(m.type()==Type::Leave){Require((mode==Mode::Remote||mode==Mode::Leaving)&&m.epoch()==epoch,"stale LEAVE");input.Release();mode=Mode::Local;notify("local","Input returned to Mac");}
            else {Require(mode==Mode::Remote&&m.epoch()==epoch,"stale or out-of-state input");
                if(m.type()==Type::Move)InjectRequire(input.Position(U16(p),U16(p+2)),"Mouse move injection failed");
                else if(m.type()==Type::Button)InjectRequire(input.Position(U16(p+2),U16(p+4))&&input.Buttons(U16(p+6)),"Mouse button injection failed");
                else if(m.type()==Type::Wheel)InjectRequire(input.Wheel(int32_t(U32(p)),int32_t(U32(p+4))),"Wheel injection failed");
                else if(m.type()==Type::Key)InjectRequire(input.Key(U16(p),U16(p+2),p[4],p[5]),"Key injection failed");
                else throw std::runtime_error("wrong input direction/type");
                if(m.type()==Type::Move||m.type()==Type::Button){if(direction==1?input.x()>=enterX+4:input.x()<=enterX-4)armed=true;bool edge=direction==1?input.x()<=0:input.x()>=input.width()-1;
                    if(armed&&edge){input.Release();mode=Mode::Leaving;leaveDeadline=now+300;Message request(Type::RequestLeave,epoch);Put16(request.data(),U16(p+(m.type()==Type::Move?0:2)));Put16(request.data()+2,U16(p+(m.type()==Type::Move?2:4)));Write(fd,request);notify("leaving");}
                }
            }lastReceive=now;
        };
        while(!stop_){uint64_t now=Now();std::deque<std::string> commands;{std::lock_guard<std::mutex> l(commandsMutex_);commands.swap(commands_);}
            for(const auto& c:commands){if(c=="disconnect"){bye(0);stop_=true;break;}if(c=="unpair"){bye(4);event_({"unpair","","Pairing removed","","",0});stop_=true;break;}
                if(c=="confirm"){Require(mode==Mode::Handshake,"not pairing");pairing.Confirm();Write(fd,hello(Phase::Confirm));notify("waiting","Waiting for Mac confirmation");}
                // Persistence completions are bound to this nonce: an old asynchronous
                // Preferences flush cannot acknowledge a newer connection's COMMIT.
                if(c=="saved:"+Hex(nonce.data(),16)){if(pairing.Saved())Write(fd,hello(Phase::Commit));}
                if(c=="saveFailed:"+Hex(nonce.data(),16)){throw std::runtime_error("Unable to save pairing; connection canceled");}
            }
            if(stop_)break;
            if(now-lastReceive>=3000)throw NetworkFailure("Heartbeat timeout");
            if(!authenticated&&now-connected>=60000)throw NetworkFailure("Pairing timed out");
            if(mode==Mode::Leaving&&now>=leaveDeadline)throw NetworkFailure("Return edge acknowledgment timeout");
            Require(Injector::Authorization()==AUTHORIZED,"Injection authorization revoked");
            if(now-lastSent>=1000){Message alive(Type::Keepalive);Put64(alive.data(),now);Write(fd,alive);lastSent=now;}
            pollfd p{fd,POLLIN,0};int ready=poll(&p,1,25);if(ready<0){if(errno==EINTR)continue;throw std::runtime_error("TCP poll failed");}
            if(ready>0){uint8_t b[4096];ssize_t n=recv(fd,b,sizeof(b),0);if(n>0)decoder.feed(b,size_t(n),handle);else if(n==0)throw std::runtime_error(decoder.partial()?"Truncated frame at EOF":"Mac connection closed");else if(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)throw std::runtime_error("TCP read failed");}
        }
        input.Release();notify("disconnected","Disconnected");
    }catch(const PeerBye& e){input.Release();notify("disconnected",e.what());}
    catch(const InjectionFailure& e){input.Release();if(!stop_){bye(3);event_({"error","disconnected",e.what(),"","",0});}else notify("disconnected");}
    catch(const NetworkFailure& e){input.Release();if(!stop_){bye(2);event_({"error","disconnected",e.what(),"","",0});}else notify("disconnected");}
    catch(const std::exception& e){input.Release();if(!stop_){bye(Injector::Authorization()==AUTHORIZED?1:3);event_({"error","disconnected",e.what(),"","",0});}else notify("disconnected");}
    catch(...){input.Release();event_({"error","disconnected","Unexpected native failure","","",0});}
    input.Release();{std::lock_guard<std::mutex> lock(fdMutex_);if(fd>=0){shutdown(fd,SHUT_RDWR);close(fd);}fd_=-1;}
}
} // namespace hop
