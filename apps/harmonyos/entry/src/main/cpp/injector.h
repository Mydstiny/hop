#pragma once
#include "protocol.h"
#include "generated/keymap.h"
#include <multimodalinput/oh_input_manager.h>
#include <atomic>
#include <set>
#include <cmath>

namespace hop {
// Only RequestAuthorization runs on UI; every event and release is on the ordered network worker.
class Injector {
    std::set<int32_t> keys_;
    uint16_t buttons_=0; int x_=0,y_=0,width_=1,height_=1;
    bool capsKnown_=false,originalCaps_=false,currentCaps_=false;
    bool axisActive_[2]={false,false};
    inline static std::atomic<int> status_{UNAUTHORIZED};
    static void Authorized(Input_InjectionStatus s) { status_.store(int(s)); }
    static int32_t ButtonCode(int b) { static const int32_t codes[]={-1,MOUSE_BUTTON_LEFT,MOUSE_BUTTON_RIGHT,MOUSE_BUTTON_MIDDLE,MOUSE_BUTTON_BACK,MOUSE_BUTTON_FORWARD}; return codes[b]; }
    bool Mouse(int action,int button=MOUSE_BUTTON_NONE,int axis=0,float value=0) {
        Input_MouseEvent* e=OH_Input_CreateMouseEvent(); if(!e) return false;
        OH_Input_SetMouseEventAction(e,action); OH_Input_SetMouseEventGlobalX(e,x_);OH_Input_SetMouseEventGlobalY(e,y_);
        // SDK has no pressed-buttons mask setter. A MOVE carries a held button and service maintains ordered downs/ups.
        if(action==MOUSE_ACTION_MOVE&&button==MOUSE_BUTTON_NONE) for(int b=1;b<=5;++b) if(buttons_&(1<<(b-1))) {button=ButtonCode(b);break;}
        OH_Input_SetMouseEventButton(e,button);
        if(action>=MOUSE_ACTION_AXIS_BEGIN) {OH_Input_SetMouseEventAxisType(e,axis);OH_Input_SetMouseEventAxisValue(e,value);}
        int result=OH_Input_InjectMouseEventGlobal(e);OH_Input_DestroyMouseEvent(&e);return result==INPUT_SUCCESS;
    }
    bool RawKey(int32_t code,bool down) {
        Input_KeyEvent* e=OH_Input_CreateKeyEvent();if(!e)return false;
        OH_Input_SetKeyEventKeyCode(e,code);OH_Input_SetKeyEventAction(e,down?KEY_ACTION_DOWN:KEY_ACTION_UP);
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
    int x()const{return x_;} int y()const{return y_;} int width()const{return width_;} int height()const{return height_;}
    bool Position(uint16_t x,uint16_t y) {x_=int(std::llround(double(x)*(width_-1)/65535.0));y_=int(std::llround(double(y)*(height_-1)/65535.0));return Mouse(MOUSE_ACTION_MOVE);}
    bool Modifiers(uint16_t mask) {
        static constexpr uint16_t mac[]={56,60,59,62,58,61,55,54};
        for(int i=0;i<8;++i)if(!SetKey(MapMacKey(mac[i]),mask&(1<<i)))return false;
        int32_t lock=0;
        if(OH_Input_GetFunctionKeyState(1,&lock)!=INPUT_SUCCESS)return false;
        if(!capsKnown_){capsKnown_=true;originalCaps_=bool(lock);}
        currentCaps_=bool(lock);bool wanted=mask&256;
        if(currentCaps_!=wanted){if(!SetKey(MapMacKey(57),true))return false;currentCaps_=wanted;if(!SetKey(MapMacKey(57),false))return false;}
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
        if(mac==57)return true; // CapsLock is synchronized as a lock, never held.
        int code=MapMacKey(mac);return code<0 ? true : SetKey(code,down,repeat);
    }
    bool Wheel(int32_t dx,int32_t dy) {
        // Protocol positive Y is down; SDK positive vertical is forward/up. Horizontal positive is right.
        for(int axis=0;axis<2;++axis){int32_t v=axis==0?dy:dx;if(!v)continue;float step=float(v)/120.0f*(axis==0?-1.0f:1.0f);
            if(!Mouse(MOUSE_ACTION_AXIS_BEGIN,MOUSE_BUTTON_NONE,axis,0))return false;
            axisActive_[axis]=true;
            bool updated=Mouse(MOUSE_ACTION_AXIS_UPDATE,MOUSE_BUTTON_NONE,axis,step);
            bool ended=Mouse(MOUSE_ACTION_AXIS_END,MOUSE_BUTTON_NONE,axis,0);
            if(ended)axisActive_[axis]=false;
            if(!updated||!ended)return false;
        }return true;
    }
    void Release() noexcept {
        // Best effort even if authorization was revoked: never clear a down without attempting its release.
        for(int axis=0;axis<2;++axis)if(axisActive_[axis]){Mouse(MOUSE_ACTION_AXIS_END,MOUSE_BUTTON_NONE,axis,0);axisActive_[axis]=false;}
        for(int code:keys_)RawKey(code,false);keys_.clear();
        for(int b=1;b<=5;++b)if(buttons_&(1<<(b-1)))Mouse(MOUSE_ACTION_BUTTON_UP,ButtonCode(b));buttons_=0;
        if(capsKnown_&&currentCaps_!=originalCaps_){RawKey(MapMacKey(57),true);RawKey(MapMacKey(57),false);}capsKnown_=false;
    }
};
} // namespace hop
