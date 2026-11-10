#pragma once
#include "hop_windows/input_bridge.h"
#include "hop_windows/session.h"
#include <memory>
#include <vector>
namespace hop::windows {
struct LocalDisplay { std::uint64_t id; PixelRect bounds; };
// Pure wiring of an authenticated receiver Session to a caller-owned input
// sink and trusted local display snapshot. It enumerates no hardware itself;
// callbacks and adapter must outlive the Session and run on one worker.
class ReceiverSessionAdapter {
public:
    ReceiverSessionAdapter(InputSink& sink,std::vector<LocalDisplay> displays,PixelRect desktop);
    bool valid() const { return valid_; }
    std::vector<std::uint64_t> screen_ids() const;
    std::vector<std::uint8_t> screen_payload() const;
    SessionCallbacks callbacks();
private:
    bool enter(const Frame& frame);
    bool input(const Frame& frame);
    bool restore();
    InputSink& sink_; std::vector<LocalDisplay> displays_; PixelRect desktop_; bool valid_=false;
    std::unique_ptr<ReceiverInputBridge> current_;
};
}
