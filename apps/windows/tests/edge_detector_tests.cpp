#include "hop_windows/edge_detector.h"
#include <cstdlib>
#include <iostream>
using namespace hop::windows;
int main() {
    int checks=0;auto check=[&](bool value){++checks;if(!value){std::cerr<<"FAIL "<<checks<<'\n';std::exit(1);}};
    const std::vector<LiveScreen> local={{1,0,0,100,100}},peer={{7,0,0,100,100}};
    const std::vector<LivePlacement> layout={{false,1,0,0},{true,7,100,0}};EdgeDetector detector;
    check(detector.configure(20,local,peer,layout));check(!detector.sample(20,98,50,true));
    check(!detector.sample(20,99,50,false));check(!detector.sample(20,99,50,true));
    check(!detector.sample(20,98,50,true));auto target=detector.sample(20,99,50,true);
    check(target && target->screen==7 && target->x==0);check(!detector.sample(20,99,50,true));
    check(detector.configure(21,local,peer,layout));check(!detector.sample(20,98,50,true) && !detector.sample(21,99,50,true));
    detector.disarm();check(!detector.sample(21,99,50,true));
    std::cout<<"PASS "<<checks<<" passive-edge policy checks; no system input\n";
}
