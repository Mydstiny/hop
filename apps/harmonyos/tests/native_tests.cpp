#include "protocol.h"
#include "pair_state.h"
#include "injector.h"
#include "client.h"
#include "generated/hid_keymap.h"
#include <fstream>
#include <iostream>
#include <chrono>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <functional>
#include <atomic>
using namespace hop;
static int checks=0;
static void Check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
static void Reject(std::function<void()> fn){bool rejected=false;try{fn();}catch(const std::exception&){rejected=true;}Check(rejected,"expected rejection");}
static Message ServerHello(Phase phase){Message m(Type::Hello);m.bytes[2]=1;auto*p=m.data();p[0]=uint8_t(phase);p[1]=1;p[2]=1;Put32(p+8,1920);Put32(p+12,1080);std::fill_n(p+16,16,0x11);std::fill_n(p+32,16,0x22);p[80]='M';
 if(phase==Phase::Challenge||phase==Phase::Commit||phase==Phase::Ready)Put32(p+4,123456);
 if(phase==Phase::Commit||phase==Phase::Ready)std::fill_n(p+48,32,0x33);return m;}
static PairState Fresh(){PairState p;p.nonce.fill(0x22);return p;}
static void PairTests(){
 auto p=Fresh();auto challenge=ServerHello(Phase::Challenge),commit=ServerHello(Phase::Commit),ready=ServerHello(Phase::Ready);
 Check(p.Receive(challenge)==PairState::Action::Challenge,"challenge");Check(Zero(p.token.data(),32),"token leaked before confirmation");
 Reject([&]{p.Receive(commit);});Reject([&]{p.Saved();});Reject([&]{p.Receive(ready);});
 p.Confirm();Check(p.Receive(commit)==PairState::Action::Persist,"commit persistence");Check(p.Receive(commit)==PairState::Action::None,"no duplicate persistence");Reject([&]{p.Receive(ready);});
 auto changed=commit;changed.data()[48]^=1;Reject([&]{p.Receive(changed);});
 Check(p.Saved()&&!p.Saved(),"persistence acknowledgment idempotent");Check(p.Receive(commit)==PairState::Action::CommitAck,"idempotent commit");Check(p.Receive(ready)==PairState::Action::Ready,"READY retains pair code");Reject([&]{p.Receive(ready);});
 auto q=Fresh();q.Receive(challenge);auto changedNonce=challenge;changedNonce.data()[32]^=1;Reject([&]{q.Receive(changedNonce);});
 auto changedCode=challenge;Put32(changedCode.data()+4,123457);Reject([&]{q.Receive(changedCode);});
 auto changedDirection=challenge;changedDirection.data()[2]=0;Reject([&]{q.Receive(changedDirection);});
 auto resume=Fresh();resume.resuming=true;resume.peer.fill(0x11);resume.token.fill(0x33);Reject([&]{resume.Receive(challenge);});
 auto bad=ready;bad.data()[48]^=1;Reject([&]{resume.Receive(bad);});bad=ready;bad.data()[16]^=1;Reject([&]{resume.Receive(bad);});
 Check(resume.Receive(ready)==PairState::Action::Ready,"resume");auto rejected=Fresh();rejected.resuming=true;Check(rejected.Receive(ServerHello(Phase::Reject))==PairState::Action::Reject,"reject must not downgrade");
 auto role=challenge;role.bytes[2]=0;Reject([&]{role.validate();});
 auto invalid=challenge;invalid.data()[0]=uint8_t(Phase::Pair);Put32(invalid.data()+4,0);Reject([&]{invalid.validate();});
}
static void CodecTests(const char* path){
 std::ifstream in(path);Check(bool(in),"open golden data");std::string hex;std::vector<uint8_t> all;int vectors=0;
 while(in>>hex){auto bytes=Unhex(hex,kFrameSize);Decoder d;int accepted=0;for(auto b:bytes)d.feed(&b,1,[&](const Message&m){++accepted;Check(Hex(m.frame().data(),kFrameSize)==hex,"golden roundtrip");});Check(accepted==1&&!d.partial(),"fragment decoder");all.insert(all.end(),bytes.begin(),bytes.end());++vectors;
  auto mutation=bytes;mutation[4]=2;Reject([&]{Decoder bad;bad.feed(mutation.data(),mutation.size(),[](auto&){});});
  mutation=bytes;mutation[131]=1;Reject([&]{Decoder bad;bad.feed(mutation.data(),mutation.size(),[](auto&){});});
 }
 Check(vectors>=12,"12 logical golden vectors");Decoder d;int accepted=0;d.feed(all.data(),all.size(),[&](auto&){++accepted;});Check(accepted==vectors,"coalesced decoder");
 for(uint32_t len:{0u,127u,129u,0xffffffffu}){uint8_t prefix[4];Put32(prefix,len);Reject([&]{Decoder bad;bad.feed(prefix,4,[](auto&){});});}
 Decoder partial;partial.feed(all.data(),37,[](auto&){});Check(partial.partial(),"truncated EOF remains partial");
}
static void InjectorTests(){
 Check(MapHidKey(0x04)==MapMacKey(0),"v2 HID A shares canonical target");
 Check(MapHidKey(0x06)==MapMacKey(8),"v2 HID C shares canonical target");
 Check(MapHidKey(0xe3)==MapMacKey(55)&&MapHidKey(0xe7)==MapMacKey(54),"v2 left and right Command stay distinct");
 Check(MapHidKey(0x39)==MapMacKey(57),"v2 Caps Lock matches canonical target");
 Check(MapHidKey(0)==-1&&MapHidKey(0xff)==-1,"unknown HID usage is not guessed");
 fake::Reset();Injector i;i.Dimensions(1001,501);Check(i.Position(32768,65535)&&i.x()==500&&i.y()==500,"normalized round");
 Check(i.Modifiers(1|64|256),"modifier repair");Check(i.Buttons(1|2|8),"held button state");Check(i.Position(40000,20000),"drag move");
 Check(i.Key(0,1|64|256,true,false)&&i.Key(0,1|64|256,true,true),"key repeat");Check(i.Wheel(120,-240),"axis sequence");
 {std::lock_guard<std::mutex> l(fake::mutex);Check(fake::buttons.count(MOUSE_BUTTON_LEFT)&&fake::buttons.count(MOUSE_BUTTON_RIGHT)&&fake::buttons.count(MOUSE_BUTTON_BACK),"explicit button mapping");
 int begin=0,update=0,end=0;for(const auto&m:fake::mouse){begin+=m.action==4;update+=m.action==5;end+=m.action==6;if(m.action==4||m.action==5){Check(m.value!=0,"scroll begin/update carry axis data");Check(m.axis==0?m.value==45.0f:m.value==22.5f,"scroll axis scale and sign");}if(m.action==6)Check(m.value==0,"scroll end has zero axis data");}Check(begin==2&&update==2&&end==2,"complete wheel phases");}
 i.Release();Check(fake::Empty(),"release actual pressed set");Check(fake::caps==0,"restore caps lock");i.Release();Check(fake::created==fake::destroyed,"destroy SDK objects");
 fake::failNext=true;Check(!i.Position(0,0),"injection errors propagated");Check(fake::created==fake::destroyed,"destroy SDK object on failure");
 fake::failMouseAction=MOUSE_ACTION_AXIS_UPDATE;Check(!i.Wheel(0,120),"axis update failure propagated");Check(fake::mouse.back().action==MOUSE_ACTION_AXIS_END,"end axis even on failed update");
 fake::failMouseAction=MOUSE_ACTION_AXIS_END;Check(!i.Wheel(0,120),"axis end failure propagated");auto before=fake::mouse.size();i.Release();Check(fake::mouse.size()==before+1&&fake::mouse.back().action==MOUSE_ACTION_AXIS_END,"release retries incomplete axis end");
 fake::failNext=true;Check(!i.Key(0,0,true,false),"key down failure tracked");i.Release();Check(fake::Empty(),"failed down release attempt safe");Check(fake::created==fake::destroyed,"all error SDK objects destroyed");
 fake::Reset();fake::failCapsQuery=true;Injector noPhysicalKeyboard;
 Check(noPhysicalKeyboard.Key(57,256,true,false)&&fake::caps==1,"caps toggle works when state query cannot find a keyboard");
 Check(noPhysicalKeyboard.Key(0,256,true,false)&&noPhysicalKeyboard.Key(0,256,false,false)&&fake::caps==1,"ordinary key does not retoggle caps");
 Check(noPhysicalKeyboard.Key(57,0,false,false)&&fake::caps==0,"second caps transition toggles off without a state query");
 noPhysicalKeyboard.Release();Check(fake::Empty()&&fake::caps==0,"unqueryable caps does not invent a restoration toggle");
 fake::Reset();fake::failCapsQuery=true;Injector hid;
 Check(hid.KeyHid(0x39,256,true,false)&&fake::caps==1,"v2 HID caps toggles once on down");
 Check(hid.KeyHid(0x39,256,false,false)&&fake::caps==1,"v2 HID caps release does not retoggle");
 Check(hid.KeyHid(0x04,256,true,false)&&hid.KeyHid(0x04,256,false,false),"v2 HID A down/up");
 Check(hid.KeyHid(0x39,0,true,false)&&hid.KeyHid(0x39,0,false,false)&&fake::caps==0,"v2 second caps tap restores");
 hid.Release();Check(fake::Empty(),"v2 HID releases injected keys");
 fake::Reset();Injector retry;Check(retry.KeyHid(0x04,0,true,false)&&retry.Buttons(1),"v2 release retry setup");
 fake::failNext=true;fake::failMouseAction=MOUSE_ACTION_BUTTON_UP;
 Check(!retry.Release(),"failed releases are reported");
 Check(!fake::Empty(),"failed release remains tracked");
 Check(retry.Release()&&fake::Empty(),"failed key and button releases retry");
}
static void Send(int fd,const Message& m,bool fragments=false){auto f=m.frame();size_t n=0;while(n<f.size()){ssize_t count=send(fd,f.data()+n,fragments?1:f.size()-n,0);Check(count>0,"server write");n+=size_t(count);}}
static Message Receive(int fd){std::array<uint8_t,kFrameSize>b{};size_t n=0;while(n<b.size()){ssize_t count=recv(fd,b.data()+n,b.size()-n,0);Check(count>0,"server read");n+=size_t(count);}Check(U32(b.data())==128,"server framing");Message m(Type::Hello);std::copy(b.begin()+4,b.end(),m.bytes.begin());m.validate();return m;}
static Message ReceiveNonHeartbeat(int fd){for(int i=0;i<10;++i){auto m=Receive(fd);if(m.type()!=Type::Keepalive)return m;}throw std::runtime_error("too many heartbeats");}
static void WithClient(bool resume,const std::function<void(int,Client&,std::atomic<int>&)>& run){
 fake::Reset();int listener=socket(AF_INET,SOCK_STREAM,0);Check(listener>=0,"listen socket");sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);Check(bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0&&listen(listener,1)==0,"listen localhost");socklen_t len=sizeof(address);getsockname(listener,reinterpret_cast<sockaddr*>(&address),&len);
 std::atomic<int> persisted{0};Client* active=nullptr;Client client([&](Event e){if(e.kind=="pairing")active->Command("confirm");if(e.kind=="persist"){++persisted;active->Command("saved:"+std::string(32,'0'));active->Command("saved:"+e.session);active->Command("saved:"+e.session);}});active=&client;
 Config cfg;cfg.host="127.0.0.1";cfg.port=ntohs(address.sin_port);cfg.width=1001;cfg.height=501;cfg.uuid=std::string(32,'4');if(resume){cfg.peerId=std::string(32,'1');cfg.token=std::string(64,'3');}client.Start(cfg);
 int fd=accept(listener,nullptr,nullptr);Check(fd>=0,"accept client");timeval timeout{5,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
 try{run(fd,client,persisted);}catch(...){client.Stop();close(fd);close(listener);throw;}
 client.Stop();close(fd);close(listener);Check(fake::created==fake::destroyed,"client SDK allocations balanced");
}
static void Authenticate(int fd,bool resume){auto request=ReceiveNonHeartbeat(fd);Check(request.type()==Type::Hello&&request.data()[0]==uint8_t(resume?Phase::Resume:Phase::Pair),"initial client HELLO");
 auto hello=[&](Phase phase){auto m=ServerHello(phase);std::copy_n(request.data()+32,16,m.data()+32);return m;};
 if(!resume){Send(fd,hello(Phase::Challenge),true);auto confirm=ReceiveNonHeartbeat(fd);Check(confirm.data()[0]==3&&Zero(confirm.data()+48,32),"confirmed before token");Send(fd,hello(Phase::Commit));auto committed=ReceiveNonHeartbeat(fd);Check(committed.data()[0]==5&&ConstantEqual(committed.data()+48,hello(Phase::Commit).data()+48,32),"commit acknowledged after persistence");}
 Send(fd,hello(Phase::Ready));
}
static Message Enter(uint32_t epoch=1){Message m(Type::Enter,epoch);Put16(m.data(),131);Put16(m.data()+2,30000);Put16(m.data()+4,1);Put16(m.data()+6,1);return m;}
static void ClientTests(){
 WithClient(false,[](int fd,Client&,std::atomic<int>&p){Authenticate(fd,false);Send(fd,Enter());Check(ReceiveNonHeartbeat(fd).type()==Type::Enter,"ENTER ack");Check(p==1,"one durable pair commit");
 Message key(Type::Key,1);Put16(key.data(),0);Put16(key.data()+2,1);key.data()[4]=1;Send(fd,key);
 Message leave(Type::Leave,1);Send(fd,leave);Message bye(Type::Bye);Send(fd,bye);});Check(fake::Empty(),"client normal leave releases");
 WithClient(true,[](int fd,Client&,std::atomic<int>&){ReceiveNonHeartbeat(fd);Send(fd,Enter());Check(ReceiveNonHeartbeat(fd).type()==Type::Bye,"unauthenticated input rejected");});Check(fake::mouse.empty()&&fake::key.empty(),"no unauthenticated injection");
 WithClient(true,[](int fd,Client&,std::atomic<int>&){Authenticate(fd,true);Send(fd,Enter());ReceiveNonHeartbeat(fd);Message leave(Type::Leave,1);Send(fd,leave);Send(fd,Enter());Check(ReceiveNonHeartbeat(fd).type()==Type::Bye,"stale epoch rejected");});Check(fake::Empty(),"stale epoch failure releases");
 WithClient(true,[](int fd,Client&,std::atomic<int>&){Authenticate(fd,true);Send(fd,Enter());ReceiveNonHeartbeat(fd);Message move(Type::Move,1);Put16(move.data(),524);Send(fd,move);Put16(move.data(),0);Send(fd,move);auto request=ReceiveNonHeartbeat(fd);Check(request.type()==Type::RequestLeave,"return edge request");Check(fake::Empty(),"release before return request");std::this_thread::sleep_for(std::chrono::milliseconds(400));Send(fd,Enter(2));Check(ReceiveNonHeartbeat(fd).type()==Type::Enter,"leave timeout stays connected and local");});
 WithClient(true,[](int fd,Client&,std::atomic<int>&){Authenticate(fd,true);Send(fd,Enter());ReceiveNonHeartbeat(fd);auto begin=std::chrono::steady_clock::now();Check(ReceiveNonHeartbeat(fd).type()==Type::Bye,"3s heartbeat deadline");Check(std::chrono::steady_clock::now()-begin<std::chrono::seconds(4),"heartbeat bounded");});Check(fake::Empty(),"timeout releases held state");
 WithClient(true,[](int fd,Client&,std::atomic<int>&){Authenticate(fd,true);Send(fd,Enter());ReceiveNonHeartbeat(fd);{std::lock_guard<std::mutex> l(fake::mutex);fake::failNext=true;}Message move(Type::Move,1);Send(fd,move);auto bye=ReceiveNonHeartbeat(fd);Check(bye.type()==Type::Bye&&bye.data()[0]==3,"injection failure BYE3");});Check(fake::Empty(),"injection error releases held state");
 WithClient(true,[](int fd,Client&,std::atomic<int>&){Authenticate(fd,true);Send(fd,Enter());ReceiveNonHeartbeat(fd);fake::authorization=UNAUTHORIZED;auto bye=ReceiveNonHeartbeat(fd);Check(bye.type()==Type::Bye&&bye.data()[0]==3,"revocation BYE3");});Check(fake::releaseAttempts>=2,"revocation attempts actual releases");
}
int main(int argc,char**argv){try{Require(argc==2,"pass golden hex file");CodecTests(argv[1]);PairTests();InjectorTests();ClientTests();std::cout<<"PASS HarmonyOS native codec / pair-state / fake-SDK Injector+Client: "<<checks<<" checks\n";return 0;}catch(const std::exception&e){std::cerr<<"FAIL: "<<e.what()<<" (after "<<checks<<" checks)\n";return 1;}}
