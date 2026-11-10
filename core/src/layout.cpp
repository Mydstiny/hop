#include "hop/layout.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <utility>

namespace hop {
namespace {
constexpr std::int32_t coordinate_limit = 10000000;
constexpr std::int32_t size_limit = 1000000;

bool valid(const Screen& s) {
    return !s.id.empty() && !s.device_id.empty() &&
        s.x >= -coordinate_limit && s.x <= coordinate_limit &&
        s.y >= -coordinate_limit && s.y <= coordinate_limit &&
        s.width > 0 && s.width <= size_limit &&
        s.height > 0 && s.height <= size_limit;
}
std::int64_t right(const Screen& s) { return std::int64_t(s.x) + s.width; }
std::int64_t bottom(const Screen& s) { return std::int64_t(s.y) + s.height; }
bool overlaps(const Screen& a, const Screen& b) {
    return std::max<std::int64_t>(a.x, b.x) < std::min(right(a), right(b)) &&
           std::max<std::int64_t>(a.y, b.y) < std::min(bottom(a), bottom(b));
}
}

bool Layout::replace(std::vector<Screen> screens, std::string* error) {
    std::unordered_set<std::string> ids;
    for (std::size_t i = 0; i < screens.size(); ++i) {
        if (!valid(screens[i])) {
            if (error) *error = "invalid screen";
            return false;
        }
        if (!ids.insert(screens[i].id).second) {
            if (error) *error = "duplicate screen id";
            return false;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (overlaps(screens[i], screens[j])) {
                if (error) *error = "overlapping screens";
                return false;
            }
        }
    }
    screens_ = std::move(screens);
    if (error) error->clear();
    return true;
}

std::optional<Crossing> Layout::cross(const std::string& screen_id, Edge edge,
                                      double position) const {
    if (!std::isfinite(position) || position < 0 || position > 1) return std::nullopt;
    auto source = std::find_if(screens_.begin(), screens_.end(),
                               [&](const Screen& s) { return s.id == screen_id; });
    if (source == screens_.end()) return std::nullopt;
    const bool vertical = edge == Edge::left || edge == Edge::right;
    const double start = vertical ? source->y : source->x;
    const double length = vertical ? source->height : source->width;
    double along = start + position * length;
    if (position == 1) along = std::nextafter(start + length, start);
    for (const auto& target : screens_) {
        if (target.id == source->id) continue;
        bool touches = false;
        Edge entry = Edge::left;
        switch (edge) {
        case Edge::left:   touches = right(target) == source->x; entry = Edge::right; break;
        case Edge::right:  touches = right(*source) == target.x; entry = Edge::left; break;
        case Edge::top:    touches = bottom(target) == source->y; entry = Edge::bottom; break;
        case Edge::bottom: touches = bottom(*source) == target.y; entry = Edge::top; break;
        }
        if (!touches) continue;
        const double start = vertical ? target.y : target.x;
        const double length = vertical ? target.height : target.width;
        if (along >= start && along < start + length) {
            return Crossing{target.id, target.device_id, entry, (along - start) / length};
        }
    }
    return std::nullopt;
}
} // namespace hop
