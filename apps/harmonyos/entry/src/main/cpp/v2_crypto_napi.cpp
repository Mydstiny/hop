#include <napi/native_api.h>
#include <database/pasteboard/oh_pasteboard.h>
#include "hop/secure_channel_c.h"
#include "v2_receiver.h"
#include "injector.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
namespace {
std::mutex listenersMutex, controlMutex;
std::map<napi_env,napi_threadsafe_function> listeners;
std::unique_ptr<hop::V2Receiver> receiver;
std::shared_ptr<hop::V2ClipboardMailbox> clipboard;
hop::V2ReceiverEvent lastEvent{"disconnected","Ready for secure connection",""};
napi_value undefined(napi_env env){napi_value out;napi_get_undefined(env,&out);return out;}
napi_value boolean(napi_env env,bool value){napi_value out;napi_get_boolean(env,value,&out);return out;}
napi_value jsString(napi_env env,const std::string& s){napi_value out;napi_create_string_utf8(env,s.c_str(),s.size(),&out);return out;}
void deliver(napi_env env,napi_value callback,void*,void* data){
    std::unique_ptr<hop::V2ReceiverEvent> e(static_cast<hop::V2ReceiverEvent*>(data));if(!env||!callback||!e)return;
    napi_value object,result;napi_create_object(env,&object);
    napi_set_named_property(env,object,"state",jsString(env,e->state));
    napi_set_named_property(env,object,"message",jsString(env,e->message));
    napi_set_named_property(env,object,"peerFingerprint",jsString(env,e->peer_fingerprint));
    napi_set_named_property(env,object,"peerCertificatePEM",jsString(env,e->peer_certificate_pem));
    napi_set_named_property(env,object,"peerName",jsString(env,e->peer_name));
    napi_value id;napi_create_uint32(env,e->request_id,&id);napi_set_named_property(env,object,"requestId",id);
    napi_call_function(env,undefined(env),callback,1,&object,&result);
}
void emit(const hop::V2ReceiverEvent& event){
    std::lock_guard<std::mutex> lock(listenersMutex);lastEvent=event;
    for(const auto& item:listeners){auto* copy=new hop::V2ReceiverEvent(event);if(napi_call_threadsafe_function(item.second,copy,napi_tsfn_nonblocking)!=napi_ok)delete copy;}
}
struct Wipe { char* p; std::size_t n; ~Wipe(){volatile char* x=p;for(std::size_t i=0;i<n;++i)x[i]=0;} };
napi_value fail(napi_env env,const char* message){napi_throw_error(env,nullptr,message);return nullptr;}
bool string_arg(napi_env env,napi_value value,std::string& out){
    std::size_t n=0;if(napi_get_value_string_utf8(env,value,nullptr,0,&n)!=napi_ok||!n||n>65536)return false;
    std::vector<char> buffer(n+1);std::size_t copied=0;
    if(napi_get_value_string_utf8(env,value,buffer.data(),buffer.size(),&copied)!=napi_ok||copied!=n)return false;
    out.assign(buffer.data(),copied);return out.find('\0')==std::string::npos;
}
bool bytes_arg(napi_env env,napi_value value,const std::uint8_t*& data,std::size_t& n){
    bool is=false;if(napi_is_typedarray(env,value,&is)!=napi_ok||!is)return false;
    napi_typedarray_type type;void* raw=nullptr;napi_value buffer;std::size_t offset=0;
    if(napi_get_typedarray_info(env,value,&type,&n,&raw,&buffer,&offset)!=napi_ok||type!=napi_uint8_array||!raw||!n||n>65536)return false;
    data=static_cast<const std::uint8_t*>(raw);return true;
}
napi_value Generate(napi_env env,napi_callback_info){
    try {
    std::array<char,8192> cert{},key{};std::array<char,65> fingerprint{};Wipe wipe{key.data(),key.size()};
    if(hop_tls_generate_identity(cert.data(),cert.size(),key.data(),key.size(),fingerprint.data(),fingerprint.size())!=0)return fail(env,"Device identity generation failed");
    napi_value object,certValue,fpValue,buffer,array;void* bytes=nullptr;const auto n=std::strlen(key.data());
    if(napi_create_object(env,&object)!=napi_ok||napi_create_string_utf8(env,cert.data(),NAPI_AUTO_LENGTH,&certValue)!=napi_ok||
       napi_create_string_utf8(env,fingerprint.data(),NAPI_AUTO_LENGTH,&fpValue)!=napi_ok||
       napi_create_arraybuffer(env,n,&bytes,&buffer)!=napi_ok||!bytes||
       napi_create_typedarray(env,napi_uint8_array,n,buffer,0,&array)!=napi_ok)return fail(env,"Cannot allocate device identity");
    if(napi_set_named_property(env,object,"certificatePEM",certValue)!=napi_ok||
       napi_set_named_property(env,object,"fingerprint",fpValue)!=napi_ok||
       napi_set_named_property(env,object,"privateKeyPEM",array)!=napi_ok)return fail(env,"Cannot create device identity");
    std::memcpy(bytes,key.data(),n);return object;
    } catch (...) { return fail(env,"Device identity generation failed"); }
}
napi_value Fingerprint(napi_env env,napi_callback_info info){
    try {
    std::size_t argc=1;napi_value args[1];napi_get_cb_info(env,info,&argc,args,nullptr,nullptr);std::string cert;
    if(argc!=1||!string_arg(env,args[0],cert))return fail(env,"Invalid certificate");
    std::array<char,65> fp{};if(hop_tls_certificate_fingerprint(cert.data(),cert.size(),fp.data(),fp.size())!=0)return fail(env,"Invalid certificate");
    napi_value result;if(napi_create_string_utf8(env,fp.data(),64,&result)!=napi_ok)return fail(env,"Cannot return fingerprint");return result;
    } catch (...) { return fail(env,"Invalid certificate"); }
}
napi_value Matches(napi_env env,napi_callback_info info){
    try {
    std::size_t argc=2;napi_value args[2];napi_get_cb_info(env,info,&argc,args,nullptr,nullptr);std::string cert;const std::uint8_t* key=nullptr;std::size_t n=0;
    if(argc!=2||!string_arg(env,args[0],cert)||!bytes_arg(env,args[1],key,n))return fail(env,"Invalid identity");
    bool ok=hop_tls_validate_identity(cert.data(),cert.size(),reinterpret_cast<const char*>(key),n)==0;
    napi_value result;if(napi_get_boolean(env,ok,&result)!=napi_ok)return fail(env,"Cannot return validation result");return result;
    } catch (...) { return fail(env,"Invalid identity"); }
}
napi_value Listen(napi_env env,napi_callback_info info){
    try {
        std::size_t argc=1;napi_value args[1];napi_get_cb_info(env,info,&argc,args,nullptr,nullptr);napi_valuetype type;
        if(argc!=1||napi_typeof(env,args[0],&type)!=napi_ok||type!=napi_function)return fail(env,"Expected event callback");
        napi_threadsafe_function fn=nullptr;
        if(napi_create_threadsafe_function(env,args[0],nullptr,jsString(env,"Hop v2 state"),64,1,nullptr,nullptr,nullptr,deliver,&fn)!=napi_ok)return fail(env,"Cannot register event callback");
        std::lock_guard<std::mutex> lock(listenersMutex);auto old=listeners.find(env);
        if(old!=listeners.end())napi_release_threadsafe_function(old->second,napi_tsfn_abort);
        listeners[env]=fn;auto* initial=new hop::V2ReceiverEvent(lastEvent);
        if(napi_call_threadsafe_function(fn,initial,napi_tsfn_nonblocking)!=napi_ok)delete initial;
        return undefined(env);
    }catch(...){return fail(env,"Cannot register event callback");}
}
napi_value Unlisten(napi_env env,napi_callback_info){
    std::lock_guard<std::mutex> lock(listenersMutex);auto i=listeners.find(env);
    if(i!=listeners.end()){napi_release_threadsafe_function(i->second,napi_tsfn_abort);listeners.erase(i);}
    return undefined(env);
}
bool property(napi_env env,napi_value object,const char* name,napi_value& out){return napi_get_named_property(env,object,name,&out)==napi_ok;}
bool field_string(napi_env env,napi_value object,const char* name,std::string& out){napi_value v;return property(env,object,name,v)&&string_arg(env,v,out);}
bool field_uint(napi_env env,napi_value object,const char* name,std::uint32_t& out){napi_value v;return property(env,object,name,v)&&napi_get_value_uint32(env,v,&out)==napi_ok;}
bool displayInteger(napi_env env,napi_value object,const char* name,std::int32_t& out) {
    napi_value value;double number=0;
    if(!property(env,object,name,value)||napi_get_value_double(env,value,&number)!=napi_ok||!std::isfinite(number)||
       std::trunc(number)!=number||number<INT32_MIN||number>INT32_MAX)return false;
    out=static_cast<std::int32_t>(number);return true;
}
bool displayGeometry(napi_env env,napi_value config,std::vector<hop::V2Display>& displays) {
    napi_value array;bool isArray=false;std::uint32_t count=0;
    if(!property(env,config,"displays",array)||napi_is_array(env,array,&isArray)!=napi_ok||!isArray||
       napi_get_array_length(env,array,&count)!=napi_ok||!count||count>64)return false;
    for(std::uint32_t i=0;i<count;++i) {
        napi_value entry;hop::V2Display d{};std::int32_t width=0,height=0;
        if(napi_get_element(env,array,i,&entry)!=napi_ok||!displayInteger(env,entry,"displayId",d.display_id)||
           !displayInteger(env,entry,"x",d.x)||!displayInteger(env,entry,"y",d.y)||
           !displayInteger(env,entry,"width",width)||!displayInteger(env,entry,"height",height)||width<=0||height<=0)return false;
        d.width=static_cast<std::uint32_t>(width);d.height=static_cast<std::uint32_t>(height);
        displays.push_back(d);
    }
    return hop::validV2Displays(displays);
}
napi_value Offer(napi_env env,napi_callback_info info){
    try {
        std::size_t argc=1;napi_value args[1];napi_get_cb_info(env,info,&argc,args,nullptr,nullptr);
        std::string host,name,certificate;std::uint32_t port=0,request=0;
        if(argc!=1||!field_string(env,args[0],"host",host)||!field_string(env,args[0],"name",name)||
           !field_string(env,args[0],"certificatePEM",certificate)||!field_uint(env,args[0],"port",port)||!field_uint(env,args[0],"requestId",request)||!request||!port||port>65535)
            return fail(env,"Invalid pairing request");
        std::lock_guard<std::mutex> lock(controlMutex);if(!receiver)receiver=std::make_unique<hop::V2Receiver>(emit);
        if(!receiver->startOffer(host,static_cast<std::uint16_t>(port),name,certificate,request))return fail(env,"A secure connection is already active");
        return undefined(env);
    }catch(...){return fail(env,"Cannot start pairing request");}
}
napi_value Connect(napi_env env,napi_callback_info info){
    try {
        std::size_t argc=1;napi_value args[1],value;napi_get_cb_info(env,info,&argc,args,nullptr,nullptr);if(argc!=1)return fail(env,"Expected secure connection configuration");
        hop::V2ReceiverConfig c;std::uint32_t port=0,request=0;bool pairing=false;
        if(!field_string(env,args[0],"host",c.host)||!field_string(env,args[0],"certificatePEM",c.certificate_pem)||
           !field_string(env,args[0],"peerCertificatePEM",c.verified_peer_certificate_pem)||!field_uint(env,args[0],"port",port)||
           !displayGeometry(env,args[0],c.displays)||!field_uint(env,args[0],"requestId",request)||!request||
           !property(env,args[0],"pairing",value)||napi_get_value_bool(env,value,&pairing)!=napi_ok||
           !port||port>65535)return fail(env,"Invalid secure connection configuration");
        const std::uint8_t* key=nullptr;std::size_t n=0;
        if(!property(env,args[0],"privateKeyPEM",value)||!bytes_arg(env,value,key,n)||n>1024)return fail(env,"Invalid private key");
        c.private_key_pem.assign(reinterpret_cast<const char*>(key),n);
        volatile std::uint8_t* clearKey=const_cast<std::uint8_t*>(key);for(std::size_t i=0;i<n;++i)clearKey[i]=0;
        c.port=static_cast<std::uint16_t>(port);c.request_id=request;c.require_pairing=pairing;
        if(hop::Injector::Authorization()!=AUTHORIZED)return fail(env,"Input injection permission is not granted");
        std::lock_guard<std::mutex> lock(controlMutex);
        if(!receiver)receiver=std::make_unique<hop::V2Receiver>(emit);
        bool allowClipboard=false;
        if(property(env,args[0],"clipboard",value))napi_get_value_bool(env,value,&allowClipboard);
        if(allowClipboard) {
            auto board=std::shared_ptr<OH_Pasteboard>(OH_Pasteboard_Create(),OH_Pasteboard_Destroy);
            if(!board)return fail(env,"System clipboard is unavailable");
            c.clipboard=std::make_shared<hop::V2ClipboardMailbox>(request);
            c.sample_clipboard=[board]{return OH_Pasteboard_GetChangeCount(board.get());};
        }
        auto configuredClipboard=c.clipboard;
        if(!receiver->start(std::move(c)))return fail(env,"A secure connection is already active");
        clipboard=std::move(configuredClipboard);
        return undefined(env);
    }catch(...){return fail(env,"Cannot start secure connection");}
}
std::shared_ptr<hop::V2ClipboardMailbox> clipboardFor(napi_env env,napi_value request) {
    std::uint32_t id=0;if(napi_get_value_uint32(env,request,&id)!=napi_ok||!id)return {};
    std::lock_guard<std::mutex> lock(controlMutex);
    return clipboard && clipboard->request()==id ? clipboard : nullptr;
}
std::uint64_t clipboardEpoch(napi_env env,napi_value arg) {
    std::string value;if(!string_arg(env,arg,value)||value.size()>20)return 0;
    std::uint64_t epoch=0;
    for(char c:value){if(c<'0'||c>'9'||epoch>(UINT64_MAX-static_cast<unsigned>(c-'0'))/10)return 0;epoch=epoch*10+static_cast<unsigned>(c-'0');}
    return epoch;
}
napi_value ClipboardStatus(napi_env env,napi_callback_info info) {
    try {
        std::size_t argc=1;napi_value a[1];napi_get_cb_info(env,info,&argc,a,nullptr,nullptr);
        auto clip=argc==1?clipboardFor(env,a[0]):nullptr;if(!clip)return undefined(env);
        auto s=clip->state();napi_value object,value;napi_create_object(env,&object);
        napi_set_named_property(env,object,"epoch",jsString(env,std::to_string(s.epoch)));
        napi_set_named_property(env,object,"active",boolean(env,s.active));
        napi_set_named_property(env,object,"poll",boolean(env,s.poll));
        napi_set_named_property(env,object,"draining",boolean(env,s.draining));
        napi_create_uint32(env,s.baseline,&value);napi_set_named_property(env,object,"baseline",value);
        napi_create_uint32(env,static_cast<std::uint32_t>(s.formats),&value);napi_set_named_property(env,object,"formats",value);
        return object;
    }catch(...){return undefined(env);}
}
napi_value ClipboardAction(napi_env env,napi_callback_info info) {
    try {
        std::size_t argc=3;napi_value a[3];napi_get_cb_info(env,info,&argc,a,nullptr,nullptr);
        auto clip=argc==3?clipboardFor(env,a[0]):nullptr;std::string action;
        const auto epoch=clip?clipboardEpoch(env,a[1]):0;
        if(!epoch||!string_arg(env,a[2],action))return boolean(env,false);
        if(action=="activate")return boolean(env,clip->activate(epoch));
        if(action=="permitted")return boolean(env,clip->permitted(epoch));
        if(action=="finish"){clip->finishIncoming(epoch);return boolean(env,true);}
        if(action=="drained")return boolean(env,clip->acknowledge(epoch));
        if(action=="pause"){clip->pause(epoch);return boolean(env,true);}
        return boolean(env,false);
    }catch(...){return boolean(env,false);}
}
napi_value TakeClipboard(napi_env env,napi_callback_info info) {
    try {
        std::size_t argc=2;napi_value a[2];napi_get_cb_info(env,info,&argc,a,nullptr,nullptr);
        auto clip=argc==2?clipboardFor(env,a[0]):nullptr;const auto epoch=clip?clipboardEpoch(env,a[1]):0;
        if(!epoch)return undefined(env);
        auto packet=clip->takeIncoming(epoch);if(!packet)return undefined(env);
        napi_value object,buffer,array,value;void* bytes=nullptr;
        if(napi_create_object(env,&object)!=napi_ok||
           napi_create_arraybuffer(env,packet->bytes.size(),&bytes,&buffer)!=napi_ok||
           napi_create_typedarray(env,napi_uint8_array,packet->bytes.size(),buffer,0,&array)!=napi_ok) {
            clip->finishIncoming(epoch);clip->pause(epoch);return undefined(env);
        }
        if(!packet->bytes.empty())std::memcpy(bytes,packet->bytes.data(),packet->bytes.size());
        napi_set_named_property(env,object,"bytes",array);
        napi_create_uint32(env,static_cast<std::uint32_t>(packet->format),&value);napi_set_named_property(env,object,"format",value);
        napi_create_uint32(env,packet->baseline,&value);napi_set_named_property(env,object,"baseline",value);
        return object;
    }catch(...){return undefined(env);}
}
napi_value SubmitClipboard(napi_env env,napi_callback_info info) {
    try {
        std::size_t argc=4;napi_value a[4];napi_get_cb_info(env,info,&argc,a,nullptr,nullptr);
        auto clip=argc==4?clipboardFor(env,a[0]):nullptr;const auto epoch=clip?clipboardEpoch(env,a[1]):0;
        std::uint32_t format=0;std::size_t count=0,offset=0;void* raw=nullptr;napi_value array;
        napi_typedarray_type type;bool isArray=false;
        if(!epoch||napi_get_value_uint32(env,a[2],&format)!=napi_ok||(format!=1&&format!=2)||
           napi_is_typedarray(env,a[3],&isArray)!=napi_ok||!isArray||
           napi_get_typedarray_info(env,a[3],&type,&count,&raw,&array,&offset)!=napi_ok||type!=napi_uint8_array||
           (count&&!raw)||count>(format==1?hop::ClipboardAssembler::max_text:hop::ClipboardAssembler::max_png)||
           (!count&&format==2)||!clip->permitted(epoch))return boolean(env,false);
        hop::V2ClipboardMailbox::Packet packet;packet.epoch=epoch;packet.format=static_cast<hop::ClipboardFormat>(format);
        if(count){auto p=static_cast<const std::uint8_t*>(raw);packet.bytes.assign(p,p+count);}
        return boolean(env,clip->submit(std::move(packet)));
    }catch(...){return boolean(env,false);}
}
napi_value Command(napi_env env,napi_callback_info info){
    try {
        std::size_t argc=1;napi_value args[1];napi_get_cb_info(env,info,&argc,args,nullptr,nullptr);std::string command;
        if(argc!=1||!string_arg(env,args[0],command))return fail(env,"Invalid secure command");
        std::lock_guard<std::mutex> lock(controlMutex);if(!receiver)return fail(env,"No secure connection");
        if(command=="confirm")receiver->confirmPairing();else if(command=="saved")receiver->confirmStorage();else if(command=="cancel")receiver->cancelPairing();
        else if(command=="disconnect")receiver->stop();else return fail(env,"Unknown secure command");
        return undefined(env);
    }catch(...){return fail(env,"Secure command failed");}
}
void Cleanup(void* context){
    auto env=static_cast<napi_env>(context);bool empty=false;
    {std::lock_guard<std::mutex> lock(listenersMutex);auto i=listeners.find(env);if(i!=listeners.end()){napi_release_threadsafe_function(i->second,napi_tsfn_abort);listeners.erase(i);}empty=listeners.empty();}
    if(empty){std::lock_guard<std::mutex> lock(controlMutex);if(receiver)receiver->stop();}
}
napi_value Init(napi_env env,napi_value exports){
    napi_property_descriptor methods[]={
        {"generateIdentity",nullptr,Generate,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"fingerprint",nullptr,Fingerprint,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"identityMatches",nullptr,Matches,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"listen",nullptr,Listen,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"unlisten",nullptr,Unlisten,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"offer",nullptr,Offer,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"connect",nullptr,Connect,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"clipboardStatus",nullptr,ClipboardStatus,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"clipboardAction",nullptr,ClipboardAction,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"takeClipboard",nullptr,TakeClipboard,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"submitClipboard",nullptr,SubmitClipboard,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"command",nullptr,Command,nullptr,nullptr,nullptr,napi_default,nullptr}};
    napi_define_properties(env,exports,sizeof(methods)/sizeof(methods[0]),methods);napi_add_env_cleanup_hook(env,Cleanup,env);return exports;
}
napi_module module={1,0,nullptr,Init,"hopv2",nullptr,{0}};
}
extern "C" __attribute__((constructor)) void RegisterHopV2Crypto(){napi_module_register(&module);}
