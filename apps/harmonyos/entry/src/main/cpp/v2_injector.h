#pragma once
#include "injector.h"
#include "v2_display.h"
#include "hop/session_c.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace hop {
// Used only by the serialized receiver worker. Each focus holds its exact
// display snapshot, and failed neutralization prevents the next snapshot.
class V2Injector {
    Injector input_;
    std::vector<V2Display> displays_;
    bool batch_authorized_=false, edge_pending_=false;
    bool vertical_edge_=false, horizontal_edge_=false;
    std::uint64_t focus_screen_=0, focus_epoch_=0;
    std::uint16_t last_x_=0,last_y_=0,edge_x_=0,edge_y_=0;
    static std::uint16_t U16v(const std::uint8_t* p) {return std::uint16_t(p[0])<<8|p[1];}
    static std::uint64_t U64v(const std::uint8_t* p) {
        std::uint64_t n=0;for(int i=0;i<8;++i)n=(n<<8)|p[i];return n;
    }
    static std::int32_t I32v(const std::uint8_t* p) {
        const std::uint32_t v=std::uint32_t(p[0])<<24|std::uint32_t(p[1])<<16|std::uint32_t(p[2])<<8|p[3];
        return static_cast<std::int32_t>(v<=0x7fffffffU ? std::int64_t(v) : std::int64_t(v)-0x100000000LL);
    }
    void updateEdge(std::uint16_t x, std::uint16_t y, bool moving) {
        // An edge is armed only by motion from the interior. Keep it armed
        // while clamped there, including across a batch of identical samples.
        vertical_edge_=(x==0 || x==65535) && (last_x_!=x ? moving : vertical_edge_);
        horizontal_edge_=(y==0 || y==65535) && (last_y_!=y ? moving : horizontal_edge_);
        edge_pending_=(moving || edge_pending_) && (vertical_edge_ || horizontal_edge_);
        // A corner reached on one axis must not request a different exit.
        edge_x_=horizontal_edge_ && !vertical_edge_ ? std::clamp<std::uint16_t>(x,1,65534) : x;
        edge_y_=vertical_edge_ && !horizontal_edge_ ? std::clamp<std::uint16_t>(y,1,65534) : y;
        last_x_=x;last_y_=y;
    }
    static int Enter(void* context,const std::uint8_t* p,std::size_t n,std::uint64_t epoch) {
        auto* self=static_cast<V2Injector*>(context);
        if(!self||!self->batch_authorized_||self->focus_epoch_||!epoch||!p||n!=20||
           p[0]!=1||p[1]!=0x10||p[2]||p[3]||U16v(p+16)||p[18]||p[19])return 0;
        const auto screen=U64v(p+4);
        const auto target=std::find_if(self->displays_.begin(),self->displays_.end(),
                                       [screen](const V2Display& d){return d.screenId()==screen;});
        if(target==self->displays_.end()||
           !self->input_.TargetDisplay(target->display_id,target->x,target->y,
                                       static_cast<int>(target->width),static_cast<int>(target->height)))return 0;
        const auto x=U16v(p+12),y=U16v(p+14);
        if(!self->input_.Position(x,y))return 0;
        self->focus_screen_=screen;self->focus_epoch_=epoch;
        self->last_x_=x;self->last_y_=y;
        self->edge_pending_=self->vertical_edge_=self->horizontal_edge_=false;
        return 1;
    }
    static int Input(void* context,const std::uint8_t* p,std::size_t n,std::uint64_t epoch) {
        auto* self=static_cast<V2Injector*>(context);
        if(!self||!self->batch_authorized_||!epoch||epoch!=self->focus_epoch_||!p||n<4||p[0]!=1||p[2]||p[3])return 0;
        switch(p[1]) {
        case 1: {
            if(n!=16||U64v(p+4)!=self->focus_screen_)return 0;
            const auto x=U16v(p+12),y=U16v(p+14);
            if(!self->input_.Position(x,y))return 0;
            self->updateEdge(x,y,true);
            return 1;
        }
        case 2: {
            if(n!=20 || U64v(p+4)!=self->focus_screen_)return 0;
            const auto x=U16v(p+12),y=U16v(p+14);
            if(!self->input_.Position(x,y) || !self->input_.Buttons(p[18]))return 0;
            self->updateEdge(x,y,false);
            return 1;
        }
        case 3: return n==12 && self->input_.Wheel(I32v(p+4),I32v(p+8)) ? 1 : 0;
        case 4: return n==12 && self->input_.KeyHid(U16v(p+4),U16v(p+6),p[8]!=0,p[9]!=0) ? 1 : 0;
        default: return 0;
        }
    }
    static int Restore(void* context) {auto* self=static_cast<V2Injector*>(context);return self && self->release() ? 1 : 0;}
public:
    bool configure(const std::vector<V2Display>& displays) {
        if(!validV2Displays(displays)||!release())return false;
        displays_=displays;
        return true;
    }
    bool beginBatch() {batch_authorized_=Injector::Authorization()==AUTHORIZED;return batch_authorized_;}
    void endBatch() {batch_authorized_=false;}
    bool release() {
        focus_screen_=focus_epoch_=0;
        edge_pending_=vertical_edge_=horizontal_edge_=false;
        return input_.Release();
    }
    bool takeEdge(std::uint16_t& x,std::uint16_t& y) {
        if(!edge_pending_)return false;edge_pending_=false;x=edge_x_;y=edge_y_;return true;
    }
    static hop_v2_callbacks callbacks() {hop_v2_callbacks cb{};cb.enter=Enter;cb.input=Input;cb.restore=Restore;return cb;}
};
}
