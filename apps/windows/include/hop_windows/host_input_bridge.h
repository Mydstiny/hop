#pragma once
#include "hop_windows/input_capture.h"
#include "hop_windows/session.h"
#include "hop/input_policy.h"
namespace hop::windows {
// Consume captured events on a dedicated serial worker, never in a hook.
// The owner provides trusted screen dimensions, initial pointer location and
// the sampled Caps Lock state after activation and focus ACK. Real cursor
// recentering and edge detection remain the display owner's responsibility.
class HostInputBridge {
public:
    HostInputBridge(Session& session, std::uint64_t screen, std::uint32_t width, std::uint32_t height,
                    std::uint16_t x, std::uint16_t y, bool caps_lock, hop::PointerSettings settings);
    bool valid() const { return valid_; }
    bool captured(const CaptureEvent& event); // keys/buttons/wheels; move uses move_relative
    bool move_relative(double x, double y);
    bool set_pointer_settings(hop::PointerSettings settings);
    std::uint16_t x() const; std::uint16_t y() const;
private:
    bool send(std::vector<std::uint8_t> payload);
    Session& session_; std::uint64_t screen_; std::uint32_t width_,height_;
    double x_,y_,wheel_x_=0,wheel_y_=0;
    std::uint16_t modifiers_=0; std::uint8_t buttons_=0;
    hop::PointerSettings settings_; bool valid_;
};
}
