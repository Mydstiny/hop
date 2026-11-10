// Test callback lifetime and untrusted records without starting DNS-SD or
// sending multicast traffic on the user's network.
#include "../src/discovery.cpp"
#include <cstdlib>
#include <cstring>
#include <iostream>
using namespace hop::windows;
using namespace hop::windows::discovery_detail;
int main() {
    int checks=0;auto check=[&](bool value){++checks;if(!value){std::cerr<<"FAIL "<<checks<<'\n';std::exit(1);}};
    check(valid_instance(L"Laptop._hop2._tcp.local") && !valid_instance(L"_hop2._tcp.local"));
    check(!valid_instance(L"Laptop._hop._tcp.local") && bounded_name(nullptr).empty());
    auto state=std::make_shared<DiscoveryState>();auto context=std::make_shared<ResolveContext>();
    context->state=state;context->name=L"Laptop._hop2._tcp.local";context->ticket=1;state->resolving[context->name]=1;
    auto feed=[&](const wchar_t* version,std::uint64_t pending) {
        state->resolving[context->name]=pending;
        {std::lock_guard<std::mutex> lock(registry_mutex);resolves[context.get()]=context;}
        unsigned char bytes[4]={10,20,30,40};IP4_ADDRESS ip{};std::memcpy(&ip,bytes,4);
        PCWSTR keys[]={L"v"};PCWSTR values[]={version};
        auto* instance=DnsServiceConstructInstance(context->name.c_str(),L"host.local",&ip,nullptr,24891,0,0,1,keys,values);
        check(instance!=nullptr);resolved(ERROR_SUCCESS,context.get(),instance);
    };
    feed(L"1",1);check(state->candidates.empty() && state->resolving.empty());
    feed(L"2",2);check(state->candidates.empty() && state->resolving[context->name]==2);
    feed(L"2",1);check(state->candidates.size()==1 && state->candidates.begin()->second.address==L"10.20.30.40" && state->candidates.begin()->second.port==24891);
    state->candidates.clear();state->active=false;feed(L"2",1);check(state->candidates.empty());
    auto advert=std::make_shared<AdvertContext>();
    {std::lock_guard<std::mutex> lock(registry_mutex);adverts[advert.get()]=advert;}
    advertised(ERROR_SUCCESS,advert.get(),nullptr);check(advert->registered && adverts.count(advert.get())==1);
    advert->deregistering=true;advertised(ERROR_SUCCESS,advert.get(),nullptr);check(adverts.count(advert.get())==0);
    {std::lock_guard<std::mutex> lock(registry_mutex);adverts[advert.get()]=advert;}
    advert->deregistering=false;advertised(ERROR_CANCELLED,advert.get(),nullptr);check(adverts.count(advert.get())==0);
    std::cout<<"PASS "<<checks<<" DNS-SD callback checks; no network discovery started\n";
}
