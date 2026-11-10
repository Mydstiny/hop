#include "hop_windows/live_layout.h"
#include <cstdlib>
#include <iostream>
using namespace hop::windows;
int main() {
    int checks=0;auto check=[&](bool value){++checks;if(!value){std::cerr<<"FAIL "<<checks<<'\n';std::exit(1);}};
    LiveLayout layout;const std::vector<LiveScreen> local={{1,0,0,100,100},{2,-100,0,100,100}},peer={{7,0,0,50,100},{8,50,0,50,100}};
    check(layout.bind(42,local,peer));auto crossing=layout.cross(42,1,hop::Edge::right,99,50);
    check(crossing && crossing->screen==7 && crossing->x==0 && crossing->y>32000 && crossing->y<34000);
    auto back=layout.cross_peer(42,7,hop::Edge::left,0,crossing->y);check(back && back->screen==1 && back->x==98 && back->y==50);
    check(!layout.cross_peer(41,7,hop::Edge::left,0,crossing->y) && !layout.cross_peer(42,7,hop::Edge::left,1,crossing->y));
    check(!layout.cross(41,1,hop::Edge::right,99,50));check(!layout.cross(42,1,hop::Edge::right,98,50));
    check(!layout.cross(42,2,hop::Edge::right,-1,50)); // local neighbor, never a peer transition
    check(!layout.cross(42,1,hop::Edge::right,99,101));
    check(!layout.bind(43,local,{{7,0,0,0,100}}));check(!layout.cross(42,1,hop::Edge::right,99,50));
    check(layout.bind(44,local,peer));layout.clear();check(!layout.cross(44,1,hop::Edge::right,99,50));
    check(!layout.bind(45,local,{{7,0,0,50,100},{7,50,0,50,100}}));
    const std::vector<LivePlacement> placed={{false,1,0,0},{false,2,100,0},{true,7,-50,0},{true,8,-100,0}};
    check(layout.bind_placed(46,local,peer,placed));crossing=layout.cross(46,1,hop::Edge::left,0,50);
    check(crossing && crossing->screen==7 && crossing->x==65535);
    auto overlap=placed;overlap[2].x=0;check(!layout.bind_placed(47,local,peer,overlap));check(!layout.cross(46,1,hop::Edge::left,0,50));
    auto missing=placed;missing.pop_back();check(!layout.bind_placed(48,local,peer,missing));

    // Every device may advertise screen 7. Routing must carry the target
    // device identity alongside that session-scoped ID.
    const std::string first(64, 'A'), second(64, 'B');
    const std::vector<LiveScreen> home = {{7,-100,0,100,100}};
    const std::vector<LivePeerScreens> peers = {
        {first, {{7,0,0,100,100},{8,0,100,100,100}}},
        {second, {{7,0,0,80,100}}}
    };
    const std::vector<LiveScreenPlacement> workspace = {
        {"",7,-100,0}, {first,7,0,0}, {first,8,0,100}, {second,7,100,0}
    };
    check(layout.bind_peers(49, home, peers, workspace));
    const auto from_home = layout.route_local(49,7,hop::Edge::right,-1,50);
    check(from_home && from_home->peer==first && from_home->position.screen==7 && from_home->position.x==0);
    const auto between_peers = layout.route_peer(49,first,7,hop::Edge::right,65535,32768);
    const auto* next_peer = between_peers ? std::get_if<PeerDestination>(&*between_peers) : nullptr;
    check(next_peer && next_peer->peer==second && next_peer->position.screen==7 && next_peer->position.x==0);
    const auto between_screens = layout.route_peer(49,first,7,hop::Edge::bottom,32768,65535);
    const auto* next_screen = between_screens ? std::get_if<PeerDestination>(&*between_screens) : nullptr;
    check(next_screen && next_screen->peer==first && next_screen->position.screen==8 && next_screen->position.y==0);
    const auto home_again = layout.route_peer(49,first,7,hop::Edge::left,0,32768);
    const auto* local_target = home_again ? std::get_if<LocalDestination>(&*home_again) : nullptr;
    check(local_target && local_target->screen==7 && local_target->x==-2 && local_target->y==50);
    check(!layout.route_peer(48,first,7,hop::Edge::right,65535,32768));
    check(!layout.route_peer(49,"unknown",7,hop::Edge::right,65535,32768));
    check(!layout.route_peer(49,first,7,hop::Edge::right,65534,32768));
    auto invalid = workspace;
    invalid[3].peer=first;
    check(!layout.bind_peers(50,home,peers,invalid));
    check(!layout.route_peer(49,first,7,hop::Edge::right,65535,32768));
    auto duplicate_peers = peers;
    duplicate_peers[1].fingerprint=first;
    check(!layout.bind_peers(51,home,duplicate_peers,workspace));
    std::cout<<"PASS "<<checks<<" connection-scoped geometry checks; no system input\n";
}
