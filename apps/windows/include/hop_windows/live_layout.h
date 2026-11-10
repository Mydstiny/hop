#pragma once
#include "hop/layout.h"
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace hop::windows {
struct LiveScreen {std::uint64_t id;std::int32_t x,y;std::uint32_t width,height;};
struct LivePlacement {bool peer;std::uint64_t screen;std::int32_t x,y;};
struct EdgeDestination {std::uint64_t screen;std::uint16_t x,y;};
struct LocalDestination {std::uint64_t screen;std::int32_t x,y;};

struct LivePeerScreens {
    // Supplied by the session owner after authentication.
    std::string fingerprint;
    std::vector<LiveScreen> screens;
};
struct LiveScreenPlacement {
    std::string peer; // empty for a local screen
    std::uint64_t screen;
    std::int32_t x, y;
};
struct PeerDestination {
    std::string peer;
    EdgeDestination position;
};
using LiveDestination = std::variant<LocalDestination, PeerDestination>;

// A connection-scoped geometry snapshot. Each screen ID is scoped to one
// device. A replacement, including a failed replacement, invalidates routes
// using the previous binding.
class LiveLayout {
public:
    bool bind(std::uint64_t generation,const std::vector<LiveScreen>& local,const std::vector<LiveScreen>& peer);
    bool bind_placed(std::uint64_t generation,const std::vector<LiveScreen>& local,const std::vector<LiveScreen>& peer,const std::vector<LivePlacement>& placements);
    bool bind_peers(std::uint64_t generation, const std::vector<LiveScreen>& local,
                    const std::vector<LivePeerScreens>& peers, const std::vector<LiveScreenPlacement>& placements);
    void clear();
    std::optional<EdgeDestination> cross(std::uint64_t generation,std::uint64_t local_screen,hop::Edge edge,std::int32_t x,std::int32_t y) const;
    std::optional<LocalDestination> cross_peer(std::uint64_t generation,std::uint64_t peer_screen,hop::Edge edge,std::uint16_t x,std::uint16_t y) const;
    std::optional<PeerDestination> route_local(std::uint64_t generation, std::uint64_t screen,
                                               hop::Edge edge, std::int32_t x, std::int32_t y) const;
    std::optional<LiveDestination> route_peer(std::uint64_t generation, const std::string& peer, std::uint64_t screen,
                                              hop::Edge edge, std::uint16_t x, std::uint16_t y) const;
private:
    const std::vector<LiveScreen>* screens(const std::string& peer) const;
    std::optional<PeerDestination> peer_destination(const hop::Crossing& crossing) const;
    std::uint64_t generation_=0;
    std::vector<LiveScreen> local_;
    std::vector<LivePeerScreens> peers_;
    hop::Layout layout_;
};
}
