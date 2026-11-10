#include "hop_windows/live_layout.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

namespace hop::windows {
namespace {
const std::string single_peer = "peer";

std::string device_id(const std::string& peer) {
    return peer.empty() ? "local" : "peer:" + peer;
}

std::string screen_id(const std::string& peer, std::uint64_t screen) {
    return device_id(peer) + ":" + std::to_string(screen);
}

bool valid(const std::vector<LiveScreen>& screens) {
    if (screens.empty() || screens.size() > 16) return false;
    std::set<std::uint64_t> ids;
    for (const auto& screen : screens) {
        if (!screen.id || !screen.width || !screen.height || screen.width > 65535 || screen.height > 65535 ||
            !ids.insert(screen.id).second) return false;
    }
    return true;
}

const LiveScreen* find_screen(const std::vector<LiveScreen>& screens, std::uint64_t id) {
    const auto found = std::find_if(screens.begin(), screens.end(), [&](const auto& screen) { return screen.id == id; });
    return found == screens.end() ? nullptr : &*found;
}

bool vertical_edge(hop::Edge edge) {
    return edge == hop::Edge::left || edge == hop::Edge::right;
}

bool on_edge(hop::Edge edge, std::int64_t x, std::int64_t y, std::int64_t width, std::int64_t height) {
    switch (edge) {
    case hop::Edge::left: return x == 0;
    case hop::Edge::right: return x == width - 1;
    case hop::Edge::top: return y == 0;
    case hop::Edge::bottom: return y == height - 1;
    }
    return false;
}
} // namespace

void LiveLayout::clear() {
    generation_ = 0;
    local_.clear();
    peers_.clear();
    layout_.replace({});
}

const std::vector<LiveScreen>* LiveLayout::screens(const std::string& peer) const {
    if (peer.empty()) return &local_;
    const auto found = std::find_if(peers_.begin(), peers_.end(), [&](const auto& candidate) {
        return candidate.fingerprint == peer;
    });
    return found == peers_.end() ? nullptr : &found->screens;
}

bool LiveLayout::bind_peers(std::uint64_t generation, const std::vector<LiveScreen>& local,
                            const std::vector<LivePeerScreens>& peers, const std::vector<LiveScreenPlacement>& placements) {
    clear();
    if (!generation || !valid(local) || peers.empty() || peers.size() > 63) return false;
    LiveLayout candidate;
    candidate.local_ = local;
    std::set<std::string> identities;
    std::size_t screen_count = local.size();
    for (const auto& peer : peers) {
        if (peer.fingerprint.empty() || peer.fingerprint.size() > 128 ||
            !identities.insert(peer.fingerprint).second || !valid(peer.screens)) return false;
        screen_count += peer.screens.size();
    }
    if (placements.size() != screen_count) return false;
    candidate.peers_ = peers;
    std::vector<hop::Screen> layout_screens;
    layout_screens.reserve(screen_count);
    for (const auto& placement : placements) {
        const auto* displays = candidate.screens(placement.peer);
        const auto* native = displays ? find_screen(*displays, placement.screen) : nullptr;
        if (!native) return false;
        layout_screens.push_back({screen_id(placement.peer, placement.screen), device_id(placement.peer),
            placement.x, placement.y, static_cast<std::int32_t>(native->width), static_cast<std::int32_t>(native->height)});
    }
    if (!candidate.layout_.replace(std::move(layout_screens))) return false;
    candidate.generation_ = generation;
    *this = std::move(candidate);
    return true;
}

bool LiveLayout::bind_placed(std::uint64_t generation,const std::vector<LiveScreen>& local,
                             const std::vector<LiveScreen>& peer,const std::vector<LivePlacement>& placements) {
    clear();
    if (!valid(local) || !valid(peer) || placements.size() != local.size() + peer.size()) return false;
    std::vector<LiveScreenPlacement> named;
    named.reserve(placements.size());
    for (const auto& placement : placements) {
        named.push_back({placement.peer ? single_peer : std::string{}, placement.screen, placement.x, placement.y});
    }
    return bind_peers(generation, local, {{single_peer, peer}}, named);
}

bool LiveLayout::bind(std::uint64_t generation, const std::vector<LiveScreen>& local,
                      const std::vector<LiveScreen>& peer) {
    clear();
    if (!generation || !valid(local) || !valid(peer)) return false;
    std::int64_t right = std::numeric_limits<std::int64_t>::min(), top = std::numeric_limits<std::int64_t>::max();
    std::int64_t peer_left = std::numeric_limits<std::int64_t>::max(), peer_top = std::numeric_limits<std::int64_t>::max();
    std::vector<LivePlacement> placements;
    for (const auto& screen : local) {
        placements.push_back({false, screen.id, screen.x, screen.y});
        right = std::max(right, std::int64_t(screen.x) + screen.width);
        top = std::min(top, std::int64_t(screen.y));
    }
    for (const auto& screen : peer) {
        peer_left = std::min(peer_left, std::int64_t(screen.x));
        peer_top = std::min(peer_top, std::int64_t(screen.y));
    }
    for (const auto& screen : peer) {
        const auto x = right + std::int64_t(screen.x) - peer_left;
        const auto y = top + std::int64_t(screen.y) - peer_top;
        if (x < -10000000 || x > 10000000 || y < -10000000 || y > 10000000) return false;
        placements.push_back({true, screen.id, static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)});
    }
    return bind_placed(generation, local, peer, placements);
}

