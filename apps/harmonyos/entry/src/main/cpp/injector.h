#pragma once
#include "protocol.h"
#include "generated/keymap.h"
#include "generated/hid_keymap.h"
#include <multimodalinput/oh_input_manager.h>
#include <hilog/log.h>
#include <atomic>
#include <set>
#include <cmath>
#include <time.h>

namespace hop {
// Only RequestAuthorization runs on UI; every event and release is on the ordered network worker.
class Injector {
    std::set<int32_t> keys_;
    uint16_t buttons_=0; int x_=0,y_=0,width_=1,height_=1;
    int32_t displayId_=-1, originX_=0, originY_=0;
    bool capsKnown_=false,originalCaps_=false,currentCaps_=false,lastCapsQueryOK_=false;
    bool axisActive_[2]={false,false};
    inline static std::atomic<int> status_{UNAUTHORIZED};
    static void Authorized(Input_InjectionStatus s) { status_.store(int(s)); }
    static int32_t ButtonCode(int b) { static const int32_t codes[]={-1,MOUSE_BUTTON_LEFT,MOUSE_BUTTON_RIGHT,MOUSE_BUTTON_MIDDLE,MOUSE_BUTTON_BACK,MOUSE_BUTTON_FORWARD}; return codes[b]; }
    static int64_t ActionTime(){timespec ts{};clock_gettime(CLOCK_MONOTONIC,&ts);return int64_t(ts.tv_sec)*1000000+ts.tv_nsec/1000;}
    bool Mouse(int action,int button=MOUSE_BUTTON_NONE,int axis=0,float value=0) {
        Input_MouseEvent* e=OH_Input_CreateMouseEvent(); if(!e) return false;
        // The primary-only legacy path keeps its known zero-coordinate
        // workaround. In v2, negative global coordinates are valid.
        int dx=x_, dy=y_;
        if(displayId_<0) {
            dx=x_<=0?1:x_;dy=y_<=0?1:y_;
            if(width_>1&&dx>=width_)dx=width_-1;if(height_>1&&dy>=height_)dy=height_-1;
        } else {
            // Some PCs reject a global axis equal to zero as unset. Only
            // nudge inside this display, never across into another target.
            if(originX_+dx==0 && width_>1)dx+=(dx+1<width_?1:-1);
            if(originY_+dy==0 && height_>1)dy+=(dy+1<height_?1:-1);
            OH_Input_SetMouseEventDisplayId(e,displayId_);
        }
        OH_Input_SetMouseEventAction(e,action);
        OH_Input_SetMouseEventGlobalX(e,originX_+dx);OH_Input_SetMouseEventGlobalY(e,originY_+dy);
        OH_Input_SetMouseEventDisplayX(e,dx);OH_Input_SetMouseEventDisplayY(e,dy);
        OH_Input_SetMouseEventActionTime(e,ActionTime());
        // SDK has no pressed-buttons mask setter. A MOVE carries a held button and service maintains ordered downs/ups.
        if(action==MOUSE_ACTION_MOVE&&button==MOUSE_BUTTON_NONE) for(int b=1;b<=5;++b) if(buttons_&(1<<(b-1))) {button=ButtonCode(b);break;}
        OH_Input_SetMouseEventButton(e,button);
        if(action>=MOUSE_ACTION_AXIS_BEGIN) {OH_Input_SetMouseEventAxisType(e,axis);OH_Input_SetMouseEventAxisValue(e,value);}
        int result=OH_Input_InjectMouseEventGlobal(e);OH_Input_DestroyMouseEvent(&e);
        if(result!=INPUT_SUCCESS) OH_LOG_Print(LOG_APP, LOG_ERROR, 0x0000, "Hop", "mouse inject %{public}d action %{public}d x %{public}d y %{public}d", result, action, x_, y_);
        return result==INPUT_SUCCESS;
    }
    bool RawKey(int32_t code,bool down) {
        Input_KeyEvent* e=OH_Input_CreateKeyEvent();if(!e)return false;
        if(displayId_>=0)OH_Input_SetKeyEventDisplayId(e,displayId_);
        OH_Input_SetKeyEventKeyCode(e,code);OH_Input_SetKeyEventAction(e,down?KEY_ACTION_DOWN:KEY_ACTION_UP);
        OH_Input_SetKeyEventActionTime(e,ActionTime());
        int result=OH_Input_InjectKeyEvent(e);OH_Input_DestroyKeyEvent(&e);return result==INPUT_SUCCESS;
    }
    bool SetKey(int32_t code,bool down,bool repeat=false) {
        if(down) {if(keys_.count(code)&&!repeat)return true;
            // Allocate/record before issuing a down. Even a failed SDK call is
            // retained for best-effort release; bad_alloc cannot lose a live down.
            keys_.insert(code);if(!RawKey(code,true))return false;}
        else if(keys_.count(code)) {if(!RawKey(code,false))return false;keys_.erase(code);} return true;
    }
public:
    static int RequestAuthorization() {
        Input_InjectionStatus s=UNAUTHORIZED;
        if(OH_Input_QueryAuthorizedStatus(&s)==INPUT_SUCCESS&&s==AUTHORIZED) {status_=AUTHORIZED;return INPUT_INJECTION_AUTHORIZED;}
        status_=AUTHORIZING; auto r=OH_Input_RequestInjection(Authorized);
        if(r==INPUT_INJECTION_AUTHORIZED)status_=AUTHORIZED;
        else if(r!=INPUT_SUCCESS&&r!=INPUT_INJECTION_AUTHORIZING)status_=UNAUTHORIZED;
        return int(r);
    }
    static int Authorization() {
        Input_InjectionStatus s=UNAUTHORIZED;
        if(OH_Input_QueryAuthorizedStatus(&s)!=INPUT_SUCCESS)s=UNAUTHORIZED;
        status_=int(s); return int(s);
    }
    void Dimensions(int width,int height) {width_=width;height_=height;}
    bool TargetDisplay(int32_t id,int32_t x,int32_t y,int width,int height) {
        if(!keys_.empty()||buttons_||axisActive_[0]||axisActive_[1]||capsKnown_)return false;
        displayId_=id;originX_=x;originY_=y;Dimensions(width,height);
        return true;
    }
    int x()const{return x_;} int y()const{return y_;} int width()const{return width_;} int height()const{return height_;}
    bool Position(uint16_t x,uint16_t y) {x_=int(std::llround(double(x)*(width_-1)/65535.0));y_=int(std::llround(double(y)*(height_-1)/65535.0));return Mouse(MOUSE_ACTION_MOVE);}
    bool Modifiers(uint16_t mask) {
        static constexpr uint16_t mac[]={56,60,59,62,58,61,55,54};
        for(int i=0;i<8;++i)if(!SetKey(MapMacKey(mac[i]),mask&(1<<i)))return false;
        int32_t lock=0;
        lastCapsQueryOK_=OH_Input_GetFunctionKeyState(1,&lock)==INPUT_SUCCESS;
        if(lastCapsQueryOK_) {
            if(!capsKnown_){capsKnown_=true;originalCaps_=bool(lock);}
            currentCaps_=bool(lock);bool wanted=mask&256;
            if(currentCaps_!=wanted){if(!SetKey(MapMacKey(57),true))return false;currentCaps_=wanted;if(!SetKey(MapMacKey(57),false))return false;}
        } else capsKnown_=false; // no reliable state to restore if the platform cannot query it
        return true;
    }
    bool Buttons(uint16_t mask) {
        for(int b=1;b<=5;++b){uint16_t bit=uint16_t(1<<(b-1));bool old=buttons_&bit,down=mask&bit;if(old==down)continue;
            if(down)buttons_|=bit; // track before the SDK call, including uncertain failures
            if(!Mouse(down?MOUSE_ACTION_BUTTON_DOWN:MOUSE_ACTION_BUTTON_UP,ButtonCode(b)))return false;
            if(!down)buttons_&=uint16_t(~bit);
        }return true;
    }
    bool Key(uint16_t mac,uint16_t modifiers,bool down,bool repeat) {
        if(!Modifiers(modifiers))return false;
        if(mac==57) {
            if(lastCapsQueryOK_)return true; // synchronized above as a latch
            // On some PCs there is no physical keyboard, so the SDK state query
            // fails. Mac sends one flagsChanged per latch transition; relay that
            // transition as a balanced tap instead of silently dropping it.
            const int code=MapMacKey(57);
            return SetKey(code,true)&&SetKey(code,false);
        }
        int code=MapMacKey(mac);return code<0 ? true : SetKey(code,down,repeat);
    }
    // v2 uses USB HID keyboard-page usage rather than a Mac virtual code.
    // Call only after session, focus and capability checks succeed.
    bool KeyHid(uint16_t usage,uint16_t modifiers,bool down,bool repeat) {
        if(!Modifiers(modifiers))return false;
        if(usage==0x39) {
            if(lastCapsQueryOK_ || !down)return true;
            // HID has a separate up event. Toggle once on down when the SDK
            // cannot query the lock state, then ignore the corresponding up.
            const int code=MapHidKey(usage);
            return code>=0 && SetKey(code,true) && SetKey(code,false);
        }
        int code=MapHidKey(usage);return code<0 ? true : SetKey(code,down,repeat);
    }
    bool Wheel(int32_t dx,int32_t dy) {
        // Protocol positive Y is down; SDK positive vertical is forward/up. Horizontal positive is right.
        // One physical wheel detent carries 120 protocol units. HarmonyOS PC's
        // axis is consumed on a finer scale; a 1.0 axis value produced only a
        // barely visible move on the user's device. Keep a conservative cap so
        // an abnormal packet cannot cause an uncontrolled enormous jump.
        for(int axis=0;axis<2;++axis){int32_t v=axis==0?dy:dx;if(!v)continue;
            float step=std::fmax(-1000.0f,std::fmin(1000.0f,float(v)/120.0f*22.5f*(axis==0?-1.0f:1.0f)));
            // The first hardware axis event carries the scroll value on BEGIN;
            // a zero-valued BEGIN can be ignored by some PC applications.
            if(!Mouse(MOUSE_ACTION_AXIS_BEGIN,MOUSE_BUTTON_NONE,axis,step))return false;
            axisActive_[axis]=true;
            bool updated=Mouse(MOUSE_ACTION_AXIS_UPDATE,MOUSE_BUTTON_NONE,axis,step);
            bool ended=Mouse(MOUSE_ACTION_AXIS_END,MOUSE_BUTTON_NONE,axis,0);
            if(ended)axisActive_[axis]=false;
            if(!updated||!ended)return false;
        }return true;
    }
    bool Release() noexcept {
        // Keep uncertain downs so a later best-effort retry can release them.
        // A failed release must never be acknowledged as successful by v2.
        bool ok=true;
        for(int axis=0;axis<2;++axis)if(axisActive_[axis]) {
            if(Mouse(MOUSE_ACTION_AXIS_END,MOUSE_BUTTON_NONE,axis,0))axisActive_[axis]=false;else ok=false;
        }
        for(auto it=keys_.begin();it!=keys_.end();) {
            if(RawKey(*it,false))it=keys_.erase(it);else {ok=false;++it;}
        }
        for(int b=1;b<=5;++b)if(buttons_&(1<<(b-1))) {
            if(Mouse(MOUSE_ACTION_BUTTON_UP,ButtonCode(b)))buttons_&=uint16_t(~(1<<(b-1)));else ok=false;
        }
        if(capsKnown_&&currentCaps_!=originalCaps_) {
            // Always attempt the balancing up even if the down failed. A failed
            // toggle is reported; querying again is required before any retry.
            bool down=RawKey(MapMacKey(57),true),up=RawKey(MapMacKey(57),false);
            if(!down||!up)ok=false;
        }
        capsKnown_=false;
        return ok;
    }
};
} // namespace hop
