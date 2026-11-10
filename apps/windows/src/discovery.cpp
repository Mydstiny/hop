#ifndef UNICODE
#define UNICODE
#endif
#include "hop_windows/discovery.h"
#include <winsock2.h>
#include <windows.h>
#include <windns.h>
#include <algorithm>
#include <cwchar>
#include <map>
#include <mutex>
#include <utility>

namespace hop::windows {
namespace discovery_detail {
constexpr wchar_t service[]=L"_hop2._tcp.local";
struct DiscoveryState {
    std::mutex mutex;bool active=true;std::uint64_t next_ticket=0;std::map<std::wstring,DiscoveryCandidate> candidates;std::map<std::wstring,std::uint64_t> expiry,resolving;
};
struct BrowseContext {std::shared_ptr<DiscoveryState> state;DNS_SERVICE_CANCEL cancel{};};
struct ResolveContext {std::shared_ptr<DiscoveryState> state;std::wstring name;std::uint64_t ticket=0;DNS_SERVICE_CANCEL cancel{};};
struct AdvertContext {
    std::mutex mutex;PDNS_SERVICE_INSTANCE instance=nullptr;DNS_SERVICE_REGISTER_REQUEST request{};DNS_SERVICE_CANCEL cancel{};
    bool stopped=false,registered=false,deregistering=false;
    ~AdvertContext(){if(instance)DnsServiceFreeInstance(instance);}
};
// Registry lookup makes a late callback safe even after cancellation. Entries
// remain until Windows sends the final completion, never until UI destruction.
std::mutex registry_mutex;
std::map<BrowseContext*,std::shared_ptr<BrowseContext>> browsers;
std::map<ResolveContext*,std::shared_ptr<ResolveContext>> resolves;
std::map<AdvertContext*,std::shared_ptr<AdvertContext>> adverts;
std::wstring bounded_name(const wchar_t* value) {
    if(!value)return {};std::size_t n=0;while(n<256 && value[n])++n;
    if(!n || n==256)return {};return std::wstring(value,n);
}
bool valid_instance(const std::wstring& name) {
    const std::wstring suffix=L"._hop2._tcp.local";
    return name.size()>suffix.size() && !_wcsicmp(name.c_str()+name.size()-suffix.size(),suffix.c_str());
}
void WINAPI resolved(DWORD status,PVOID opaque,PDNS_SERVICE_INSTANCE instance) {
    std::shared_ptr<ResolveContext> ctx;
    {std::lock_guard<std::mutex> lock(registry_mutex);auto it=resolves.find(static_cast<ResolveContext*>(opaque));if(it!=resolves.end()){ctx=it->second;resolves.erase(it);}}
    try {
        if(ctx) {
            DiscoveryCandidate result;bool v2=false;
            if(status==ERROR_SUCCESS && instance && instance->ip4Address && instance->wPort && instance->dwPropertyCount<=64) {
                for(DWORD i=0;i<instance->dwPropertyCount;++i)if(instance->keys && instance->values && bounded_name(instance->keys[i])==L"v" && bounded_name(instance->values[i])==L"2")v2=true;
                const auto* ip=reinterpret_cast<const unsigned char*>(instance->ip4Address);
                if(v2 && ip[0] && ip[0]<224 && !(ip[0]==255 && ip[1]==255 && ip[2]==255 && ip[3]==255)) {
                    wchar_t text[32]{};swprintf(text,32,L"%u.%u.%u.%u",unsigned(ip[0]),unsigned(ip[1]),unsigned(ip[2]),unsigned(ip[3]));
                    result={ctx->name,text,instance->wPort};
                }
            }
            std::lock_guard<std::mutex> lock(ctx->state->mutex);auto pending=ctx->state->resolving.find(ctx->name);
            if(pending!=ctx->state->resolving.end() && pending->second==ctx->ticket) {
                ctx->state->resolving.erase(pending);
                if(ctx->state->active && !result.address.empty()){ctx->state->candidates[ctx->name]=std::move(result);ctx->state->expiry[ctx->name]=GetTickCount64()+120000;}
            }
        }
    } catch(...) {}
    if(instance)DnsServiceFreeInstance(instance);
}
void resolve(const std::shared_ptr<DiscoveryState>& state,const std::wstring& name) {
    auto ctx=std::make_shared<ResolveContext>();ctx->state=state;ctx->name=name;
    {std::lock_guard<std::mutex> lock(state->mutex);if(!state->active || state->resolving.size()>=32 || state->candidates.size()>=64 || state->resolving.count(name) || state->next_ticket==UINT64_MAX)return;
     ctx->ticket=++state->next_ticket;state->resolving[name]=ctx->ticket;}
    bool registered=false;
    {std::lock_guard<std::mutex> lock(registry_mutex);if(resolves.size()<32){resolves[ctx.get()]=ctx;registered=true;}}
    if(!registered){std::lock_guard<std::mutex> lock(state->mutex);auto pending=state->resolving.find(name);if(pending!=state->resolving.end() && pending->second==ctx->ticket)state->resolving.erase(pending);return;}
    DNS_SERVICE_RESOLVE_REQUEST req{};req.Version=DNS_QUERY_REQUEST_VERSION1;req.QueryName=ctx->name.data();req.pResolveCompletionCallback=resolved;req.pQueryContext=ctx.get();
    if(DnsServiceResolve(&req,&ctx->cancel)!=DNS_REQUEST_PENDING) {
        {std::lock_guard<std::mutex> lock(registry_mutex);resolves.erase(ctx.get());}
        std::lock_guard<std::mutex> lock(state->mutex);auto pending=state->resolving.find(name);if(pending!=state->resolving.end() && pending->second==ctx->ticket)state->resolving.erase(pending);
    }
}
void WINAPI browsed(DWORD status,PVOID opaque,PDNS_RECORD records) {
    std::shared_ptr<BrowseContext> ctx;
    {std::lock_guard<std::mutex> lock(registry_mutex);auto it=browsers.find(static_cast<BrowseContext*>(opaque));if(it!=browsers.end()){ctx=it->second;if(status==ERROR_CANCELLED)browsers.erase(it);}}
    try {
        if(ctx && status==ERROR_SUCCESS)for(auto* record=records;record;record=record->pNext)if(record->wType==DNS_TYPE_PTR) {
            auto name=bounded_name(record->Data.PTR.pNameHost);if(!valid_instance(name))continue;
            if(!record->dwTtl){std::lock_guard<std::mutex> lock(ctx->state->mutex);ctx->state->candidates.erase(name);ctx->state->expiry.erase(name);ctx->state->resolving.erase(name);}
            else resolve(ctx->state,name);
        }
    } catch(...) {}
    if(records)DnsRecordListFree(records,DnsFreeRecordList);
}
void deregister(const std::shared_ptr<AdvertContext>& ctx) {
    if(DnsServiceDeRegister(&ctx->request,nullptr)!=DNS_REQUEST_PENDING){std::lock_guard<std::mutex> lock(registry_mutex);adverts.erase(ctx.get());}
}
void WINAPI advertised(DWORD status,PVOID opaque,PDNS_SERVICE_INSTANCE instance) {
    std::shared_ptr<AdvertContext> ctx;
    {std::lock_guard<std::mutex> lock(registry_mutex);auto it=adverts.find(static_cast<AdvertContext*>(opaque));if(it!=adverts.end())ctx=it->second;}
    bool remove=false,remove_service=false;
    if(ctx) {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        if(ctx->deregistering || status!=ERROR_SUCCESS)remove=true;
        else {ctx->registered=true;if(ctx->stopped){ctx->deregistering=true;remove_service=true;}}
    }
    if(instance)DnsServiceFreeInstance(instance);
    if(remove){std::lock_guard<std::mutex> lock(registry_mutex);adverts.erase(ctx.get());}
    if(remove_service)deregister(ctx);
}
}
using namespace discovery_detail;
struct Discovery::Impl {std::shared_ptr<BrowseContext> browse;std::shared_ptr<AdvertContext> advert;};
Discovery::Discovery():impl_(std::make_unique<Impl>()){}
Discovery::~Discovery(){stop();stop_advertising();}
bool Discovery::advertise(std::uint16_t port) {
    if(!port || impl_->advert)return false;
    wchar_t host[256]{};DWORD size=256;if(!GetComputerNameExW(ComputerNameDnsHostname,host,&size) || !size)return false;
    const std::wstring host_name=std::wstring(host)+L".local";
    const std::wstring instance_name=L"Hop-Windows-"+std::to_wstring(GetCurrentProcessId())+L"._hop2._tcp.local";
    PCWSTR keys[]={L"v"},values[]={L"2"};auto ctx=std::make_shared<AdvertContext>();
    ctx->instance=DnsServiceConstructInstance(instance_name.c_str(),host_name.c_str(),nullptr,nullptr,port,0,0,1,keys,values);
    if(!ctx->instance)return false;
    ctx->request.Version=DNS_QUERY_REQUEST_VERSION1;ctx->request.pServiceInstance=ctx->instance;ctx->request.pRegisterCompletionCallback=advertised;ctx->request.pQueryContext=ctx.get();
    {std::lock_guard<std::mutex> lock(registry_mutex);adverts[ctx.get()]=ctx;}
    if(DnsServiceRegister(&ctx->request,&ctx->cancel)!=DNS_REQUEST_PENDING){std::lock_guard<std::mutex> lock(registry_mutex);adverts.erase(ctx.get());return false;}
    impl_->advert=std::move(ctx);return true;
}
void Discovery::stop_advertising() {
    auto ctx=std::move(impl_->advert);if(!ctx)return;bool remove_service=false;
    {std::lock_guard<std::mutex> lock(ctx->mutex);ctx->stopped=true;if(ctx->registered && !ctx->deregistering){ctx->deregistering=true;remove_service=true;}}
    if(remove_service)deregister(ctx);else DnsServiceRegisterCancel(&ctx->cancel);
}
bool Discovery::start() {
    if(impl_->browse)return true;
    auto ctx=std::make_shared<BrowseContext>();ctx->state=std::make_shared<DiscoveryState>();
    {std::lock_guard<std::mutex> lock(registry_mutex);browsers[ctx.get()]=ctx;}
    DNS_SERVICE_BROWSE_REQUEST req{};req.Version=DNS_QUERY_REQUEST_VERSION1;req.QueryName=service;req.pBrowseCallback=browsed;req.pQueryContext=ctx.get();
    if(DnsServiceBrowse(&req,&ctx->cancel)!=DNS_REQUEST_PENDING){std::lock_guard<std::mutex> lock(registry_mutex);browsers.erase(ctx.get());return false;}
    impl_->browse=std::move(ctx);return true;
}
void Discovery::stop() {
    auto ctx=std::move(impl_->browse);if(!ctx)return;
    {std::lock_guard<std::mutex> lock(ctx->state->mutex);ctx->state->active=false;ctx->state->candidates.clear();ctx->state->expiry.clear();}
    DnsServiceBrowseCancel(&ctx->cancel);
}
std::vector<DiscoveryCandidate> Discovery::candidates() const {
    auto ctx=impl_->browse;if(!ctx)return {};std::lock_guard<std::mutex> lock(ctx->state->mutex);
    const auto now=GetTickCount64();
    for(auto it=ctx->state->expiry.begin();it!=ctx->state->expiry.end();)if(now>=it->second){ctx->state->candidates.erase(it->first);it=ctx->state->expiry.erase(it);}else ++it;
    std::vector<DiscoveryCandidate> result;result.reserve(ctx->state->candidates.size());for(const auto& item:ctx->state->candidates)result.push_back(item.second);return result;
}
}
