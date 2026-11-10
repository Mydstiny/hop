#include "hop_windows/host_input_bridge.h"
#include "hop_windows/keymap.h"
#include <algorithm>
#include <cmath>
namespace hop::windows {
namespace {
void u16(std::vector<std::uint8_t>& p,std::uint16_t n) { p.push_back(n>>8); p.push_back(n&0xff); }
void u32(std::vector<std::uint8_t>& p,std::uint32_t n) { for(int i=3;i>=0;--i)p.push_back(static_cast<std::uint8_t>(n>>(i*8))); }
void u64(std::vector<std::uint8_t>& p,std::uint64_t n) { for(int i=7;i>=0;--i)p.push_back(static_cast<std::uint8_t>(n>>(i*8))); }
std::uint16_t modifier(std::uint16_t usage) {
    switch(usage) {case 0xe0:return hop::left_control;case 0xe1:return hop::left_shift;case 0xe2:return hop::left_alt;case 0xe3:return hop::left_meta;
    case 0xe4:return hop::right_control;case 0xe5:return hop::right_shift;case 0xe6:return hop::right_alt;case 0xe7:return hop::right_meta;default:return 0;}
}
std::uint16_t axis(double value,std::uint32_t extent) { return extent==1 ? 0 : static_cast<std::uint16_t>(std::lround(value*65535/(extent-1))); }
}
HostInputBridge::HostInputBridge(Session& session,std::uint64_t screen,std::uint32_t width,std::uint32_t height,std::uint16_t x,std::uint16_t y,bool caps,hop::PointerSettings settings)
 : session_(session),screen_(screen),width_(width),height_(height),x_(width ? double(x)*(width-1)/65535 : 0),y_(height ? double(y)*(height-1)/65535 : 0),
   modifiers_(caps ? hop::caps_lock : 0),settings_(settings),valid_(screen && width && width<=65535 && height && height<=65535 && hop::valid_pointer_settings(settings)) {}
std::uint16_t HostInputBridge::x() const { return axis(x_,width_); }
std::uint16_t HostInputBridge::y() const { return axis(y_,height_); }
bool HostInputBridge::set_pointer_settings(hop::PointerSettings settings) { if(!hop::valid_pointer_settings(settings))return false; settings_=settings; return true; }
bool HostInputBridge::send(std::vector<std::uint8_t> payload) {
    if (!valid_ || session_.phase()!=SessionPhase::active || !session_.send_input(std::move(payload))) { session_.disconnect(); return false; }
    return true;
}
bool HostInputBridge::captured(const CaptureEvent& event) {
    try {
    if (!valid_ || session_.phase()!=SessionPhase::active) { session_.disconnect(); return false; }
    if (event.kind==CaptureKind::key) {
        if (event.code>255) { session_.disconnect(); return false; }
        auto mapped=scan_to_usage({static_cast<std::uint8_t>(event.code),event.extended,event.e1});
        if (mapped.status!=KeyMapStatus::ok) { session_.disconnect(); return false; }
        const auto bit=modifier(mapped.usage);
        if (bit) { if(event.down) modifiers_|=bit; else modifiers_&=static_cast<std::uint16_t>(~bit); }
        if (mapped.usage==0x39 && event.down && !event.repeat) modifiers_^=hop::caps_lock;
        std::vector<std::uint8_t> p{1,4,0,0}; u16(p,mapped.usage); u16(p,modifiers_); p.push_back(event.down); p.push_back(event.repeat); u16(p,0);
        return send(std::move(p));
    }
    if (event.kind==CaptureKind::button) {
        if (event.code>4) { session_.disconnect(); return false; }
        const auto bit=static_cast<std::uint8_t>(1u<<event.code);
        if (bool(buttons_&bit)==event.down) { session_.disconnect(); return false; }
        buttons_=static_cast<std::uint8_t>(event.down ? buttons_|bit : buttons_&~bit);
        std::vector<std::uint8_t> p{1,2,0,0}; u64(p,screen_); u16(p,x()); u16(p,y()); p.push_back(static_cast<std::uint8_t>(event.code+1)); p.push_back(event.down); p.push_back(buttons_); p.push_back(0);
        return send(std::move(p));
    }
    if (event.kind==CaptureKind::wheel) {
        auto delta=hop::map_scroll(settings_,{double(event.x),double(event.y)});
        if(!delta) { session_.disconnect(); return false; }
        const double sx=delta->x+wheel_x_,sy=delta->y+wheel_y_;
        if(std::abs(sx)>120000 || std::abs(sy)>120000) { session_.disconnect(); return false; }
        const auto dx=static_cast<std::int32_t>(std::trunc(sx)),dy=static_cast<std::int32_t>(std::trunc(sy)); wheel_x_=sx-dx; wheel_y_=sy-dy;
        if (!dx && !dy) return true;
        std::vector<std::uint8_t> p{1,3,0,0}; u32(p,static_cast<std::uint32_t>(dx)); u32(p,static_cast<std::uint32_t>(dy)); return send(std::move(p));
    }
    // Absolute low-level hook coordinates require recentering and an owner
    // converting them into safe deltas. Never silently forward them as delta.
    session_.disconnect(); return false;
    } catch (...) { session_.disconnect(); return false; }
}
bool HostInputBridge::move_relative(double dx,double dy) {
    try {
    if (!valid_ || session_.phase()!=SessionPhase::active) { session_.disconnect(); return false; }
    auto delta=hop::map_motion(settings_,{dx,dy}); if (!delta) { session_.disconnect(); return false; }
    x_=std::clamp(x_+delta->x,0.0,double(width_-1)); y_=std::clamp(y_+delta->y,0.0,double(height_-1));
    std::vector<std::uint8_t> p{1,1,0,0}; u64(p,screen_); u16(p,x()); u16(p,y()); return send(std::move(p));
    } catch (...) { session_.disconnect(); return false; }
}
}
