#include "hop_windows/input_bridge.h"
#include "hop_windows/keymap.h"
#include "hop/wire_validation.h"
#include <limits>
namespace hop::windows {
namespace {
std::uint16_t get16(const std::uint8_t* p) { return (std::uint16_t(p[0])<<8)|p[1]; }
std::uint32_t get32(const std::uint8_t* p) { return (std::uint32_t(p[0])<<24)|(std::uint32_t(p[1])<<16)|(std::uint32_t(p[2])<<8)|p[3]; }
std::uint64_t get64(const std::uint8_t* p) { std::uint64_t value=0; for(int i=0;i<8;++i) value=(value<<8)|p[i]; return value; }
std::int32_t get_i32(const std::uint8_t* p) { auto n=get32(p); return static_cast<std::int32_t>(n<=0x7fffffffU ? std::int64_t(n) : std::int64_t(n)-0x100000000LL); }
bool geometry(PixelRect a,PixelRect whole) {
    if (!a.width || !a.height || !whole.width || !whole.height || a.width>65535 || a.height>65535 || whole.width>1000000 || whole.height>1000000) return false;
    const auto ax=std::int64_t(a.x),ay=std::int64_t(a.y),wx=std::int64_t(whole.x),wy=std::int64_t(whole.y);
    const auto right=ax+a.width,bottom=ay+a.height,whole_right=wx+whole.width,whole_bottom=wy+whole.height;
    return ax>=wx && ay>=wy && right<=whole_right && bottom<=whole_bottom &&
           whole_right<=std::numeric_limits<std::int32_t>::max() && whole_bottom<=std::numeric_limits<std::int32_t>::max();
}
std::uint16_t normalized(std::uint16_t value,std::int32_t start,std::uint32_t size,std::int32_t whole_start,std::uint32_t whole_size) {
    const auto pixel=std::int64_t(start)+static_cast<std::int64_t>((std::uint64_t(value)*(size-1)+32767)/65535);
    if (whole_size==1) return 0;
    return static_cast<std::uint16_t>(((pixel-whole_start)*65535+(whole_size-1)/2)/(whole_size-1));
}
}
ReceiverInputBridge::ReceiverInputBridge(InputSink& sink,std::uint64_t id,PixelRect screen,PixelRect whole)
 : sink_(sink),screen_id_(id),screen_(screen),virtual_(whole),valid_(id && geometry(screen,whole)) {}
bool ReceiverInputBridge::position(std::uint16_t x,std::uint16_t y) {
    return sink_.move_absolute(normalized(x,screen_.x,screen_.width,virtual_.x,virtual_.width),normalized(y,screen_.y,screen_.height,virtual_.y,virtual_.height));
}
bool ReceiverInputBridge::enter(const hop::Frame& f) {
    if (!valid_ || f.kind!=FrameKind::control || !hop::valid_wire_payload(f) || f.payload[1]!=0x10 || get64(f.payload.data()+4)!=screen_id_ || get16(f.payload.data()+16) || f.payload[18] || sink_.held_keys() || sink_.held_buttons()) return false;
    return position(get16(f.payload.data()+12),get16(f.payload.data()+14));
}
bool ReceiverInputBridge::input(const hop::Frame& f) {
    if (!valid_ || f.kind!=FrameKind::input || !hop::valid_wire_payload(f)) return false;
    const auto* p=f.payload.data();
    switch(p[1]) {
    case 1: return get64(p+4)==screen_id_ && position(get16(p+12),get16(p+14));
    case 2:
        return get64(p+4)==screen_id_ && position(get16(p+12),get16(p+14)) && sink_.button(static_cast<Button>(p[16]-1),p[17]!=0);
    case 3: return sink_.scroll(get_i32(p+4),get_i32(p+8));
    case 4: { const auto mapped=usage_to_scan(get16(p+4)); return mapped.status==KeyMapStatus::ok && sink_.key(mapped.scan,p[8]!=0); }
    default: return false;
    }
}
bool ReceiverInputBridge::restore() { return sink_.release_all(); }
}
