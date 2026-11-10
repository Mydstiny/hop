#pragma once
#include "hop_windows/live_layout.h"
namespace hop::windows {
// Pure passive-edge policy. The owner controls hook lifetime, rechecks real
// held input before ENTER and disables/rearms on each focus transition.
class EdgeDetector {
public:
    bool configure(std::uint64_t generation,std::vector<LiveScreen> local,std::vector<LiveScreen> peer,std::vector<LivePlacement> placement);
    bool configure_peers(std::uint64_t generation,const std::vector<LiveScreen>& local,
                         const std::vector<LivePeerScreens>& peers,const std::vector<LiveScreenPlacement>& placements);
    void disarm();
    std::optional<EdgeDestination> sample(std::uint64_t generation,std::int32_t x,std::int32_t y,bool inputs_clear);
    std::optional<PeerDestination> sample_target(std::uint64_t generation,std::int32_t x,std::int32_t y,bool inputs_clear);
private:
    LiveLayout layout_;std::vector<LiveScreen> local_;std::uint64_t generation_=0,last_screen_=0;
    std::int32_t last_x_=0,last_y_=0;bool prior_=false,armed_=false;
};
}
