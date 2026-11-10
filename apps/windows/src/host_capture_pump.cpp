#include "hop_windows/host_capture_pump.h"
#include <windows.h>
#include <cmath>
#include <limits>
#include <utility>
namespace hop::windows {
namespace {
using SetContext=HANDLE (WINAPI*)(HANDLE);
SetContext dpi_function() {
    const auto user32=GetModuleHandleW(L"user32.dll");
    return user32 ? reinterpret_cast<SetContext>(GetProcAddress(user32,"SetThreadDpiAwarenessContext")) : nullptr;
}
}
bool WindowsCaptureSource::inputs_clear() {
    for(int vk=1;vk<256;++vk)if(GetAsyncKeyState(vk)&0x8000)return false;
    return true;
}
bool WindowsCaptureSource::prepare(CursorPoint& original,CursorPoint& anchor,bool& caps) {
    const auto set_context=dpi_function(); if(!set_context)return false;
    const auto old=set_context(reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(-4))); if(!old)return false;
    POINT point{}; bool ok=GetCursorPos(&point)!=FALSE;
    HMONITOR monitor=ok ? MonitorFromPoint(point,MONITOR_DEFAULTTONEAREST) : nullptr;
    MONITORINFO info{}; info.cbSize=sizeof(info);
    ok=ok && monitor && GetMonitorInfoW(monitor,&info);
    const auto width=std::int64_t(info.rcMonitor.right)-info.rcMonitor.left,height=std::int64_t(info.rcMonitor.bottom)-info.rcMonitor.top;
    ok=ok && width>0 && height>0;
    if(ok) {
        original={point.x,point.y}; anchor={static_cast<std::int32_t>(std::int64_t(info.rcMonitor.left)+width/2),static_cast<std::int32_t>(std::int64_t(info.rcMonitor.top)+height/2)};
        caps=(GetKeyState(VK_CAPITAL)&1)!=0; ok=SetCursorPos(anchor.x,anchor.y)!=FALSE;
    }
    const bool restored=set_context(old)!=nullptr;
    if(!restored && ok) SetCursorPos(original.x,original.y);
    return ok && restored;
}
bool WindowsCaptureSource::restore_cursor(CursorPoint p) {
    const auto set_context=dpi_function(); if(!set_context)return false;
    const auto old=set_context(reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(-4))); if(!old)return false;
    const bool moved=SetCursorPos(p.x,p.y)!=FALSE,restored=set_context(old)!=nullptr;
    return moved && restored;
}
bool HostCapturePump::set_return_edge(std::function<std::optional<CursorPoint>(std::uint64_t,std::uint16_t,std::uint16_t,std::int32_t,std::int32_t)> callback) {
    if(!callback)return false;
    return set_route_edge([callback=std::move(callback)](auto screen,auto x,auto y,auto dx,auto dy)->std::optional<LiveDestination> {
        const auto target=callback(screen,x,y,dx,dy);
        if(target)return LiveDestination{LocalDestination{0,target->x,target->y}};
        return std::nullopt;
    });
}
bool HostCapturePump::set_route_edge(std::function<std::optional<LiveDestination>(std::uint64_t,std::uint16_t,std::uint16_t,std::int32_t,std::int32_t)> callback) {
    if(started_capture_ || !callback)return false;
    route_edge_=std::move(callback);return true;
}
std::optional<PeerDestination> HostCapturePump::take_handoff() {
    return std::exchange(handoff_,std::nullopt);
}
bool HostCapturePump::begin(std::uint64_t screen,std::uint32_t width,std::uint32_t height,std::uint16_t x,std::uint16_t y,hop::PointerSettings settings,std::uint64_t now,std::uint16_t motion_rate) {
    if(started_capture_ || session_.phase()!=SessionPhase::active)return false;
    seen_active_=false;restored_=false;deactivation_requested_=false;
    if(!screen || !width || width>65535 || !height || height>65535 || !hop::valid_pointer_settings(settings) || motion_rate<20 || motion_rate>1000)return fail();
    bool caps=false; if(!source_.prepare(original_,anchor_,caps))return fail();
    try {bridge_=std::make_unique<HostInputBridge>(session_,screen,width,height,x,y,caps,settings);} catch (...) {source_.restore_cursor(original_);return fail();}
    if(!bridge_->valid() || !source_.start()) {bridge_.reset();source_.restore_cursor(original_);return fail();}
    screen_=screen;started_capture_=true;started_=last_now_=next_motion_=now;motion_rate_=motion_rate;pending_x_=pending_y_=0;
    if(!source_.activate())return fail();
    return true;
}
bool HostCapturePump::fail() {
    if(session_.phase()==SessionPhase::active) { if(!session_.release(2))session_.disconnect(); }
    else session_.disconnect();
    deactivate(); return false;
}
bool HostCapturePump::flush_motion(std::uint64_t now) {
    if(!pending_x_ && !pending_y_)return true;
    const auto interval=(1000u+motion_rate_-1)/motion_rate_;
    if(now>std::numeric_limits<std::uint64_t>::max()-interval || !bridge_->move_relative(double(pending_x_),double(pending_y_)))return false;
    pending_x_=pending_y_=0;next_motion_=now+interval;return true;
}
bool HostCapturePump::update_pointer_settings(hop::PointerSettings settings,std::uint16_t rate,std::uint64_t now) {
    if(!started_capture_ || !bridge_ || now<last_now_ || rate<20 || rate>1000 || !hop::valid_pointer_settings(settings))return false;
    if(!flush_motion(now) || !bridge_->set_pointer_settings(settings))return fail();
    motion_rate_=rate;last_now_=now;return true;
}
bool HostCapturePump::step(std::uint64_t now) {
    if(!started_capture_ || now<last_now_ || session_.phase()!=SessionPhase::active)return false;
    last_now_=now;
    if(source_.overflow() || source_.emergency())return fail();
    const bool active=source_.active();
    if(active)seen_active_=true; else if(seen_active_ || now-started_>=1000)return fail();
    CaptureEvent event{};
    for(unsigned i=0;i<256 && source_.poll(event);++i) {
        if(!active)continue; // passive edge samples precede activation
        if(event.kind==CaptureKind::move) {
            const auto dx=std::int64_t(event.x)-anchor_.x,dy=std::int64_t(event.y)-anchor_.y;
            if(std::abs(dx)>1000000 || std::abs(dy)>1000000 || std::abs(pending_x_+dx)>1000000 || std::abs(pending_y_+dy)>1000000)return fail();
            const bool at_edge=(bridge_->x()==0 && dx<0) || (bridge_->x()==65535 && dx>0) || (bridge_->y()==0 && dy<0) || (bridge_->y()==65535 && dy>0);
            if(route_edge_ && at_edge && source_.inputs_clear()) {
                if(!flush_motion(now))return fail();
                auto target=route_edge_(screen_,bridge_->x(),bridge_->y(),static_cast<std::int32_t>(dx),static_cast<std::int32_t>(dy));
                if(target && source_.inputs_clear()) {
                    if(auto* local=std::get_if<LocalDestination>(&*target))original_={local->x,local->y};
                    if(!session_.release(0))return fail();
                    if(auto* peer=std::get_if<PeerDestination>(&*target))handoff_=std::move(*peer);
                    return true;
                }
            }
            pending_x_+=dx;pending_y_+=dy;
            if(now>=next_motion_ && !flush_motion(now))return fail();
        } else {
            if(!flush_motion(now) || !bridge_->captured(event))return fail();
        }
    }
    if(now>=next_motion_ && !flush_motion(now))return fail();
    return true;
}
bool HostCapturePump::deactivate() {
    if(!started_capture_)return true;
    pending_x_=pending_y_=0;
    if(!deactivation_requested_) deactivation_requested_=source_.deactivate();
    if(!restored_)restored_=source_.restore_cursor(original_);
    return deactivation_requested_ && restored_;
}
bool HostCapturePump::drain() {
    if(!started_capture_)return true;
    if(source_.active() || !source_.drained())return false;
    if(!source_.stop())return false;
    started_capture_=false; bridge_.reset(); return restored_;
}
}
