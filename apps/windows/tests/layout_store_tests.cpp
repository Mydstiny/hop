#include "hop_windows/layout_store.h"
#include <cstdlib>
#include <iostream>
using namespace hop::windows;
namespace {
int n=0;void check(bool b,const char* s,int line){++n;if(!b){std::cerr<<line<<": "<<s<<'\n';std::exit(1);}}
#define CHECK(x) check(bool(x),#x,__LINE__)
struct Memory final:LayoutStorage{
 std::vector<std::uint8_t> bytes;bool exists=false,fail_read=false,fail_write=false;
 bool load(std::vector<std::uint8_t>& out,bool& found)override{if(fail_read)return false;found=exists;if(found)out=bytes;return true;}
 bool save(const std::vector<std::uint8_t>& v)override{if(fail_write)return false;bytes=v;exists=true;return true;}
};
}
int main(){
 LayoutEditor e;std::vector<LayoutDevice> devices={
  {"a",L"Mac",true,false,{{"a1","a",-100,0,100,100}},true},
  {"b",L"鸿蒙",true,true,{{"b1","b",0,0,100,100},{"b2","b",100,0,100,100}},true},
  {"c",L"Windows",false,false,{{"c1","c",0,100,100,100}},false}};
 CHECK(e.replace(devices));Memory store;bool found=false;
 CHECK(save_layout(store,e) && store.exists);
 LayoutEditor restored;CHECK(load_layout(store,restored,found) && found);
 CHECK(restored.devices().size()==3 && restored.devices()[1].name==L"鸿蒙" && restored.devices()[1].expanded);
 CHECK(!restored.devices()[0].online && !restored.devices()[0].trusted && !restored.devices()[1].online && !restored.devices()[1].trusted);
 CHECK(restored.devices()[0].screens[0].x==-100 && restored.devices()[1].screens.size()==2);
 auto valid=store.bytes;store.bytes.pop_back();CHECK(!load_layout(store,restored,found) && restored.devices().size()==3);
 store.bytes=valid;store.bytes[0]='X';CHECK(!load_layout(store,restored,found));
 store.bytes=valid;store.bytes.push_back(0);CHECK(!load_layout(store,restored,found));
 store.bytes=valid;store.bytes[4]=0xff;CHECK(!load_layout(store,restored,found));
 store.bytes=valid;store.fail_read=true;CHECK(!load_layout(store,restored,found));store.fail_read=false;
 store.fail_write=true;CHECK(!save_layout(store,e) && store.bytes==valid);store.fail_write=false;
 Memory missing;LayoutEditor unchanged;CHECK(unchanged.replace({devices[0]}));CHECK(load_layout(missing,unchanged,found)&&!found&&unchanged.devices().size()==1);
 LayoutEditor empty;std::vector<std::uint8_t> zero;CHECK(encode_layout(empty,zero));CHECK(decode_layout(zero,unchanged)&&unchanged.devices().empty());
 auto bad=devices;bad[0].id="bad id";bad[0].screens[0].device_id="bad id";CHECK(e.replace(bad));CHECK(!save_layout(store,e));
 std::cout<<"PASS "<<n<<" layout persistence checks; fake store only, no registry access\n";
}
