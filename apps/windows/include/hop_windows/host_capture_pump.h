#pragma once
#include "hop_windows/host_input_bridge.h"
#include "hop_windows/live_layout.h"
#include <cstdint>
#include <functional>
namespace hop::windows {
struct CursorPoint { std::int32_t x,y; };
class CaptureSource {
public:
    virtual ~CaptureSource() = default;
    virtual bool inputs_clear() = 0;
    virtual bool prepare(CursorPoint& original,CursorPoint& anchor,bool& caps) = 0;
    virtual bool start() = 0;
    virtual bool activate() = 0;
    virtual bool deactivate() = 0;
    virtual bool active() const = 0;
    virtual bool drained() const = 0;
    virtual bool overflow() = 0;
    virtual bool emergency() = 0;
    virtual bool poll(CaptureEvent& event) = 0;
    virtual bool stop() = 0;
    virtual bool force_recovery() = 0;
    virtual bool restore_cursor(CursorPoint point) = 0;
};
// Real methods install low-level hooks and move the local cursor. They must be
// called only after explicit enablement and never in automated tests.
class WindowsCaptureSource final : public CaptureSource {
public:
    bool inputs_clear() override;
    bool prepare(CursorPoint& original,CursorPoint& anchor,bool& caps) override;
    bool start() override {return capture_.start();}
    bool activate() override {return capture_.request_activation();}
    bool deactivate() override {return capture_.request_deactivation();}
    bool active() const override {return capture_.active();}
    bool drained() const override {return capture_.drained();}
    bool overflow() override {return capture_.take_overflow();}
    bool emergency() override {return capture_.take_emergency();}
    bool poll(CaptureEvent& event) override {return capture_.poll(event);}
    bool stop() override {return capture_.stop();}
    bool force_recovery() override {return capture_.request_force_recovery();}
    bool restore_cursor(CursorPoint point) override;
private:
    InputCapture capture_;
};
// Serial session worker. Session's restore callback must call deactivate();
// the owner must continue calling drain() until pending physical releases end.
class HostCapturePump {
public:
    HostCapturePump(Session& session,CaptureSource& source) : session_(session),source_(source) {}
    bool begin(std::uint64_t screen,std::uint32_t width,std::uint32_t height,std::uint16_t x,std::uint16_t y,
               hop::PointerSettings settings,std::uint64_t now_ms,std::uint16_t motion_rate_hz=240);
    bool set_return_edge(std::function<std::optional<CursorPoint>(std::uint64_t,std::uint16_t,std::uint16_t,std::int32_t,std::int32_t)> callback);
    bool set_route_edge(std::function<std::optional<LiveDestination>(std::uint64_t,std::uint16_t,std::uint16_t,std::int32_t,std::int32_t)> callback);
    std::optional<PeerDestination> take_handoff();
    bool step(std::uint64_t now_ms);
    bool active() const {return started_capture_ && seen_active_;}
    bool deactivate();
    bool update_pointer_settings(hop::PointerSettings settings,std::uint16_t motion_rate_hz,std::uint64_t now_ms);
    bool drain();
private:
    bool fail();
    bool flush_motion(std::uint64_t now_ms);
    Session& session_; CaptureSource& source_; std::unique_ptr<HostInputBridge> bridge_;
    CursorPoint original_{},anchor_{}; std::uint64_t started_=0,last_now_=0,next_motion_=0;
    std::int64_t pending_x_=0,pending_y_=0;std::uint16_t motion_rate_=240;
    bool started_capture_=false,seen_active_=false,restored_=false,deactivation_requested_=false;
    std::uint64_t screen_=0;
    std::function<std::optional<LiveDestination>(std::uint64_t,std::uint16_t,std::uint16_t,std::int32_t,std::int32_t)> route_edge_;
    std::optional<PeerDestination> handoff_;
};
}
