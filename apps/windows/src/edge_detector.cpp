#include "hop_windows/edge_detector.h"
#include <algorithm>
#include <utility>
namespace hop::windows {
bool EdgeDetector::configure(std::uint64_t generation,std::vector<LiveScreen> local,std::vector<LiveScreen> peer,std::vector<LivePlacement> placement) {
    disarm();if(!layout_.bind_placed(generation,local,peer,placement))return false;
    generation_=generation;local_=std::move(local);armed_=true;return true;
}
bool EdgeDetector::configure_peers(std::uint64_t generation,const std::vector<LiveScreen>& local,
                                   const std::vector<LivePeerScreens>& peers,const std::vector<LiveScreenPlacement>& placements) {
    disarm();
    if(!layout_.bind_peers(generation,local,peers,placements))return false;
    generation_=generation;local_=local;armed_=true;return true;
}
void EdgeDetector::disarm(){layout_.clear();local_.clear();generation_=last_screen_=0;prior_=armed_=false;}
std::optional<EdgeDestination> EdgeDetector::sample(std::uint64_t generation,std::int32_t x,std::int32_t y,bool inputs_clear) {
    const auto target=sample_target(generation,x,y,inputs_clear);
    return target ? std::optional<EdgeDestination>{target->position} : std::nullopt;
}
std::optional<PeerDestination> EdgeDetector::sample_target(std::uint64_t generation,std::int32_t x,std::int32_t y,bool inputs_clear) {
    if(!armed_ || generation!=generation_)return std::nullopt;
    const auto screen=std::find_if(local_.begin(),local_.end(),[&](const auto& s){return std::int64_t(x)>=s.x && std::int64_t(y)>=s.y && std::int64_t(x)<std::int64_t(s.x)+s.width && std::int64_t(y)<std::int64_t(s.y)+s.height;});
    if(screen==local_.end()){prior_=false;return std::nullopt;}
    std::optional<PeerDestination> target;
    if(prior_ && last_screen_==screen->id && inputs_clear) {
        if(x<last_x_)target=layout_.route_local(generation,screen->id,hop::Edge::left,x,y);
        if(!target && x>last_x_)target=layout_.route_local(generation,screen->id,hop::Edge::right,x,y);
        if(!target && y<last_y_)target=layout_.route_local(generation,screen->id,hop::Edge::top,x,y);
        if(!target && y>last_y_)target=layout_.route_local(generation,screen->id,hop::Edge::bottom,x,y);
    }
    prior_=true;last_screen_=screen->id;last_x_=x;last_y_=y;
    if(target)disarm();return target;
}
}
