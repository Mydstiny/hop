#pragma once
#include <cstdint>
#include <limits>
#include <vector>

namespace hop {
struct V2Display {
    std::int32_t display_id, x, y;
    std::uint32_t width, height;
    // OHOS display 0 is valid. The wire reserves screen ID 0.
    std::uint64_t screenId() const { return static_cast<std::uint64_t>(display_id) + 1; }
    std::int64_t right() const { return static_cast<std::int64_t>(x) + width; }
    std::int64_t bottom() const { return static_cast<std::int64_t>(y) + height; }
};

inline bool validV2Displays(const std::vector<V2Display>& displays) {
    if (displays.empty() || displays.size() > 64 || displays.front().x != 0 || displays.front().y != 0) return false;
    for (std::size_t i = 0; i < displays.size(); ++i) {
        const auto& a = displays[i];
        if (a.display_id < 0 || !a.width || !a.height || a.width > 65535 || a.height > 65535 ||
            a.right() > std::numeric_limits<std::int32_t>::max() ||
            a.bottom() > std::numeric_limits<std::int32_t>::max()) return false;
        for (std::size_t j = 0; j < i; ++j) {
            const auto& b = displays[j];
            const bool overlap = a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
            if (a.display_id == b.display_id || overlap) return false;
        }
    }
    return true;
}
}
