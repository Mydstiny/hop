#pragma once
#include "hop/frame.h"
#include "hop_windows/input_sink.h"
#include <cstdint>
namespace hop::windows {
struct PixelRect { std::int32_t x, y; std::uint32_t width, height; };
// Receiver-side worker adapter. The owning authenticated Session must check
// role, active focus epoch, screen and event transitions before these methods.
// Geometry is supplied by a trusted display enumerator, never by the peer.
// Injected SendInput remains subject to desktop/UIPI and real-device tests.
class ReceiverInputBridge {
public:
    ReceiverInputBridge(InputSink& sink,std::uint64_t screen_id,PixelRect screen,PixelRect virtual_desktop);
    bool valid() const { return valid_; }
    bool enter(const hop::Frame& frame);
    bool input(const hop::Frame& frame);
    bool restore();
private:
    bool position(std::uint16_t x,std::uint16_t y);
    InputSink& sink_; std::uint64_t screen_id_; PixelRect screen_,virtual_; bool valid_;
};
}
