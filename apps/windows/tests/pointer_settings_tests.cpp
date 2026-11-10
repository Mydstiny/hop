#include "hop_windows/pointer_settings.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <thread>
#include <atomic>
using namespace hop::windows;
namespace {
int checks=0;
void check(bool v,const char* s,int line){++checks;if(!v){std::cerr<<line<<": "<<s<<'\n';std::exit(1);}}
#define CHECK(x) check(bool(x),#x,__LINE__)
struct Memory final : PointerSettingsStore {
    std::map<DeviceFingerprint,std::vector<std::uint8_t>> data;
    bool fail_read=false,fail_write=false;
    int writes=0;
    bool load(const DeviceFingerprint& id,std::vector<std::uint8_t>& out,bool& found) override {
        if(fail_read) return false;
        auto it=data.find(id);found=it!=data.end();if(found)out=it->second;return true;
    }
    bool save(const DeviceFingerprint& id,const std::vector<std::uint8_t>& bytes) override {
        ++writes;if(fail_write)return false;data[id]=bytes;return true;
    }
};
}
int main(){
    Memory store;TargetPointerSettings settings(store);
    DeviceFingerprint a{},b{},zero{};a[0]=1;b[0]=2;
    DeviceFingerprint parsed{};
    CHECK(parse_device_fingerprint(std::string(64,'A'),parsed) && parsed[0]==0xaa);
    CHECK(parse_device_fingerprint(std::string(64,'a'),parsed) && parsed[31]==0xaa);
    CHECK(!parse_device_fingerprint(std::string(64,'0'),parsed) && parsed[0]==0xaa);
    CHECK(!parse_device_fingerprint(std::string(63,'A'),parsed) && !parse_device_fingerprint(std::string(64,'Z'),parsed));
    CHECK(!settings.current() && !settings.motion({1,1}));
    CHECK(!settings.select(zero));
    CHECK(settings.select(a) && settings.current() && settings.current()->movement_scale==1 && settings.motion_rate()==240);
    CHECK(settings.update_motion_rate(a,500) && settings.motion_rate()==500);
    CHECK(!settings.update_motion_rate(a,19) && !settings.update_motion_rate(a,1001));
    hop::PointerSettings one;one.movement_scale=2;one.horizontal_scroll_scale=0.5;one.reverse_vertical=true;
    CHECK(settings.update(a,one));
    auto motion=settings.motion({3,-4});CHECK(motion && motion->x==6 && motion->y==-8);
    auto wheel=settings.scroll({2,3});CHECK(wheel && wheel->x==1 && wheel->y==-3);
    hop::PointerSettings two;two.movement_scale=0.5;two.vertical_scroll_scale=3;
    CHECK(settings.update(b,two));
    CHECK(settings.update_motion_rate(b,60) && settings.motion_rate()==500);
    CHECK(settings.current()->movement_scale==2); // editing another target does not change current
    CHECK(settings.select(b) && settings.current()->movement_scale==0.5 && settings.motion_rate()==60);
    motion=settings.motion({4,6});CHECK(motion && motion->x==2 && motion->y==3);
    wheel=settings.scroll({2,3});CHECK(wheel && wheel->x==2 && wheel->y==9);
    CHECK(settings.select(a) && settings.current()->reverse_vertical && settings.motion_rate()==500);
    TargetPointerSettings reloaded(store);CHECK(reloaded.select(b) && reloaded.current()->movement_scale==0.5 && reloaded.motion_rate()==60);
    DeviceFingerprint legacy{};legacy[0]=3;store.data[legacy]=store.data[b];store.data[legacy].resize(29);store.data[legacy][3]='1';
    CHECK(reloaded.select(legacy) && reloaded.motion_rate()==240 && reloaded.current()->movement_scale==0.5);
    CHECK(reloaded.update_motion_rate(legacy,90) && reloaded.motion_rate()==90 && store.data[legacy].size()==31);
    auto invalid=one;invalid.movement_scale=std::numeric_limits<double>::quiet_NaN();
    auto writes=store.writes;CHECK(!settings.update(a,invalid) && store.writes==writes);
    invalid=one;invalid.vertical_scroll_scale=11;CHECK(!settings.update(a,invalid));
    CHECK(!settings.update(zero,one));
    store.fail_write=true;CHECK(!settings.update(a,two) && settings.current()->movement_scale==2);
    store.fail_write=false;store.data[b][28]=0x80;
    CHECK(!settings.select(b) && !settings.current() && !settings.motion({1,1}));
    store.fail_read=true;CHECK(!settings.select(a) && !settings.current());
    store.fail_read=false;CHECK(settings.select(a));
    std::atomic<bool> done{false}, inconsistent{false};
    std::thread writer([&] {
        for(int i=0;i<2000;++i) if(!settings.update(a,(i&1) ? one : two)) inconsistent=true;
        done=true;
    });
    while(!done.load()) {
        const auto value=settings.current();
        if(!value || !((value->movement_scale==2 && value->reverse_vertical) ||
                       (value->movement_scale==0.5 && !value->reverse_vertical))) inconsistent=true;
    }
    writer.join(); CHECK(!inconsistent.load());
    settings.clear();CHECK(!settings.current() && !settings.scroll({1,1}));
    std::cout<<"PASS "<<checks<<" per-device software sensitivity checks; fake store only, no hardware DPI access\n";
}
