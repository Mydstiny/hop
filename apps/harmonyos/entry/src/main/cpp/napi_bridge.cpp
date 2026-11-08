#include <napi/native_api.h>
#include "client.h"
#include "injector.h"
#include <map>
#include <memory>
#include <unistd.h>
#include <fcntl.h>

namespace {
std::mutex gMutex;
std::mutex gControlMutex;
std::map<napi_env,napi_threadsafe_function> gListeners;
std::unique_ptr<hop::Client> gClient;
hop::Event gState{"state","disconnected","Ready to pair","","",0};
napi_value Str(napi_env env,const std::string& s){napi_value v;napi_create_string_utf8(env,s.c_str(),s.size(),&v);return v;}
napi_value Undefined(napi_env env){napi_value v;napi_get_undefined(env,&v);return v;}
napi_value EventObject(napi_env env,const hop::Event& e){napi_value obj,num;napi_create_object(env,&obj);napi_set_named_property(env,obj,"kind",Str(env,e.kind));napi_set_named_property(env,obj,"state",Str(env,e.state));napi_set_named_property(env,obj,"message",Str(env,e.message));napi_set_named_property(env,obj,"peerId",Str(env,e.peerId));napi_set_named_property(env,obj,"token",Str(env,e.token));napi_create_uint32(env,e.code,&num);napi_set_named_property(env,obj,"code",num);napi_set_named_property(env,obj,"host",Str(env,e.host));napi_set_named_property(env,obj,"session",Str(env,e.session));napi_create_int32(env,e.port,&num);napi_set_named_property(env,obj,"port",num);return obj;}
void Deliver(napi_env env,napi_value callback,void*,void* data){std::unique_ptr<hop::Event> event(static_cast<hop::Event*>(data));if(!env||!callback)return;napi_value arg=EventObject(env,*event),result;napi_call_function(env,Undefined(env),callback,1,&arg,&result);}
void Emit(hop::Event event){std::lock_guard<std::mutex> lock(gMutex);if(event.kind=="state"||event.kind=="error"||event.kind=="pairing"){gState=event;gState.token.clear();}for(auto [env,fn]:gListeners){auto* copy=new hop::Event(event);if(napi_call_threadsafe_function(fn,copy,napi_tsfn_nonblocking)!=napi_ok)delete copy;}}
std::string String(napi_env env,napi_value value){napi_valuetype type;napi_typeof(env,value,&type);hop::Require(type==napi_string,"Expected string");size_t n=0;napi_get_value_string_utf8(env,value,nullptr,0,&n);hop::Require(n<=1024,"String too long");std::string s(n+1,'\0');napi_get_value_string_utf8(env,value,s.data(),s.size(),&n);s.resize(n);hop::Require(s.find('\0')==std::string::npos,"Embedded NUL");return s;}
napi_value Property(napi_env env,napi_value obj,const char* name){napi_value v;hop::Require(napi_get_named_property(env,obj,name,&v)==napi_ok,"Missing configuration field");return v;}
int Int(napi_env env,napi_value obj,const char* name){int32_t n=0;hop::Require(napi_get_value_int32(env,Property(env,obj,name),&n)==napi_ok,"Expected integer");return n;}
napi_value Listen(napi_env env,napi_callback_info info){size_t argc=1;napi_value args[1];napi_get_cb_info(env,info,&argc,args,nullptr,nullptr);napi_valuetype type;if(argc!=1||napi_typeof(env,args[0],&type)!=napi_ok||type!=napi_function){napi_throw_type_error(env,nullptr,"listen(callback)");return nullptr;}
    napi_threadsafe_function fn=nullptr;napi_value label=Str(env,"Hop state");if(napi_create_threadsafe_function(env,args[0],nullptr,label,64,1,nullptr,nullptr,nullptr,Deliver,&fn)!=napi_ok)return nullptr;
    {std::lock_guard<std::mutex> lock(gMutex);auto old=gListeners.find(env);if(old!=gListeners.end())napi_release_threadsafe_function(old->second,napi_tsfn_abort);gListeners[env]=fn;auto* initial=new hop::Event(gState);if(napi_call_threadsafe_function(fn,initial,napi_tsfn_nonblocking)!=napi_ok)delete initial;}
    return Undefined(env);
}
napi_value Unlisten(napi_env env,napi_callback_info){std::lock_guard<std::mutex> lock(gMutex);auto i=gListeners.find(env);if(i!=gListeners.end()){napi_release_threadsafe_function(i->second,napi_tsfn_abort);gListeners.erase(i);}return Undefined(env);}
napi_value Request(napi_env env,napi_callback_info){napi_value v;napi_create_int32(env,hop::Injector::RequestAuthorization(),&v);return v;}
napi_value Authorization(napi_env env,napi_callback_info){napi_value v;napi_create_int32(env,hop::Injector::Authorization(),&v);return v;}
napi_value Connect(napi_env env,napi_callback_info info){try{size_t argc=1;napi_value args[1];napi_get_cb_info(env,info,&argc,args,nullptr,nullptr);hop::Require(argc==1,"connect(config)");hop::Config c;
    c.host=String(env,Property(env,args[0],"host"));c.uuid=String(env,Property(env,args[0],"uuid"));c.peerId=String(env,Property(env,args[0],"peerId"));c.token=String(env,Property(env,args[0],"token"));c.name=String(env,Property(env,args[0],"name"));c.port=Int(env,args[0],"port");c.width=Int(env,args[0],"width");c.height=Int(env,args[0],"height");
    std::lock_guard<std::mutex> control(gControlMutex);if(!gClient)gClient=std::make_unique<hop::Client>(Emit);gClient->Start(c);return Undefined(env);
    }catch(const std::exception& e){napi_throw_error(env,nullptr,e.what());return nullptr;}}
napi_value Command(napi_env env,napi_callback_info info){try{size_t argc=1;napi_value args[1];napi_get_cb_info(env,info,&argc,args,nullptr,nullptr);hop::Require(argc==1,"command(name)");auto command=String(env,args[0]);bool persistence=command.rfind("saved:",0)==0||command.rfind("saveFailed:",0)==0;if(persistence){auto colon=command.find(':');hop::Unhex(command.substr(colon+1),16);}hop::Require(command=="confirm"||persistence||command=="disconnect"||command=="unpair","Unknown command");std::lock_guard<std::mutex> control(gControlMutex);if(gClient)gClient->Command(command);return Undefined(env);}catch(const std::exception& e){napi_throw_error(env,nullptr,e.what());return nullptr;}}
napi_value RandomHex(napi_env env,napi_callback_info info){try{size_t argc=1;napi_value args[1];napi_get_cb_info(env,info,&argc,args,nullptr,nullptr);uint32_t n=16;if(argc)napi_get_value_uint32(env,args[0],&n);hop::Require(n==16||n==32,"random size must be16 or32");std::array<uint8_t,32> data{};int fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC);hop::Require(fd>=0,"Secure random unavailable");size_t got=0;while(got<n){ssize_t r=read(fd,data.data()+got,n-got);if(r<=0){close(fd);throw std::runtime_error("Secure random failed");}got+=size_t(r);}close(fd);return Str(env,hop::Hex(data.data(),n));}catch(const std::exception& e){napi_throw_error(env,nullptr,e.what());return nullptr;}}
napi_value Snapshot(napi_env env,napi_callback_info){std::lock_guard<std::mutex> lock(gMutex);return EventObject(env,gState);}
void Cleanup(void* data){auto env=static_cast<napi_env>(data);bool empty=false;{std::lock_guard<std::mutex> lock(gMutex);auto i=gListeners.find(env);if(i!=gListeners.end()){napi_release_threadsafe_function(i->second,napi_tsfn_abort);gListeners.erase(i);}empty=gListeners.empty();}if(empty){std::lock_guard<std::mutex> control(gControlMutex);if(gClient)gClient->Stop();}}
napi_value Init(napi_env env,napi_value exports){napi_property_descriptor props[]={
    {"listen",nullptr,Listen,nullptr,nullptr,nullptr,napi_default,nullptr},{"unlisten",nullptr,Unlisten,nullptr,nullptr,nullptr,napi_default,nullptr},
    {"requestAuthorization",nullptr,Request,nullptr,nullptr,nullptr,napi_default,nullptr},{"authorization",nullptr,Authorization,nullptr,nullptr,nullptr,napi_default,nullptr},
    {"connect",nullptr,Connect,nullptr,nullptr,nullptr,napi_default,nullptr},{"command",nullptr,Command,nullptr,nullptr,nullptr,napi_default,nullptr},
    {"randomHex",nullptr,RandomHex,nullptr,nullptr,nullptr,napi_default,nullptr},{"snapshot",nullptr,Snapshot,nullptr,nullptr,nullptr,napi_default,nullptr}};
    napi_define_properties(env,exports,sizeof(props)/sizeof(props[0]),props);napi_add_env_cleanup_hook(env,Cleanup,env);return exports;}
napi_module mod={1,0,nullptr,Init,"hop",nullptr,{0}};
}
extern "C" __attribute__((constructor)) void RegisterHopBridge(){napi_module_register(&mod);}
