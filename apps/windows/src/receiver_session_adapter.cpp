#include "hop_windows/receiver_session_adapter.h"
#include "hop/wire_validation.h"
#include <algorithm>
namespace hop::windows {
namespace {
void u16(std::vector<std::uint8_t>& p,std::uint16_t n) {p.push_back(n>>8);p.push_back(n&0xff);}
void u32(std::vector<std::uint8_t>& p,std::uint32_t n) {for(int i=3;i>=0;--i)p.push_back(static_cast<std::uint8_t>(n>>(i*8)));}
void u64(std::vector<std::uint8_t>& p,std::uint64_t n) {for(int i=7;i>=0;--i)p.push_back(static_cast<std::uint8_t>(n>>(i*8)));}
std::uint64_t get64(const std::uint8_t* p) {std::uint64_t n=0;for(int i=0;i<8;++i)n=(n<<8)|p[i];return n;}
}
ReceiverSessionAdapter::ReceiverSessionAdapter(InputSink& sink,std::vector<LocalDisplay> displays,PixelRect desktop)
 : sink_(sink),displays_(std::move(displays)),desktop_(desktop) {
    if(displays_.empty() || displays_.size()>64)return;
    for(std::size_t i=0;i<displays_.size();++i) {
        if(!ReceiverInputBridge(sink_,displays_[i].id,displays_[i].bounds,desktop_).valid())return;
        for(std::size_t j=0;j<i;++j) if(displays_[i].id==displays_[j].id)return;
    }
    valid_=true;
    auto p=screen_payload(); if(p.empty() || !hop::valid_wire_payload({FrameKind::control,1,1,p})) valid_=false;
}
std::vector<std::uint64_t> ReceiverSessionAdapter::screen_ids() const {
    std::vector<std::uint64_t> ids; if(!valid_)return ids;
    ids.reserve(displays_.size()); for(const auto& d:displays_)ids.push_back(d.id);return ids;
}
std::vector<std::uint8_t> ReceiverSessionAdapter::screen_payload() const {
    if(!valid_)return {};
    std::vector<std::uint8_t> p{1,3,0,0};u16(p,static_cast<std::uint16_t>(displays_.size()));u16(p,0);
    for(const auto& d:displays_) {u64(p,d.id);u32(p,static_cast<std::uint32_t>(d.bounds.x));u32(p,static_cast<std::uint32_t>(d.bounds.y));u32(p,d.bounds.width);u32(p,d.bounds.height);}
    return p;
}
bool ReceiverSessionAdapter::enter(const Frame& frame) {
    if(!valid_ || current_ || frame.payload.size()<12)return false;
    auto id=get64(frame.payload.data()+4); auto found=std::find_if(displays_.begin(),displays_.end(),[&](const auto& d){return d.id==id;});
    if(found==displays_.end())return false;
    auto candidate=std::make_unique<ReceiverInputBridge>(sink_,found->id,found->bounds,desktop_);
    if(!candidate->enter(frame))return false; current_=std::move(candidate);return true;
}
bool ReceiverSessionAdapter::input(const Frame& frame) {return current_ && current_->input(frame);}
bool ReceiverSessionAdapter::restore() {const bool ok=sink_.release_all(); if(ok)current_.reset();return ok;}
SessionCallbacks ReceiverSessionAdapter::callbacks() {
    return {[this](const Frame& f){return enter(f);},[this](const Frame& f){return input(f);},[this]{return restore();}};
}
}
