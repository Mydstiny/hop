#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hop {

enum class Edge { left, right, top, bottom };

// Rectangles are half-open in layout coordinates. Coordinates are intentionally
// bounded to make endpoint arithmetic safe on every supported architecture.
struct Screen {
    std::string id;
    std::string device_id;
    std::int32_t x;
    std::int32_t y;
    std::int32_t width;
    std::int32_t height;
};

struct Crossing {
    std::string screen_id;
    std::string device_id;
    Edge entry_edge;
    // Normalized position along the receiving edge, top-to-bottom or left-to-right.
    double position;
};

class Layout {
public:
    // Reject ambiguous overlaps and invalid identifiers before changing state.
    // Failure leaves the previous valid layout intact.
    bool replace(std::vector<Screen> screens, std::string* error = nullptr);
    const std::vector<Screen>& screens() const { return screens_; }

    // position is normalized along the source edge and must be finite in [0, 1].
    // Screen bounds are half-open; position 1 denotes the final point just inside
    // the edge rather than a corner-only connection to another screen.
    std::optional<Crossing> cross(const std::string& screen_id, Edge edge,
                                  double position) const;
private:
    std::vector<Screen> screens_;
};

} // namespace hop