std::optional<PeerDestination> LiveLayout::peer_destination(const hop::Crossing& crossing) const {
    if (crossing.device_id == "local") return std::nullopt;
    const auto peer = crossing.device_id.substr(5); // "peer:" prefix is added at bind time
    const auto* displays = screens(peer);
    if (!displays) return std::nullopt;
    const auto found = std::find_if(displays->begin(), displays->end(), [&](const auto& screen) {
        return screen_id(peer, screen.id) == crossing.screen_id;
    });
    if (found == displays->end()) return std::nullopt;
    const auto along = static_cast<std::uint16_t>(std::clamp(std::lround(crossing.position * 65535.0), 0L, 65535L));
    switch (crossing.entry_edge) {
    case hop::Edge::left: return PeerDestination{peer, {found->id, 0, along}};
    case hop::Edge::right: return PeerDestination{peer, {found->id, 65535, along}};
    case hop::Edge::top: return PeerDestination{peer, {found->id, along, 0}};
    case hop::Edge::bottom: return PeerDestination{peer, {found->id, along, 65535}};
    }
    return std::nullopt;
}

std::optional<PeerDestination> LiveLayout::route_local(std::uint64_t generation, std::uint64_t screen,
                                                       hop::Edge edge, std::int32_t x, std::int32_t y) const {
    if (!generation || generation != generation_) return std::nullopt;
    const auto* native = find_screen(local_, screen);
    if (!native) return std::nullopt;
    const auto dx = std::int64_t(x) - native->x, dy = std::int64_t(y) - native->y;
    if (dx < 0 || dy < 0 || dx >= native->width || dy >= native->height ||
        !on_edge(edge, dx, dy, native->width, native->height)) return std::nullopt;
    const auto position = vertical_edge(edge) ? (double(dy) + 0.5) / native->height : (double(dx) + 0.5) / native->width;
    const auto crossing = layout_.cross(screen_id({}, screen), edge, position);
    return crossing ? peer_destination(*crossing) : std::nullopt;
}

std::optional<LiveDestination> LiveLayout::route_peer(std::uint64_t generation, const std::string& peer, std::uint64_t screen,
                                                      hop::Edge edge, std::uint16_t x, std::uint16_t y) const {
    if (!generation || generation != generation_ || peer.empty()) return std::nullopt;
    const auto* displays = screens(peer);
    if (!displays || !find_screen(*displays, screen) || !on_edge(edge, x, y, 65536, 65536)) return std::nullopt;
    const bool vertical = vertical_edge(edge);
    const auto crossing = layout_.cross(screen_id(peer, screen), edge, double(vertical ? y : x) / 65535.0);
    if (!crossing) return std::nullopt;
    if (crossing->device_id != "local") {
        if (auto target = peer_destination(*crossing)) return LiveDestination{std::move(*target)};
        return std::nullopt;
    }
    const auto target = std::find_if(local_.begin(), local_.end(), [&](const auto& display) {
        return screen_id({}, display.id) == crossing->screen_id;
    });
    if (target == local_.end()) return std::nullopt;
    const auto length = vertical ? target->height : target->width;
    const auto along = std::clamp<std::int64_t>(static_cast<std::int64_t>(std::floor(crossing->position * length)), 0, length - 1);
    std::int64_t px = target->x, py = target->y;
    switch (crossing->entry_edge) {
    case hop::Edge::left: px += std::min<std::uint32_t>(1, target->width - 1); py += along; break;
    case hop::Edge::right: px += target->width - 1 - std::min<std::uint32_t>(1, target->width - 1); py += along; break;
    case hop::Edge::top: px += along; py += std::min<std::uint32_t>(1, target->height - 1); break;
    case hop::Edge::bottom: px += along; py += target->height - 1 - std::min<std::uint32_t>(1, target->height - 1); break;
    }
    if (px < std::numeric_limits<std::int32_t>::min() || px > std::numeric_limits<std::int32_t>::max() ||
        py < std::numeric_limits<std::int32_t>::min() || py > std::numeric_limits<std::int32_t>::max()) return std::nullopt;
    return LiveDestination{LocalDestination{target->id, static_cast<std::int32_t>(px), static_cast<std::int32_t>(py)}};
}

std::optional<EdgeDestination> LiveLayout::cross(std::uint64_t generation, std::uint64_t screen, hop::Edge edge,
                                                std::int32_t x, std::int32_t y) const {
    const auto target = route_local(generation, screen, edge, x, y);
    return target && target->peer == single_peer ? std::optional<EdgeDestination>{target->position} : std::nullopt;
}

std::optional<LocalDestination> LiveLayout::cross_peer(std::uint64_t generation, std::uint64_t screen, hop::Edge edge,
                                                     std::uint16_t x, std::uint16_t y) const {
    const auto target = route_peer(generation, single_peer, screen, edge, x, y);
    if (target) {
        if (const auto* local = std::get_if<LocalDestination>(&*target)) return *local;
    }
    return std::nullopt;
}
}
