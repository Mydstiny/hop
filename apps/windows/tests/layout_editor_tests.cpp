#include "hop_windows/layout_editor.h"
#include <cstdlib>
#include <iostream>
#include <limits>
using namespace hop::windows;
int n=0;
void check(bool b,const char* s,int line){++n;if(!b){std::cerr<<line<<": "<<s<<'\n';std::exit(1);}}
#define CHECK(x) check(bool(x),#x,__LINE__)
int main(){
 LayoutEditor e; std::string error;
 std::vector<LayoutDevice> devices={
  {"mac",L"Mac",true,false,{{"m1","mac",0,0,100,100}}},
  {"win",L"Windows",true,false,{{"w1","win",100,0,100,100},{"w2","win",200,0,100,100}}},
  {"harmony",L"鸿蒙",true,false,{{"h1","harmony",300,0,100,100}}},
  {"linux",L"Linux",false,false,{{"l1","linux",0,100,100,100}}}};
 CHECK(e.replace(devices,&error) && e.devices().size()==4 && error.empty());
 CHECK(e.select("win") && e.selected_device()=="win");
 CHECK(e.expand("win",true) && e.devices()[1].expanded);
 CHECK(!e.expand("missing",true) && !e.select("missing") && e.selected_device()=="win");
 CHECK(e.move_screen("w2",200,100,&error));
 CHECK(e.devices()[1].screens[1].y==100);
 CHECK(!e.move_screen("w2",0,100,&error) && error=="overlapping screens");
 CHECK(e.devices()[1].screens[1].x==200 && e.devices()[1].screens[1].y==100 && e.selected_device()=="win");
 CHECK(e.move_device("win",0,100,&error));
 CHECK(e.devices()[1].screens[0].y==100 && e.devices()[1].screens[1].y==200);
 CHECK(!e.move_device("win",std::numeric_limits<std::int32_t>::max(),0,&error));
 CHECK(e.devices()[1].screens[0].x==100);
 CHECK(!e.move_device("missing",0,0,&error) && error=="unknown device");
 CHECK(!e.move_screen("missing",0,0,&error) && error=="unknown screen");
 auto invalid=devices;invalid[1].id="mac";CHECK(!e.replace(invalid,&error));
 invalid=devices;invalid[0].screens[0].device_id="wrong";CHECK(!e.replace(invalid,&error));
 invalid=devices;invalid[0].screens.clear();CHECK(!e.replace(invalid,&error));
 CHECK(e.devices().size()==4 && e.selected_device()=="win");
 CHECK(e.replace({devices[0]},&error) && e.selected_device().empty());
 std::cout<<"PASS "<<n<<" layout editor checks; fake devices only\n";
}
