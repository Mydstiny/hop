#include "hop_windows/layout_view_model.h"
#include <cstdlib>
#include <iostream>
using namespace hop::windows;
int n=0;void check(bool v,const char* t,int l){++n;if(!v){std::cerr<<l<<": "<<t<<'\n';std::exit(1);}}
#define CHECK(x) check(bool(x),#x,__LINE__)
int main(){
 LayoutEditor e; CHECK(project_layout(e,500,300).boxes.empty());
 std::vector<LayoutDevice> d={{"a",L"甲",true,false,{{"a1","a",0,0,100,100}}},
  {"b",L"乙",false,false,{{"b1","b",100,0,100,100},{"b2","b",200,0,100,100}}},
  {"c",L"丙",true,false,{{"c1","c",0,100,100,100}}}};
 CHECK(e.replace(d)); CHECK(e.select("b"));
 CHECK(!device_allows_pointer_settings(d[0]));
 auto trusted=d[0];trusted.id=std::string(64,'A');trusted.trusted=true;
 CHECK(device_allows_pointer_settings(trusted));trusted.online=false;CHECK(!device_allows_pointer_settings(trusted));
 trusted.online=true;trusted.id=std::string(64,'0');CHECK(!device_allows_pointer_settings(trusted));
 auto view=project_layout(e,500,300); CHECK(view.boxes.size()==3 && view.scale>0);
 CHECK(view.boxes[1].selected && !view.boxes[1].online && view.boxes[1].screen_id.empty());
 auto hit=hit_layout(view,view.boxes[1].left+1,view.boxes[1].top+1); CHECK(hit && hit->device_id=="b");
 CHECK(!hit_layout(view,0,0));
 CHECK(e.expand("b",true));view=project_layout(e,500,300);CHECK(view.boxes.size()==4);
 CHECK(view.boxes[1].screen_id=="b1" && view.boxes[2].screen_id=="b2");
 CHECK(!project_layout(e,1,1).scale);
 CHECK(e.move_screen("b2",200,100));view=project_layout(e,500,300);CHECK(view.boxes[2].top>view.boxes[1].top);
 std::cout<<"PASS "<<n<<" layout projection checks; fake devices, no windows or input\n";
}
