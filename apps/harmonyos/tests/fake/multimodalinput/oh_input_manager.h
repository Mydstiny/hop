#pragma once
// Test-only SDK surface. No system input library is linked by host tests.
#include <atomic>
#include <mutex>
#include <set>
#include <vector>
#include <cstdint>
enum Input_InjectionStatus { UNAUTHORIZED=0, AUTHORIZING=1, AUTHORIZED=2 };
enum Input_Result { INPUT_SUCCESS=0, INPUT_INJECTION_AUTHORIZING=3900005, INPUT_INJECTION_AUTHORIZED=3900007 };
enum { MOUSE_BUTTON_NONE=-1,MOUSE_BUTTON_LEFT=0,MOUSE_BUTTON_MIDDLE=1,MOUSE_BUTTON_RIGHT=2,MOUSE_BUTTON_FORWARD=3,MOUSE_BUTTON_BACK=4,
 MOUSE_ACTION_MOVE=1,MOUSE_ACTION_BUTTON_DOWN=2,MOUSE_ACTION_BUTTON_UP=3,MOUSE_ACTION_AXIS_BEGIN=4,MOUSE_ACTION_AXIS_UPDATE=5,MOUSE_ACTION_AXIS_END=6,
 KEY_ACTION_DOWN=1,KEY_ACTION_UP=2 };
struct Input_MouseEvent {int action=0,x=0,y=0,display_x=0,display_y=0,display_id=-1,button=-1,axis=0;float value=0;};
struct Input_KeyEvent {int code=0,action=0,display_id=-1;};
namespace fake {
inline std::atomic<int> authorization{AUTHORIZED};
inline std::mutex mutex;
inline std::set<int> keys,buttons;
inline std::vector<Input_MouseEvent> mouse;
inline std::vector<Input_KeyEvent> key;
inline int caps=0, created=0,destroyed=0, releaseAttempts=0;
inline bool failCapsQuery=false;
inline bool failNext=false;
inline int failMouseAction=-1;
inline int unavailableDisplay=-2;
inline void Reset(){std::lock_guard<std::mutex> l(mutex);authorization=AUTHORIZED;keys.clear();buttons.clear();mouse.clear();key.clear();caps=0;created=destroyed=releaseAttempts=0;failNext=false;failCapsQuery=false;failMouseAction=-1;unavailableDisplay=-2;}
inline bool Empty(){std::lock_guard<std::mutex> l(mutex);return keys.empty()&&buttons.empty();}
}
inline Input_Result OH_Input_QueryAuthorizedStatus(Input_InjectionStatus* s){*s=Input_InjectionStatus(fake::authorization.load());return INPUT_SUCCESS;}
inline Input_Result OH_Input_RequestInjection(void(*cb)(Input_InjectionStatus)){fake::authorization=AUTHORIZED;cb(AUTHORIZED);return INPUT_SUCCESS;}
inline Input_Result OH_Input_GetFunctionKeyState(int32_t,int32_t* s){std::lock_guard<std::mutex> l(fake::mutex);if(fake::failCapsQuery)return Input_Result(1);*s=fake::caps;return INPUT_SUCCESS;}
inline Input_MouseEvent* OH_Input_CreateMouseEvent(){std::lock_guard<std::mutex> l(fake::mutex);++fake::created;return new Input_MouseEvent;}
inline void OH_Input_DestroyMouseEvent(Input_MouseEvent** e){std::lock_guard<std::mutex> l(fake::mutex);++fake::destroyed;delete *e;*e=nullptr;}
inline void OH_Input_SetMouseEventAction(Input_MouseEvent* e,int a){e->action=a;}
inline void OH_Input_SetMouseEventGlobalX(Input_MouseEvent* e,int a){e->x=a;}
inline void OH_Input_SetMouseEventGlobalY(Input_MouseEvent* e,int a){e->y=a;}
inline void OH_Input_SetMouseEventDisplayX(Input_MouseEvent* e,int a){e->display_x=a;}
inline void OH_Input_SetMouseEventDisplayY(Input_MouseEvent* e,int a){e->display_y=a;}
inline void OH_Input_SetMouseEventDisplayId(Input_MouseEvent* e,int a){e->display_id=a;}
inline void OH_Input_SetMouseEventActionTime(Input_MouseEvent*,int64_t){}
inline void OH_Input_SetKeyEventActionTime(Input_KeyEvent*,int64_t){}
inline void OH_Input_SetMouseEventButton(Input_MouseEvent* e,int a){e->button=a;}
inline void OH_Input_SetMouseEventAxisType(Input_MouseEvent* e,int a){e->axis=a;}
inline void OH_Input_SetMouseEventAxisValue(Input_MouseEvent* e,float a){e->value=a;}
inline int OH_Input_InjectMouseEventGlobal(const Input_MouseEvent* e){std::lock_guard<std::mutex> l(fake::mutex);fake::mouse.push_back(*e);if(e->action==MOUSE_ACTION_BUTTON_UP)++fake::releaseAttempts;
 if(fake::authorization!=AUTHORIZED||e->display_id==fake::unavailableDisplay)return 1;if(fake::failNext){fake::failNext=false;return 1;}if(e->action==fake::failMouseAction){fake::failMouseAction=-1;return 1;}if(e->action==MOUSE_ACTION_BUTTON_DOWN)fake::buttons.insert(e->button);if(e->action==MOUSE_ACTION_BUTTON_UP)fake::buttons.erase(e->button);return 0;}
inline Input_KeyEvent* OH_Input_CreateKeyEvent(){std::lock_guard<std::mutex> l(fake::mutex);++fake::created;return new Input_KeyEvent;}
inline void OH_Input_DestroyKeyEvent(Input_KeyEvent** e){std::lock_guard<std::mutex> l(fake::mutex);++fake::destroyed;delete *e;*e=nullptr;}
inline void OH_Input_SetKeyEventKeyCode(Input_KeyEvent* e,int a){e->code=a;}
inline void OH_Input_SetKeyEventAction(Input_KeyEvent* e,int a){e->action=a;}
inline void OH_Input_SetKeyEventDisplayId(Input_KeyEvent* e,int a){e->display_id=a;}
inline int OH_Input_InjectKeyEvent(const Input_KeyEvent* e){std::lock_guard<std::mutex> l(fake::mutex);fake::key.push_back(*e);if(e->action==KEY_ACTION_UP)++fake::releaseAttempts;
 if(fake::authorization!=AUTHORIZED||e->display_id==fake::unavailableDisplay)return 1;if(fake::failNext){fake::failNext=false;return 1;}if(e->action==KEY_ACTION_DOWN){fake::keys.insert(e->code);if(e->code==2074)fake::caps=!fake::caps;}else fake::keys.erase(e->code);return 0;}
