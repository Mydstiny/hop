#ifndef UNICODE
#define UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "hop_windows/ui_state.h"
#include "hop_windows/layout_view_model.h"
#include "hop_windows/pointer_settings.h"
#include "hop_windows/layout_store.h"
#ifdef HOP_WINDOWS_EXPERIMENTAL_TLS
#include "hop_windows/live_layout_store.h"
#include "hop_windows/pairing_runtime.h"
#include "hop_windows/pairing_ui.h"
#include "hop_windows/displays.h"
#include "hop_windows/clipboard_windows.h"
#include "hop_windows/discovery.h"
#include <atomic>
#include <memory>
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <cwchar>
#include <algorithm>
#include <initializer_list>
#include <limits>
#include <cmath>
#include <windowsx.h>

namespace {
using namespace hop::windows;
constexpr UINT tray_message = WM_APP + 81;
#ifdef HOP_WINDOWS_EXPERIMENTAL_TLS
constexpr UINT pairing_message=WM_APP+82, pair_listen_id=201,pair_connect_id=202,peer_settings_id=203,screen_combo_id=204,discovery_id=205,edge_id=206,peer_list_id=207;
constexpr UINT reap_timer=91, panel_timer=92, add_id=208, options_id=209, disconnect_id=210, manual_id=211;
#endif
constexpr UINT show_id = 101, enable_id = 102, pause_id = 103, quit_id = 104, layout_id = 105, angle_id=212;
constexpr UINT tray_id = 1;
const wchar_t window_class[] = L"HopNativeStatusWindow";
const wchar_t layout_class[] = L"HopNativeLayoutWindow";
// The installer and uninstaller reserve the same lifetime in this session.
const wchar_t lifecycle_mutex[] = L"Local\\HopNativeLifecycle-{6D941BF5-C595-430E-8242-A63E8429D60A}";
struct App {
    UiLanguage language = UiLanguage::english;
    UiState state;
    HWND window = nullptr, layout_window = nullptr, status = nullptr, explanation = nullptr, enable = nullptr, pause = nullptr, layout = nullptr;
    UINT taskbar_created = 0;
    bool tray_added = false;
    LayoutEditor editor;
    RegistryLayoutStorage layout_storage;
    bool layout_storage_ok = true;
    RegistryPointerSettingsStore pointer_store;
    TargetPointerSettings pointer_settings{pointer_store};
    bool dragging = false;
    POINT drag_start{}, drag_current{};
    double drag_scale = 0;
    bool spatial = true;
    HFONT headline=nullptr, body=nullptr;
    HBRUSH page_brush=nullptr, card_brush=nullptr;
    COLORREF page_color=RGB(246,244,251), card_color=RGB(255,255,255), ink_color=RGB(36,33,55);
    std::string drag_device, drag_screen;
    std::int32_t screen_x = 0, screen_y = 0;
#ifdef HOP_WINDOWS_EXPERIMENTAL_TLS
    WindowsCredentialBackend credentials;
    std::unique_ptr<InputSink> receiver_sink;
    std::unique_ptr<ReceiverSessionAdapter> receiver_adapter;
    WindowsCaptureSource host_capture;
    std::atomic<bool> allow_receiver{false};
    std::atomic<HWND> event_window{nullptr};
    std::unique_ptr<PairingRuntime> runtime;
    Discovery discovery;
    HWND address=nullptr,port=nullptr,host_role=nullptr,receiver_role=nullptr,listen=nullptr,connect=nullptr,screen_combo=nullptr,peer_settings=nullptr,clipboard_checkbox=nullptr,edge=nullptr;
    HWND peer_list=nullptr, brand=nullptr, add=nullptr, options=nullptr, disconnect=nullptr;
    HWND address_label=nullptr, port_label=nullptr, discover=nullptr, manual=nullptr;
    bool show_add=false, show_options=false, show_manual=false, animating=false;
    int panel_height=0;
    std::vector<RemoteScreen> peer_displays;
    std::string screen_peer;
    std::vector<LiveScreen> local_geometry, transport_geometry;
    std::vector<LivePeerScreens> peer_geometry;
    std::vector<hop::Screen> native_geometry;
    std::string live_layout_key;
    std::optional<LayoutEditor> offline_editor;
    bool receiver_mode=false,control_pending=false,edge_armed=false,auto_edge_desired=false,peer_change_pending=false;std::uint32_t runtime_generation=0;
#endif
};
const UiStrings& strings(const App& app) { return ui_strings(app.language); }
#ifdef HOP_WINDOWS_EXPERIMENTAL_TLS
const wchar_t* localized(const App& app,const wchar_t* zh,const wchar_t* en);
constexpr int panel_width=420;
void adjust_panel(App& app,int height) {
    RECT desired{0,0,panel_width,height};
    AdjustWindowRectEx(&desired,static_cast<DWORD>(GetWindowLongPtrW(app.window,GWL_STYLE)),FALSE,
        static_cast<DWORD>(GetWindowLongPtrW(app.window,GWL_EXSTYLE)));
    SetWindowPos(app.window,nullptr,0,0,desired.right-desired.left,desired.bottom-desired.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
}
bool can_change_peer(const App& app) {
    return app.runtime && !app.receiver_mode && app.state.connected() && app.runtime->state()==PairingState::connected &&
        !app.state.sharing() && !app.edge_armed && !app.auto_edge_desired && !app.control_pending && !app.peer_change_pending;
}
#endif
void update_controls(App& app) {
    const auto& s = strings(app);
    SetWindowTextW(app.status, app.state.sharing() ? s.sharing : app.state.connected() ? s.connected : s.disconnected);
#ifdef HOP_WINDOWS_EXPERIMENTAL_TLS
    const bool connected=app.state.connected();
    const bool sharing=app.state.sharing() || app.auto_edge_desired;
    if(sharing)SetWindowTextW(app.status,localized(app,L"键鼠共享已开启",L"Keyboard and mouse sharing is on"));
    else if(app.runtime && !connected)SetWindowTextW(app.status,localized(app,L"正在连接电脑…",L"Connecting…"));
    if(app.add)SetWindowTextW(app.add,app.show_add ? localized(app,L"收起",L"Done") : localized(app,L"添加电脑…",L"Add computer…"));
    if(app.options)SetWindowTextW(app.options,app.show_options ? localized(app,L"收起设置",L"Hide settings") : localized(app,L"设置…",L"Settings…"));
    if(app.disconnect)SetWindowTextW(app.disconnect,connected ? localized(app,L"断开连接",L"Disconnect") : localized(app,L"取消",L"Cancel"));
#else
    SetWindowTextW(app.explanation, s.unavailable);
#endif
    SetWindowTextW(app.enable, s.enable); SetWindowTextW(app.pause, s.pause);
    SetWindowTextW(app.layout, s.layout);
    bool can_enable=app.state.can_enable();
#ifdef HOP_WINDOWS_EXPERIMENTAL_TLS
    if(app.runtime && !app.receiver_mode)can_enable=can_enable && !app.peer_displays.empty() && !app.control_pending && !app.auto_edge_desired && !app.peer_change_pending;
#endif
    EnableWindow(app.enable, can_enable);
    bool can_pause=app.state.can_pause();
#ifdef HOP_WINDOWS_EXPERIMENTAL_TLS
    can_pause=can_pause || app.auto_edge_desired;EnableWindow(app.layout,!app.auto_edge_desired && !app.state.sharing());
    const bool peer_idle=can_change_peer(app);
    if(app.edge)EnableWindow(app.edge,peer_idle && app.offline_editor.has_value());
    if(app.listen)EnableWindow(app.listen,!app.runtime || peer_idle);
    if(app.connect)EnableWindow(app.connect,!app.runtime || peer_idle);
    if(app.peer_list)EnableWindow(app.peer_list,peer_idle && app.runtime->connected_peers().size()>1);
    const auto show=[](HWND control,bool visible){if(control)ShowWindow(control,visible ? SW_SHOWNA : SW_HIDE);};
    show(app.add,!app.runtime || peer_idle);
    show(app.disconnect,app.runtime && !sharing && (!peer_idle || app.show_options));
    if(app.disconnect)MoveWindow(app.disconnect,280,peer_idle ? 248 : 156,119,32,TRUE);
    show(app.pause,sharing);
    show(app.layout,connected && !app.receiver_mode && app.offline_editor.has_value());
    show(app.edge,connected && !app.receiver_mode && !sharing);
    show(app.enable,connected && !sharing && (app.receiver_mode || app.show_options));
    if(app.edge)SetWindowTextW(app.edge,localized(app,L"开始共享",L"Start sharing"));
    if(app.layout)SetWindowTextW(app.layout,localized(app,L"空间布局…",L"Arrange screens…"));
    if(app.enable && app.receiver_mode)SetWindowTextW(app.enable,localized(app,L"允许键鼠控制",L"Allow control"));
    const bool picker=app.show_add && (!app.runtime || peer_idle);
    const bool manual=picker && app.show_manual;
    for(const auto control:{app.host_role,app.receiver_role,app.listen,app.discover,app.manual})show(control,picker);
    for(const auto control:{app.address_label,app.port_label,app.address,app.port,app.connect})show(control,manual);
    show(app.clipboard_checkbox,app.show_options);
    show(app.peer_settings,app.show_options && connected && !app.receiver_mode);
    show(app.peer_list,app.show_options && connected && !app.receiver_mode && app.runtime->connected_peers().size()>1);
    show(app.screen_combo,app.show_options && connected && !app.receiver_mode);
    if(app.enable)MoveWindow(app.enable,app.receiver_mode ? 21 : 280,app.receiver_mode ? 156 : 292,119,32,TRUE);
    if(app.clipboard_checkbox)MoveWindow(app.clipboard_checkbox,21,picker ? (manual ? 354 : 267) : 214,376,25,TRUE);
    const int panelY=picker ? (manual ? 388 : 301) : 248;
    if(app.peer_settings)MoveWindow(app.peer_settings,21,panelY,122,30,TRUE);
    if(app.peer_list)MoveWindow(app.peer_list,150,panelY,122,30,TRUE);
    if(app.screen_combo)MoveWindow(app.screen_combo,21,panelY+40,244,112,TRUE);
    if(app.enable && !app.receiver_mode)MoveWindow(app.enable,280,panelY+40,119,30,TRUE);
    app.panel_height=app.show_options ? panelY+(connected && !app.receiver_mode ? 82 : 0) : manual ? 363 : picker ? 274 : 199;
    if(!app.animating && app.window)adjust_panel(app,app.panel_height);
    if(app.window)InvalidateRect(app.window,nullptr,TRUE);
#endif
    EnableWindow(app.pause, can_pause);
}
#ifdef HOP_WINDOWS_EXPERIMENTAL_TLS
bool online_peer(App& app,const std::string& id) {
    return !id.empty() && app.runtime && app.runtime->state()==PairingState::connected &&
        app.runtime->peer_fingerprint()==id;
}
bool same_local_geometry(const std::vector<LiveScreen>& expected) {
    LocalDisplaySnapshot current;
    if (!enumerate_local_displays(current) || current.displays.size() != expected.size()) return false;
    return std::equal(expected.begin(), expected.end(), current.displays.begin(), [](const auto& screen, const auto& display) {
        const auto& bounds = display.bounds;
        return screen.id == display.id && screen.x == bounds.x && screen.y == bounds.y &&
            screen.width == bounds.width && screen.height == bounds.height;
    });
}
bool same_peer_geometry(const std::vector<LiveScreen>& expected, const std::vector<RemoteScreen>& current) {
    return expected.size() == current.size() &&
        std::equal(expected.begin(), expected.end(), current.begin(), [](const auto& screen, const auto& display) {
            return screen.id == display.id && screen.x == display.x && screen.y == display.y &&
                screen.width == display.width && screen.height == display.height;
        });
}
bool current_live_geometry(App& app) {
    if(!app.offline_editor || !app.runtime || app.runtime->state()!=PairingState::connected ||
       !same_local_geometry(app.local_geometry))return false;
    const auto current=app.runtime->connected_peers();
    if(current.size()!=app.peer_geometry.size())return false;
    for(const auto& expected:app.peer_geometry) {
        const auto peer=std::find_if(current.begin(),current.end(),[&](const auto& candidate){return candidate.fingerprint==expected.fingerprint;});
        if(peer==current.end() || !same_peer_geometry(expected.screens,peer->displays))return false;
    }
    return true;
}
RegistryLayoutStorage live_storage(const std::string& fingerprint) {
    return RegistryLayoutStorage(L"Live-" + std::wstring(fingerprint.begin(), fingerprint.end()));
}
#endif
bool layout_editable(const App& app) {
#ifdef HOP_WINDOWS_EXPERIMENTAL_TLS
    return !app.edge_armed && !app.auto_edge_desired && !app.state.sharing() && !app.control_pending && !app.peer_change_pending &&
        (!app.runtime || app.receiver_mode || app.offline_editor.has_value());
#else
    (void)app;
    return true;
#endif
}
bool persist_layout(App& app) {
#ifdef HOP_WINDOWS_EXPERIMENTAL_TLS
    if (app.offline_editor) {
        if (!current_live_geometry(app)) return false;
        auto storage = live_storage(app.live_layout_key);
        return save_live_layout(storage, app.editor, app.native_geometry);
    }
#endif
    return app.layout_storage_ok && save_layout(app.layout_storage,app.editor);
}
bool add_tray(App& app) {
    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon); icon.hWnd = app.window; icon.uID = tray_id;
    icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    icon.uCallbackMessage = tray_message;
    icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    // The current skeleton has no active transport and displays that honestly.
    lstrcpynW(icon.szTip, strings(app).disconnected, static_cast<int>(sizeof(icon.szTip)/sizeof(icon.szTip[0])));
    app.tray_added = Shell_NotifyIconW(NIM_ADD, &icon) != FALSE;
    return app.tray_added;
}
void remove_tray(App& app) {
    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon); icon.hWnd = app.window; icon.uID = tray_id;
    Shell_NotifyIconW(NIM_DELETE, &icon);
    app.tray_added = false;
}
void show_status(App& app) { update_controls(app); ShowWindow(app.window, SW_SHOWNORMAL); SetForegroundWindow(app.window); }
LayoutView layout_view(App& app,HWND window) {
    RECT area{}; GetClientRect(window,&area); return project_layout(app.editor,area.right,area.bottom);
}
// The spatial view tilts the routing plane and extrudes the monitor body.
struct ScenePoint { double x,y; };
ScenePoint view_delta(const App& app,double x,double y) {
    return app.spatial ? ScenePoint{0.86*x+0.24*y,0.82*y} : ScenePoint{x,y};
}
ScenePoint world_delta(const App& app,double x,double y) {
    if(!app.spatial)return {x,y};
    y/=0.82;
    return {(x-0.24*y)/0.86,y};
}
POINT scene_point(const App& app,RECT area,double x,double y) {
    const double cx=area.right/2.0,cy=area.bottom/2.0;
    const auto p=view_delta(app,x-cx,y-cy);
    return {static_cast<LONG>(std::lround(p.x+cx)),static_cast<LONG>(std::lround(p.y+cy))};
}
std::optional<LayoutBox> hit_scene(const App& app,HWND window,const LayoutView& view,int x,int y) {
    RECT area{};GetClientRect(window,&area);
    const double cx=area.right/2.0,cy=area.bottom/2.0;
    const auto p=world_delta(app,x-cx,y-cy);
    return hit_layout(view,static_cast<int>(std::floor(p.x+cx)),static_cast<int>(std::floor(p.y+cy)));
}
ScenePoint snap_drag(const App& app,double dx,double dy) {
    double snap_x=0,snap_y=0,near_x=14/app.drag_scale,near_y=14/app.drag_scale;
    for(const auto& a:app.editor.layout().screens()) {
        if(a.device_id!=app.drag_device || (!app.drag_screen.empty() && a.id!=app.drag_screen))continue;
        for(const auto& b:app.editor.layout().screens()) {
            if(b.device_id==app.drag_device && (app.drag_screen.empty() || a.id==b.id))continue;
            const double ax=a.x+dx,ay=a.y+dy,bx=b.x,by=b.y;
            if(std::max(ay,by)<std::min(ay+a.height,by+b.height)) {
                for(auto gap:{bx+b.width-ax,bx-(ax+a.width)})if(std::abs(gap)<=near_x){near_x=std::abs(gap);snap_x=gap;}
            }
            if(std::max(ax,bx)<std::min(ax+a.width,bx+b.width)) {
                for(auto gap:{by+b.height-ay,by-(ay+a.height)})if(std::abs(gap)<=near_y){near_y=std::abs(gap);snap_y=gap;}
            }
        }
    }
    return {dx+snap_x,dy+snap_y};
}
void polygon(HDC dc,const POINT* points,int count,COLORREF fill,COLORREF stroke,int width=1) {
    const auto brush=CreateSolidBrush(fill);const auto pen=CreatePen(PS_SOLID,width,stroke);
    const auto old_brush=SelectObject(dc,brush);const auto old_pen=SelectObject(dc,pen);
    Polygon(dc,points,count);
    SelectObject(dc,old_pen);SelectObject(dc,old_brush);DeleteObject(pen);DeleteObject(brush);
}
#ifdef HOP_WINDOWS_EXPERIMENTAL_TLS
void draw_panel_button(const App& app,const DRAWITEMSTRUCT& item) {
    const auto dc=item.hDC;
    RECT rect=item.rcItem;
    FillRect(dc,&rect,app.page_brush);
    const bool selected=(item.itemState&ODS_SELECTED)!=0, disabled=(item.itemState&ODS_DISABLED)!=0;
    const bool themed=app.page_color==RGB(246,244,251);
    const bool primary=!disabled && (item.CtlID==edge_id || item.CtlID==pair_connect_id ||
        (item.CtlID==add_id && !app.show_add && !app.state.connected()) ||
        (item.CtlID==enable_id && app.receiver_mode));
    const auto fill=themed ? (primary ? (selected?RGB(58,43,132):RGB(81,64,162)) :
        (selected?RGB(229,223,246):RGB(255,255,255))) : GetSysColor(COLOR_BTNFACE);
    const auto edge=themed ? (primary?fill:RGB(219,211,237)) : GetSysColor(COLOR_WINDOWTEXT);
    const auto brush=CreateSolidBrush(fill);const auto pen=CreatePen(PS_SOLID,1,edge);
    const auto old_brush=SelectObject(dc,brush);const auto old_pen=SelectObject(dc,pen);
    RoundRect(dc,rect.left,rect.top,rect.right,rect.bottom,12,12);
    SelectObject(dc,old_pen);SelectObject(dc,old_brush);DeleteObject(pen);DeleteObject(brush);
    wchar_t title[120]{};
    GetWindowTextW(item.hwndItem,title,120);
    const auto old_font=SelectObject(dc,app.body);
    SetBkMode(dc,TRANSPARENT);
    SetTextColor(dc,!themed ? GetSysColor(disabled?COLOR_GRAYTEXT:COLOR_BTNTEXT) :
        (disabled?RGB(108,104,128):primary?RGB(255,255,255):RGB(51,42,86)));
    InflateRect(&rect,-7,-2);
    DrawTextW(dc,title,-1,&rect,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
    if((item.itemState&ODS_FOCUS)!=0 && (item.itemState&ODS_NOFOCUSRECT)==0)DrawFocusRect(dc,&rect);
    SelectObject(dc,old_font);
}
#endif
#ifdef HOP_WINDOWS_EXPERIMENTAL_TLS
void apply_pointer_settings(App& app,const std::string& id) {
    if(!online_peer(app,id))return;const auto value=app.pointer_settings.current();const auto rate=app.pointer_settings.motion_rate();
    if(value && rate)app.runtime->update_pointer_settings(id,*value,*rate);
}
#endif
void pointer_menu(App& app,HWND window,const LayoutBox& box,POINT point) {
    app.editor.select(box.device_id);
    const auto device=std::find_if(app.editor.devices().begin(),app.editor.devices().end(),[&](const LayoutDevice& d){return d.id==box.device_id;});
    DeviceFingerprint fingerprint{};
    const bool allowed=device!=app.editor.devices().end() && device_allows_pointer_settings(*device) &&
        parse_device_fingerprint(device->id,fingerprint) && app.pointer_settings.select(fingerprint);
    HMENU popup=CreatePopupMenu(); if(!popup)return;
    const auto& text=strings(app);
    if(!allowed) {
        AppendMenuW(popup,MF_STRING|MF_GRAYED,0,text.settings_unavailable);
    } else {
        const auto value=app.pointer_settings.current();
        if(value) {
            constexpr double choices[]={0.25,0.5,0.75,1,1.25,1.5,2,3,4};
            const auto submenu=[&](const wchar_t* name,UINT base,double current) {
                HMENU group=CreatePopupMenu(); if(!group)return;
                for(UINT i=0;i<9;++i) {
                    wchar_t label[20]{}; swprintf(label,20,L"%.2g×",choices[i]);
                    AppendMenuW(group,MF_STRING|(std::abs(current-choices[i])<0.00001?MF_CHECKED:0),base+i,label);
                }
                AppendMenuW(popup,MF_POPUP,reinterpret_cast<UINT_PTR>(group),name);
            };
            submenu(text.sensitivity,310,value->movement_scale);
            const auto rate=app.pointer_settings.motion_rate();
            if(rate) {
                constexpr unsigned rates[]={60,120,240,500,1000};
                HMENU group=CreatePopupMenu();if(group) {
                    for(UINT i=0;i<5;++i) {wchar_t label[24]{};swprintf(label,24,L"%u Hz",rates[i]);AppendMenuW(group,MF_STRING|(*rate==rates[i]?MF_CHECKED:0),350+i,label);}
                    AppendMenuW(popup,MF_POPUP,reinterpret_cast<UINT_PTR>(group),text.motion_frequency);
                }
            }
            submenu(text.scroll_horizontal,320,value->horizontal_scroll_scale);
            submenu(text.scroll_vertical,330,value->vertical_scroll_scale);
            AppendMenuW(popup,MF_SEPARATOR,0,nullptr);
            AppendMenuW(popup,MF_STRING|(value->reverse_horizontal?MF_CHECKED:0),340,text.reverse_horizontal);
            AppendMenuW(popup,MF_STRING|(value->reverse_vertical?MF_CHECKED:0),341,text.reverse_vertical);
        }
    }
    ClientToScreen(window,&point); SetForegroundWindow(window);
    const auto command=TrackPopupMenu(popup,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,0,window,nullptr);
    DestroyMenu(popup);
    if(!allowed || !command)return;
    auto value=app.pointer_settings.current(); if(!value)return;
    constexpr double choices[]={0.25,0.5,0.75,1,1.25,1.5,2,3,4};
    if(command>=350 && command<355) {
        constexpr std::uint16_t rates[]={60,120,240,500,1000};
        if(!app.pointer_settings.update_motion_rate(fingerprint,rates[command-350]))MessageBoxW(window,text.settings_unavailable,text.layout,MB_OK|MB_ICONWARNING);
#ifdef HOP_WINDOWS_EXPERIMENTAL_TLS
        else apply_pointer_settings(app,box.device_id);
#endif
        return;
    }
    if(command>=310 && command<319)value->movement_scale=choices[command-310];
    else if(command>=320 && command<329)value->horizontal_scroll_scale=choices[command-320];
    else if(command>=330 && command<339)value->vertical_scroll_scale=choices[command-330];
    else if(command==340)value->reverse_horizontal=!value->reverse_horizontal;
    else if(command==341)value->reverse_vertical=!value->reverse_vertical;
    else return;
    if(!app.pointer_settings.update(fingerprint,*value)) MessageBoxW(window,text.settings_unavailable,text.layout,MB_OK|MB_ICONWARNING);
#ifdef HOP_WINDOWS_EXPERIMENTAL_TLS
    else apply_pointer_settings(app,box.device_id);
#endif
}
LRESULT CALLBACK layout_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(window,message,wparam,lparam);
    try {
    switch (message) {
    case WM_CREATE: {
        const auto button=CreateWindowW(L"BUTTON",app->language==UiLanguage::chinese?L"正视图":L"Front view",
            WS_CHILD|WS_VISIBLE|WS_TABSTOP,16,8,95,28,window,reinterpret_cast<HMENU>(angle_id),nullptr,nullptr);
        SendMessageW(button,WM_SETFONT,reinterpret_cast<WPARAM>(app->body),TRUE);
        return 0;
    }
    case WM_COMMAND:
        if(LOWORD(wparam)==angle_id && !app->dragging) {
            app->spatial=!app->spatial;
            SetWindowTextW(GetDlgItem(window,angle_id),app->spatial ? (app->language==UiLanguage::chinese?L"正视图":L"Front view") :
                (app->language==UiLanguage::chinese?L"空间视图":L"Space view"));
            InvalidateRect(window,nullptr,FALSE);
        }
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        const auto view=layout_view(*app,window);
        PAINTSTRUCT paint{}; const auto screen_dc=BeginPaint(window,&paint);
        RECT area{}; GetClientRect(window,&area);
        const auto buffer=CreateCompatibleDC(screen_dc);
        const auto bitmap=CreateCompatibleBitmap(screen_dc,area.right,area.bottom);
        const bool buffered=buffer && bitmap;
        const auto dc=buffered?buffer:screen_dc;
        const auto old_bitmap=buffered?SelectObject(dc,bitmap):nullptr;
        FillRect(dc,&area,app->page_brush);
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,app->ink_color);
        SelectObject(dc,app->body);
        if(view.boxes.empty()) {
            RECT title{20,70,area.right-20,105}, help{35,112,area.right-35,200};
            DrawTextW(dc,strings(*app).layout_empty,-1,&title,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            DrawTextW(dc,app->layout_storage_ok ? strings(*app).layout_help : strings(*app).layout_save_failed,-1,&help,DT_CENTER|DT_WORDBREAK);
        } else {
            RECT instructions{125,12,area.right-12,35};
            DrawTextW(dc,app->language==UiLanguage::chinese?L"拖动设备让边缘相贴 · 双击展开显示器":L"Drag edges together · Double-click to expand displays",
                -1,&instructions,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);
            const auto restore=SaveDC(dc);
            IntersectClipRect(dc,16,47,area.right-16,area.bottom-42);
            RECT board{16,47,area.right-16,area.bottom-42};
            FillRect(dc,&board,app->card_brush);
            for(int y=56;y<board.bottom;y+=18)for(int x=20;x<board.right;x+=18) {
                const auto gx=20+(x-20+(app->spatial?(y-56)/3:0)) % std::max(1,int(board.right)-20);
                SetPixelV(dc,gx,y,app->page_color==RGB(246,244,251)?RGB(204,197,227):app->ink_color);
            }
            const auto delta=world_delta(*app,app->drag_current.x-app->drag_start.x,app->drag_current.y-app->drag_start.y);
            const auto snapped=app->dragging && app->drag_scale>0 ? snap_drag(*app,delta.x/app->drag_scale,delta.y/app->drag_scale) : ScenePoint{};
            for(const auto& box:view.boxes) {
                RECT rect{box.left,box.top,box.right,box.bottom};
                if(app->dragging && box.device_id==app->drag_device && (app->drag_screen.empty() || box.screen_id==app->drag_screen))
                    OffsetRect(&rect,static_cast<int>(std::lround(snapped.x*app->drag_scale)),static_cast<int>(std::lround(snapped.y*app->drag_scale)));
                const POINT face[]{scene_point(*app,area,rect.left,rect.top),scene_point(*app,area,rect.right,rect.top),
                    scene_point(*app,area,rect.right,rect.bottom),scene_point(*app,area,rect.left,rect.bottom)};
                const bool local=box.device_id=="local", theme=app->page_color==RGB(246,244,251);
                const auto tint=theme?(local?RGB(230,223,250):RGB(218,239,235)):app->card_color;
                const auto edge=theme?(local?RGB(106,82,167):RGB(57,117,108)):app->ink_color;
                const auto back=theme?(local?RGB(177,159,226):RGB(143,192,182)):app->card_color;
                if(app->spatial) {
                    const POINT top[]{face[0],face[1],{face[1].x+7,face[1].y-6},{face[0].x+7,face[0].y-6}};
                    const POINT side[]{face[1],face[2],{face[2].x+7,face[2].y-6},{face[1].x+7,face[1].y-6}};
                    polygon(dc,top,4,back,edge);polygon(dc,side,4,back,edge);
                    const auto mid=(face[2].x+face[3].x)/2;
                    RECT post{mid-2,face[2].y,mid+2,face[2].y+12},foot{mid-12,face[2].y+11,mid+12,face[2].y+15};
                    const auto stand=CreateSolidBrush(edge);FillRect(dc,&post,stand);FillRect(dc,&foot,stand);DeleteObject(stand);
                }
                polygon(dc,face,4,tint,edge,box.selected?3:1);
                const int mid_x=(face[0].x+face[2].x)/2,mid_y=(face[0].y+face[2].y)/2,w=std::max(0,int(face[1].x-face[0].x)-16);
                RECT label{mid_x-w/2,mid_y-13,mid_x+w/2,mid_y+13};
                if(w>=23)DrawTextW(dc,box.name.c_str(),-1,&label,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
            }
            RestoreDC(dc,restore);
            RECT help{18,area.bottom-31,area.right-18,area.bottom-8};
            DrawTextW(dc,app->language==UiLanguage::chinese?L"紫色是本机。鼠标从相贴的边缘穿越。":L"Purple is this PC. The pointer crosses where screen edges touch.",
                -1,&help,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);
        }
        if(buffered){BitBlt(screen_dc,0,0,area.right,area.bottom,dc,0,0,SRCCOPY);SelectObject(dc,old_bitmap);}
        if(bitmap)DeleteObject(bitmap);
        if(buffer)DeleteDC(buffer);
        EndPaint(window,&paint); return 0;
    }
    case WM_LBUTTONDOWN: {
        if(!layout_editable(*app))return 0;
        const auto view=layout_view(*app,window); const auto box=hit_scene(*app,window,view,GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam));
        if(!box || box->device_id=="local" || view.scale<=0)return 0;
        app->editor.select(box->device_id); app->dragging=true; app->drag_start={GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)}; app->drag_current=app->drag_start;
        app->drag_scale=view.scale; app->drag_device=box->device_id; app->drag_screen=box->screen_id;
        for(const auto& device:app->editor.devices()) for(const auto& screen:device.screens) if(screen.id==box->screen_id){app->screen_x=screen.x;app->screen_y=screen.y;}
        SetCapture(window); InvalidateRect(window,nullptr,TRUE); return 0;
    }
    case WM_MOUSEMOVE:
        if(app->dragging && (wparam&MK_LBUTTON)){app->drag_current={GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};InvalidateRect(window,nullptr,TRUE);}
        return 0;
    case WM_LBUTTONDBLCLK: {
        if(!layout_editable(*app))return 0;
        const auto view=layout_view(*app,window); const auto box=hit_scene(*app,window,view,GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam));
        if(app->dragging){app->dragging=false;ReleaseCapture();}
        if(box){
            auto previous=app->editor;
            app->editor.select(box->device_id);app->editor.expand(box->device_id,!box->expanded);
            if(!persist_layout(*app)) {
                app->editor=std::move(previous);MessageBoxW(window,strings(*app).layout_save_failed,strings(*app).layout,MB_OK|MB_ICONWARNING);
            }
            InvalidateRect(window,nullptr,TRUE);
        } return 0;
    }
    case WM_LBUTTONUP: {
        if(!app->dragging)return 0;
        app->dragging=false;ReleaseCapture();
        if(!layout_editable(*app)){InvalidateRect(window,nullptr,TRUE);return 0;}
        const auto delta=world_delta(*app,GET_X_LPARAM(lparam)-app->drag_start.x,GET_Y_LPARAM(lparam)-app->drag_start.y);
        const auto snapped=snap_drag(*app,delta.x/app->drag_scale,delta.y/app->drag_scale);
        const auto dx=snapped.x,dy=snapped.y;
        auto previous=app->editor;
        bool ok=std::isfinite(dx)&&std::isfinite(dy)&&std::abs(dx)<10000000&&std::abs(dy)<10000000;
        if(ok) {
            const auto x=static_cast<std::int32_t>(std::llround(dx)), y=static_cast<std::int32_t>(std::llround(dy));
            if(app->drag_screen.empty())ok=app->editor.move_device(app->drag_device,x,y);
            else {
                const auto sx=std::int64_t(app->screen_x)+x, sy=std::int64_t(app->screen_y)+y;
                ok=sx>=std::numeric_limits<std::int32_t>::min()&&sx<=std::numeric_limits<std::int32_t>::max()&&sy>=std::numeric_limits<std::int32_t>::min()&&sy<=std::numeric_limits<std::int32_t>::max();
                if(ok)ok=app->editor.move_screen(app->drag_screen,static_cast<std::int32_t>(sx),static_cast<std::int32_t>(sy));
            }
        }
        if(ok && !persist_layout(*app)) {
            app->editor=std::move(previous);
            MessageBoxW(window,strings(*app).layout_save_failed,strings(*app).layout,MB_OK|MB_ICONWARNING);
        } else if(!ok) MessageBoxW(window,strings(*app).layout_invalid,strings(*app).layout,MB_OK|MB_ICONWARNING);
        InvalidateRect(window,nullptr,TRUE);return 0;
    }
    case WM_RBUTTONUP: {
        const auto view=layout_view(*app,window); const auto box=hit_scene(*app,window,view,GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam));
        if(box){pointer_menu(*app,window,*box,{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)});InvalidateRect(window,nullptr,TRUE);} return 0;
    }
    case WM_CAPTURECHANGED: app->dragging=false; return 0;
    case WM_CLOSE: DestroyWindow(window); return 0;
    case WM_DESTROY: app->layout_window=nullptr; return 0;
    }
    } catch (...) {
        // Never unwind a C++ exception across a Win32 window procedure.
        app->dragging=false;
        if(GetCapture()==window) ReleaseCapture();
        if(message==WM_PAINT) ValidateRect(window,nullptr);
        return 0;
    }
    return DefWindowProcW(window,message,wparam,lparam);
}
void show_layout(App& app) {
    if (!app.layout_window) {
        app.layout_window=CreateWindowExW(WS_EX_TOOLWINDOW,layout_class,strings(app).layout,
            WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,CW_USEDEFAULT,CW_USEDEFAULT,620,374,
            app.window,nullptr,GetModuleHandleW(nullptr),&app);
    }
    if (app.layout_window) { ShowWindow(app.layout_window,SW_SHOWNORMAL); SetForegroundWindow(app.layout_window); }
}
void menu(App& app) {
    const auto& s = strings(app);
    HMENU popup = CreatePopupMenu();
    if (!popup) return;
    AppendMenuW(popup, MF_STRING | MF_GRAYED, 0, app.state.sharing() ? s.sharing : app.state.connected() ? s.connected : s.disconnected);
    AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(popup, MF_STRING, show_id, s.show);
    AppendMenuW(popup, MF_STRING, layout_id, s.layout);
    bool can_enable=app.state.can_enable();
#ifdef HOP_WINDOWS_EXPERIMENTAL_TLS
    can_enable=can_enable && !app.edge_armed && !app.control_pending;
#endif
    AppendMenuW(popup, MF_STRING | (can_enable ? MF_ENABLED : MF_GRAYED), enable_id, s.enable);
    bool can_pause=app.state.can_pause();
#ifdef HOP_WINDOWS_EXPERIMENTAL_TLS
    can_pause=can_pause || app.auto_edge_desired;
#endif
    AppendMenuW(popup, MF_STRING | (can_pause ? MF_ENABLED : MF_GRAYED), pause_id, s.pause);
    AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(popup, MF_STRING, quit_id, s.quit);
    POINT cursor{}; GetCursorPos(&cursor);
    SetForegroundWindow(app.window);
    TrackPopupMenu(popup, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, app.window, nullptr);
    PostMessageW(app.window, WM_NULL, 0, 0);
    DestroyMenu(popup);
}
#ifdef HOP_WINDOWS_EXPERIMENTAL_TLS
const wchar_t* localized(const App& app,const wchar_t* zh,const wchar_t* en) {return app.language==UiLanguage::chinese?zh:en;}
void set_pair_status(App& app,const wchar_t* zh,const wchar_t* en) {SetWindowTextW(app.explanation,localized(app,zh,en));}
std::wstring peer_label(const App& app,const ConnectedPeer& peer) {
    std::wstring name;
    const auto bytes=static_cast<int>(peer.name.size());
    const auto length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,peer.name.data(),bytes,nullptr,0);
    if(length>0) {
        name.resize(length);
        if(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,peer.name.data(),bytes,name.data(),length)!=length)name.clear();
    }
    if(name.empty())name=localized(app,L"已配对设备",L"Paired device");
    if(name.size()>45) {
        name.resize(45);
        if(name.back()>=0xd800 && name.back()<=0xdbff)name.pop_back();
        name+=L"…";
    }
    std::wstring label;
    for(const auto c:name){label+=c;if(c==L'&')label+=c;}
    label+=L" · ";
    const auto short_pin=peer.fingerprint.substr(0,12);
    label.append(short_pin.begin(),short_pin.end());
    return label;
}
void cancel_layout_drag(App& app) {
    app.dragging = false;
    if (app.layout_window && GetCapture() == app.layout_window) ReleaseCapture();
}
void clear_live_layout(App& app) {
    if (app.offline_editor) {
        cancel_layout_drag(app);
        app.editor = std::move(*app.offline_editor);
        app.offline_editor.reset();
        if (app.layout_window) InvalidateRect(app.layout_window, nullptr, TRUE);
    }
    app.local_geometry.clear();
    app.peer_geometry.clear();
    app.native_geometry.clear();
    app.live_layout_key.clear();
}
std::string live_screen_id(const std::string& peer,std::uint64_t screen,bool multi_peer) {
    if(peer.empty())return "l:"+std::to_string(screen);
    return (multi_peer ? "r:"+peer+":" : "r:")+std::to_string(screen);
}
bool refresh_live_layout(App& app) {
    if(current_live_geometry(app))return true;
    clear_live_layout(app);
    if(!app.runtime || app.runtime->state()!=PairingState::connected || app.receiver_mode)return false;
    auto peers=app.runtime->connected_peers();
    LocalDisplaySnapshot local;
    if(peers.empty() || !enumerate_local_displays(local) || local.displays.size()>16)return false;
    std::sort(peers.begin(),peers.end(),[](const auto& a,const auto& b){return a.fingerprint<b.fingerprint;});
    const bool multi_peer=peers.size()>1;
    std::vector<LayoutDevice> devices;
    LayoutDevice home{"local",localized(app,L"本机",L"This computer"),true,false,{},true};
    std::vector<LiveScreen> local_geometry, transport_geometry;
    std::vector<LivePeerScreens> peer_geometry;
    std::vector<hop::Screen> native_geometry;
    std::string profile;
    std::int64_t right=std::numeric_limits<std::int64_t>::min(),top=std::numeric_limits<std::int64_t>::max();
    for(const auto& display:local.displays) {
        const auto& bounds=display.bounds;
        local_geometry.push_back({display.id,bounds.x,bounds.y,bounds.width,bounds.height});
        home.screens.push_back({live_screen_id({},display.id,multi_peer),home.id,bounds.x,bounds.y,
            static_cast<std::int32_t>(bounds.width),static_cast<std::int32_t>(bounds.height)});
        native_geometry.push_back(home.screens.back());
        right=std::max(right,std::int64_t(bounds.x)+bounds.width);
        top=std::min(top,std::int64_t(bounds.y));
    }
    devices.push_back(std::move(home));
    for(const auto& peer:peers) {
        DeviceFingerprint fingerprint{};
        if(!parse_device_fingerprint(peer.fingerprint,fingerprint) || peer.displays.empty() || peer.displays.size()>16)return false;
        if(!profile.empty())profile+='-';
        profile+=peer.fingerprint;
        LayoutDevice remote{peer.fingerprint,peer_label(app,peer),true,false,{},true};
        LivePeerScreens geometry{peer.fingerprint,{}};
        std::int64_t left=std::numeric_limits<std::int64_t>::max(),peer_top=left;
        for(const auto& display:peer.displays) {
            left=std::min(left,std::int64_t(display.x));
            peer_top=std::min(peer_top,std::int64_t(display.y));
        }
        auto next_right=right;
        for(const auto& display:peer.displays) {
            const auto x=right+std::int64_t(display.x)-left,y=top+std::int64_t(display.y)-peer_top;
            if(x < -10000000 || x > 10000000 || y < -10000000 || y > 10000000)return false;
            geometry.screens.push_back({display.id,display.x,display.y,display.width,display.height});
            hop::Screen native{live_screen_id(peer.fingerprint,display.id,multi_peer),remote.id,display.x,display.y,
                static_cast<std::int32_t>(display.width),static_cast<std::int32_t>(display.height)};
            native_geometry.push_back(native);
            native.x=static_cast<std::int32_t>(x);native.y=static_cast<std::int32_t>(y);
            remote.screens.push_back(std::move(native));
            next_right=std::max(next_right,x+display.width);
        }
        right=next_right;
        peer_geometry.push_back(std::move(geometry));
        devices.push_back(std::move(remote));
    }
    LayoutEditor editor;
    if(!editor.replace(std::move(devices)))return false;
    auto storage=live_storage(profile);
    load_live_layout(storage,native_geometry,editor);
    cancel_layout_drag(app);
    app.live_layout_key=std::move(profile);
    app.local_geometry=std::move(local_geometry);
    app.peer_geometry=std::move(peer_geometry);
    app.native_geometry=std::move(native_geometry);
    app.offline_editor.emplace(std::move(app.editor));
    app.editor=std::move(editor);
    if(!current_live_geometry(app)){clear_live_layout(app);return false;}
    if(app.layout_window)InvalidateRect(app.layout_window,nullptr,TRUE);
    return true;
}
bool arm_live_edge(App& app) {
    if(app.receiver_mode || !current_live_geometry(app))return false;
    std::vector<LiveScreenPlacement> placements;
    for(const auto& device:app.editor.devices()) {
        const bool local=device.id=="local";
        const auto peer=local ? std::string{} : device.id;
        const auto group=std::find_if(app.peer_geometry.begin(),app.peer_geometry.end(),[&](const auto& candidate){return candidate.fingerprint==peer;});
        if(!local && group==app.peer_geometry.end())return false;
        const auto& geometry=local ? app.local_geometry : group->screens;
        for(const auto& screen:device.screens) {
            const auto found=std::find_if(geometry.begin(),geometry.end(),[&](const auto& native){
                return screen.id==live_screen_id(peer,native.id,app.peer_geometry.size()>1) &&
                    screen.width==static_cast<std::int32_t>(native.width) && screen.height==static_cast<std::int32_t>(native.height);
            });
            if(found==geometry.end())return false;
            placements.push_back({peer,found->id,screen.x,screen.y});
        }
    }
    return app.runtime->request_workspace_edges(app.screen_peer,app.local_geometry,app.peer_geometry,std::move(placements));
}
void refresh_screens(App& app) {
    auto snapshot=app.runtime?app.runtime->selected_displays():PeerDisplaySnapshot{};
    app.screen_peer=std::move(snapshot.peer);
    app.peer_displays=std::move(snapshot.displays);
    SendMessageW(app.screen_combo,CB_RESETCONTENT,0,0);
    for(std::size_t i=0;i<app.peer_displays.size();++i) {
        const auto& d=app.peer_displays[i];wchar_t label[80]{};
        swprintf(label,80,localized(app,L"显示器 %zu · %u×%u",L"Display %zu · %u×%u"),i+1,d.width,d.height);
        SendMessageW(app.screen_combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label));
    }
    if(!app.peer_displays.empty())SendMessageW(app.screen_combo,CB_SETCURSEL,0,0);
    if(!app.receiver_mode)refresh_live_layout(app);
    update_controls(app);
}
bool start_pairing(App& app,bool listen) {
    wchar_t port_text[16]{},address_text[128]{};GetWindowTextW(app.port,port_text,16);GetWindowTextW(app.address,address_text,128);
    wchar_t* end=nullptr;const auto port=std::wcstoul(port_text,&end,10);if(!end || *end || !port || port>65535)return false;
    std::string address;
    if(listen)address="0.0.0.0";
    else {for(const auto c:address_text){if(!c)break;if(c>127)return false;address.push_back(static_cast<char>(c));}if(address.empty())return false;}
    TcpAddress endpoint{};if(!numeric_tcp_address(address,static_cast<std::uint16_t>(port),endpoint))return false;
    if(app.runtime) {
        if(!can_change_peer(app) || !app.runtime->request_add_peer(listen,endpoint))return false;
        app.peer_change_pending=true;
        update_controls(app);
        set_pair_status(app,L"正在添加设备…",L"Adding a device…");
        return true;
    }
    LocalDisplaySnapshot snapshot;if(!enumerate_local_displays(snapshot))return false;
    app.transport_geometry.clear();
    for(const auto& display:snapshot.displays) {
        const auto& r=display.bounds;
        app.transport_geometry.push_back({display.id,r.x,r.y,r.width,r.height});
    }
    const bool receiver=SendMessageW(app.receiver_role,BM_GETCHECK,0,0)==BST_CHECKED;
    app.receiver_mode=receiver;app.allow_receiver.store(false);app.peer_displays.clear();
    PairingRuntimeConfig config;config.listen=listen;config.address=endpoint;config.role=receiver?SessionRole::receiver:SessionRole::host;config.name="Windows";
    if(SendMessageW(app.clipboard_checkbox,BM_GETCHECK,0,0)==BST_CHECKED) {
        config.clipboard_backend=std::make_shared<WindowsClipboardBackend>(app.window);
        config.clipboard_sequence=[] {return static_cast<std::uint32_t>(GetClipboardSequenceNumber());};
    }
    if(receiver) {
        app.receiver_sink=std::make_unique<InputSink>(InputSink::system());
        app.receiver_adapter=std::make_unique<ReceiverSessionAdapter>(*app.receiver_sink,std::move(snapshot.displays),snapshot.desktop);
        if(!app.receiver_adapter->valid())return false;
        config.screens=app.receiver_adapter->screen_ids();config.screen_payload=app.receiver_adapter->screen_payload();
        auto callbacks=app.receiver_adapter->callbacks();auto* owner=&app;
        config.callbacks=[callbacks,owner] {return SessionCallbacks{
            [callbacks,owner](const hop::Frame& f){return owner->allow_receiver.load() && callbacks.enter && callbacks.enter(f);},
            [callbacks,owner](const hop::Frame& f){return owner->allow_receiver.load() && callbacks.input && callbacks.input(f);},
            [callbacks]{return callbacks.restore && callbacks.restore();}};};
    } else {
        config.host_capture=&app.host_capture;
        config.verify_local_geometry=same_local_geometry;
        config.pointer_profile=[](const std::string& peer)->std::optional<PointerProfile> {
            DeviceFingerprint fingerprint{};
            if(!parse_device_fingerprint(peer,fingerprint))return std::nullopt;
            RegistryPointerSettingsStore store;
            TargetPointerSettings target(store);
            if(!target.select(fingerprint))return std::nullopt;
            const auto settings=target.current();
            const auto rate=target.motion_rate();
            if(!settings || !rate)return std::nullopt;
            return PointerProfile{*settings,*rate};
        };
        config.callbacks=[] {return SessionCallbacks{{},{},[]{return true;}};};
    }
    app.event_window.store(app.window);auto* owner=&app;
    if(++app.runtime_generation==0)++app.runtime_generation;const auto generation=app.runtime_generation;
    app.runtime=std::make_unique<PairingRuntime>(app.credentials,[owner,generation](PairingRuntimeEvent event){const auto window=owner->event_window.load();if(window)PostMessageW(window,pairing_message,static_cast<WPARAM>(event),static_cast<LPARAM>(generation));});
    if(!app.runtime->start(std::move(config))){app.runtime.reset();app.receiver_adapter.reset();app.receiver_sink.reset();return false;}
    EnableWindow(app.listen,FALSE);EnableWindow(app.connect,FALSE);EnableWindow(app.host_role,FALSE);EnableWindow(app.receiver_role,FALSE);EnableWindow(app.clipboard_checkbox,FALSE);
    app.show_add=false;app.show_options=false;update_controls(app);
    set_pair_status(app,L"正在准备配对…",L"Preparing pairing…");SetTimer(app.window,reap_timer,200,nullptr);return true;
}
void on_pair_event(App& app,PairingRuntimeEvent event) {
    if(!app.runtime)return;
    switch(event) {
    case PairingRuntimeEvent::listener_ready: {
        TcpAddress address{};if(app.runtime->listen_address(address) && address.value.ss_family==AF_INET)app.discovery.advertise(ntohs(reinterpret_cast<const sockaddr_in*>(&address.value)->sin_port));
        set_pair_status(app,L"请在另一台电脑上打开 Hop，选择这台电脑。",L"Open Hop on the other computer and select this computer.");break;
    }
    case PairingRuntimeEvent::reviewing: {
        auto review=app.runtime->review();if(!review || !show_candidate_fingerprint_prompt(app.window,app.language,*review))set_pair_status(app,L"配对已取消或已过期。",L"Pairing was cancelled or expired.");break;
    }
    case PairingRuntimeEvent::confirming: {
        auto review=app.runtime->review();const bool accepted=review && show_tls_pair_confirmation(app.window,app.language,*review);app.runtime->confirm_tls(accepted);break;
    }
    case PairingRuntimeEvent::connected: {
        app.discovery.stop_advertising();app.state.connected(true);app.control_pending=false;app.peer_change_pending=false;
        const auto peer=app.runtime->peer_fingerprint();
        DeviceFingerprint id{};if(parse_device_fingerprint(peer,id) && app.pointer_settings.select(id))apply_pointer_settings(app,peer);
        app.show_add=false;app.show_options=false;
        refresh_screens(app);
        if(app.receiver_mode)set_pair_status(app,L"配对完成。允许键鼠控制后，就可以从主机穿越到这台电脑。",L"Paired. Allow control to cross from the sharing computer.");
        else set_pair_status(app,L"配对完成。排列屏幕，然后开始共享。",L"Paired. Arrange the screens, then start sharing.");
        break;
    }
    case PairingRuntimeEvent::screens_changed:refresh_screens(app);break;
    case PairingRuntimeEvent::sharing_started:app.control_pending=false;app.edge_armed=false;app.state.enable();update_controls(app);break;
    case PairingRuntimeEvent::sharing_stopped:app.control_pending=false;app.state.pause();update_controls(app);break;
    case PairingRuntimeEvent::edge_monitor_started:if(app.auto_edge_desired){app.edge_armed=true;update_controls(app);set_pair_status(app,L"把鼠标移过相贴的屏幕边缘，即可穿越。",L"Move the pointer across a touching screen edge.");}break;
    case PairingRuntimeEvent::action_failed:if(app.auto_edge_desired && app.runtime && !app.runtime->request_pause())app.runtime->request_stop();app.control_pending=false;app.edge_armed=false;app.auto_edge_desired=false;update_controls(app);set_pair_status(app,L"无法启用键鼠，请检查权限和目标状态。",L"Input could not be enabled. Check permissions and target status.");break;
    case PairingRuntimeEvent::failed:app.allow_receiver.store(false);app.control_pending=false;app.state.connected(false);clear_live_layout(app);update_controls(app);set_pair_status(app,L"连接已中断。可以通过「添加电脑」重新连接。",L"Connection interrupted. Use Add computer to reconnect.");break;
    case PairingRuntimeEvent::stopped:app.allow_receiver.store(false);app.control_pending=false;app.state.connected(false);clear_live_layout(app);update_controls(app);break;
    case PairingRuntimeEvent::peer_action_failed:
        app.discovery.stop_advertising();app.peer_change_pending=false;update_controls(app);
        set_pair_status(app,L"无法添加或切换设备，请先恢复本机控制并重试。",L"Could not add or switch device. Return control to this computer and try again.");
        break;
    case PairingRuntimeEvent::peers_changed:if(!app.receiver_mode)refresh_screens(app);else update_controls(app);break;
    case PairingRuntimeEvent::target_changed:
        app.edge_armed=app.auto_edge_desired;
        refresh_screens(app);
        set_pair_status(app,L"正在穿越到下一台设备…",L"Crossing to the next device…");
        break;
    }
}
#endif
LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        app = static_cast<App*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        app->window = window;
    }
    if (!app) return DefWindowProcW(window, message, wparam, lparam);
    if (app->taskbar_created && message == app->taskbar_created) { if (!add_tray(*app)) show_status(*app); return 0; }
    switch (message) {
    case WM_CREATE: {
        app->window=window;
        const auto make=[&](const wchar_t* cls,const wchar_t* title,DWORD style,int x,int y,int w,int h,UINT id=0,DWORD ex=0) {
            return CreateWindowExW(ex,cls,title,WS_CHILD|style,x,y,w,h,window,reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),nullptr,nullptr);
        };
        HIGHCONTRASTW hc{sizeof(HIGHCONTRASTW)};
        if(SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(hc),&hc,0) && (hc.dwFlags&HCF_HIGHCONTRASTON)) {
            app->page_color=GetSysColor(COLOR_WINDOW);app->card_color=app->page_color;app->ink_color=GetSysColor(COLOR_WINDOWTEXT);
        }
        app->page_brush=CreateSolidBrush(app->page_color); app->card_brush=CreateSolidBrush(app->card_color);
        app->headline=CreateFontW(-23,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        app->body=CreateFontW(-15,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
#ifdef HOP_WINDOWS_EXPERIMENTAL_TLS
        constexpr DWORD button=WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW;
#else
        constexpr DWORD button=WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON;
#endif
        app->status=make(L"STATIC",L"",WS_VISIBLE,34,65,353,27);
        app->explanation=make(L"STATIC",L"",WS_VISIBLE,34,98,353,42);
        app->enable=make(L"BUTTON",L"",button,21,156,119,32,enable_id);
        app->pause=make(L"BUTTON",L"",button,21,156,119,32,pause_id);
        app->layout=make(L"BUTTON",L"",button,151,156,119,32,layout_id);
        if(!app->status || !app->explanation || !app->enable || !app->pause || !app->layout)return -1;
#ifdef HOP_WINDOWS_EXPERIMENTAL_TLS
        app->brand=make(L"STATIC",L"Hop",WS_VISIBLE,52,14,140,31);
        app->options=make(L"BUTTON",localized(*app,L"设置…",L"Settings…"),button,280,16,119,29,options_id);
        app->add=make(L"BUTTON",L"",button,280,156,119,32,add_id);
        app->disconnect=make(L"BUTTON",L"",button,280,156,119,32,disconnect_id);
        app->edge=make(L"BUTTON",L"",button,21,156,119,32,edge_id);
        app->address_label=make(L"STATIC",localized(*app,L"局域网 IP",L"LAN address"),0,21,267,260,20);
        app->port_label=make(L"STATIC",localized(*app,L"端口",L"Port"),0,303,267,96,20);
        app->address=make(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,21,291,267,25,0,WS_EX_CLIENTEDGE);
        app->port=make(L"EDIT",L"24891",WS_TABSTOP|ES_NUMBER,303,291,96,25,0,WS_EX_CLIENTEDGE);
        app->host_role=make(L"BUTTON",localized(*app,L"共享键鼠",L"Share input"),WS_TABSTOP|WS_GROUP|BS_AUTORADIOBUTTON,21,203,116,24);
        app->receiver_role=make(L"BUTTON",localized(*app,L"接收键鼠",L"Receive input"),WS_TABSTOP|BS_AUTORADIOBUTTON,151,203,117,24);
        SendMessageW(app->receiver_role,BM_SETCHECK,BST_CHECKED,0);
        app->listen=make(L"BUTTON",localized(*app,L"等待配对",L"Wait for pairing"),WS_TABSTOP|WS_GROUP|BS_OWNERDRAW,21,231,119,28,pair_listen_id);
        app->discover=make(L"BUTTON",localized(*app,L"查找附近设备",L"Find nearby"),WS_TABSTOP|BS_OWNERDRAW,151,231,119,28,discovery_id);
        app->manual=make(L"BUTTON",localized(*app,L"输入地址…",L"Enter address…"),WS_TABSTOP|BS_OWNERDRAW,280,231,119,28,manual_id);
        app->connect=make(L"BUTTON",localized(*app,L"连接",L"Connect"),WS_TABSTOP|BS_OWNERDRAW,280,320,119,28,pair_connect_id);
        app->peer_settings=make(L"BUTTON",localized(*app,L"鼠标设置…",L"Mouse settings…"),WS_TABSTOP|BS_OWNERDRAW,0,0,1,1,peer_settings_id);
        app->peer_list=make(L"BUTTON",localized(*app,L"切换设备…",L"Switch device…"),WS_TABSTOP|BS_OWNERDRAW,0,0,1,1,peer_list_id);
        app->screen_combo=make(L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST,0,0,1,1,screen_combo_id);
        app->clipboard_checkbox=make(L"BUTTON",localized(*app,L"同步文本和图片剪贴板",L"Sync text and image clipboard"),WS_TABSTOP|BS_AUTOCHECKBOX,0,0,1,1);
        for(auto control:{app->brand,app->options,app->add,app->disconnect,app->edge,app->address_label,app->port_label,app->address,app->port,
                          app->host_role,app->receiver_role,app->listen,app->discover,app->manual,app->connect,app->peer_settings,app->peer_list,
                          app->screen_combo,app->clipboard_checkbox}) {
            if(!control)return -1;
            SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(app->body),TRUE);
        }
        SendMessageW(app->brand,WM_SETFONT,reinterpret_cast<WPARAM>(app->headline),TRUE);
        SetWindowTextW(app->explanation,localized(*app,L"添加电脑，开始使用一套键鼠。",L"Add a computer to share one keyboard and mouse."));
#endif
        for(auto control:{app->status,app->explanation,app->enable,app->pause,app->layout})
            SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(app->body),TRUE);
        SendMessageW(app->status,WM_SETFONT,reinterpret_cast<WPARAM>(app->headline),TRUE);
        update_controls(*app);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        auto dc=reinterpret_cast<HDC>(wparam);
        const auto control=reinterpret_cast<HWND>(lparam);
        const bool card=control==app->status || control==app->explanation;
        SetTextColor(dc,app->ink_color);SetBkColor(dc,card?app->card_color:app->page_color);
        return reinterpret_cast<LRESULT>(card?app->card_brush:app->page_brush);
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};auto dc=BeginPaint(window,&paint);RECT bounds{};GetClientRect(window,&bounds);
        FillRect(dc,&bounds,app->page_brush);
        auto old_brush=SelectObject(dc,app->card_brush);auto old_pen=SelectObject(dc,GetStockObject(NULL_PEN));
        RoundRect(dc,20,51,bounds.right-20,145,22,22);
#ifdef HOP_WINDOWS_EXPERIMENTAL_TLS
        const auto brand_pen=CreatePen(PS_SOLID,2,app->page_color==RGB(246,244,251)?RGB(81,64,162):app->ink_color);
        SelectObject(dc,brand_pen);
        RoundRect(dc,22,18,41,33,5,5);RoundRect(dc,27,23,46,38,5,5);
#endif
        SelectObject(dc,old_pen);SelectObject(dc,old_brush);
#ifdef HOP_WINDOWS_EXPERIMENTAL_TLS
        DeleteObject(brand_pen);
#endif
        EndPaint(window,&paint);return 0;
    }
#ifdef HOP_WINDOWS_EXPERIMENTAL_TLS
    case WM_DRAWITEM:
        if(lparam && reinterpret_cast<const DRAWITEMSTRUCT*>(lparam)->CtlType==ODT_BUTTON) {
            draw_panel_button(*app,*reinterpret_cast<const DRAWITEMSTRUCT*>(lparam));
            return TRUE;
        }
        break;
    case pairing_message:if(static_cast<std::uint32_t>(lparam)==app->runtime_generation)on_pair_event(*app,static_cast<PairingRuntimeEvent>(wparam));return 0;
    case WM_TIMER:
        if(wparam==panel_timer) {
            RECT bounds{};GetClientRect(window,&bounds);
            const int delta=app->panel_height-bounds.bottom;
            if(std::abs(delta)<=5) {KillTimer(window,panel_timer);app->animating=false;adjust_panel(*app,app->panel_height);}
            else adjust_panel(*app,bounds.bottom+(delta/3));
            return 0;
        }
        if(wparam==reap_timer && app->runtime && app->runtime->finished()) {
            app->discovery.stop_advertising();app->runtime->stop();app->runtime.reset();app->receiver_adapter.reset();app->receiver_sink.reset();app->peer_displays.clear();
            clear_live_layout(*app);
            app->allow_receiver.store(false);app->state.connected(false);app->control_pending=false;app->edge_armed=false;app->auto_edge_desired=false;app->peer_change_pending=false;
            EnableWindow(app->listen,TRUE);EnableWindow(app->connect,TRUE);EnableWindow(app->host_role,TRUE);EnableWindow(app->receiver_role,TRUE);EnableWindow(app->clipboard_checkbox,TRUE);
            KillTimer(window,reap_timer);refresh_screens(*app);update_controls(*app);
        }
        return 0;
#endif
    case tray_message:
        if (lparam == WM_LBUTTONUP || lparam == WM_LBUTTONDBLCLK) show_status(*app);
        else if (lparam == WM_RBUTTONUP || lparam == WM_CONTEXTMENU) menu(*app);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case show_id: show_status(*app); break;
        case enable_id:
#ifdef HOP_WINDOWS_EXPERIMENTAL_TLS
            if(app->runtime && app->state.connected()) {
                if(app->receiver_mode){app->allow_receiver.store(true);app->state.enable();update_controls(*app);}
                else if(!app->edge_armed && !app->control_pending && !app->peer_change_pending){const auto index=SendMessageW(app->screen_combo,CB_GETCURSEL,0,0);if(index!=CB_ERR && static_cast<std::size_t>(index)<app->peer_displays.size() && app->runtime->request_control(app->screen_peer,app->peer_displays[static_cast<std::size_t>(index)].id)){app->control_pending=true;update_controls(*app);}}
            }
#else
            if (app->state.enable()) update_controls(*app);
#endif
            break;
        case pause_id:
#ifdef HOP_WINDOWS_EXPERIMENTAL_TLS
            if(app->runtime && (app->state.sharing() || app->auto_edge_desired)) {
                if(app->receiver_mode){app->allow_receiver.store(false);app->runtime->request_stop();set_pair_status(*app,L"正在停止并释放输入…",L"Stopping and releasing input…");}
                else if(app->runtime->request_pause()){app->edge_armed=false;app->auto_edge_desired=false;update_controls(*app);}
            }
#else
            if (app->state.pause()) update_controls(*app);
#endif
            break;
#ifdef HOP_WINDOWS_EXPERIMENTAL_TLS
        case add_id: case options_id: case manual_id: {
            if(LOWORD(wparam)==add_id) {
                app->show_add=!app->show_add; app->show_options=false;
                if(app->show_add)app->discovery.start();
            } else if(LOWORD(wparam)==options_id) {
                app->show_options=!app->show_options; app->show_add=false;
            } else app->show_manual=!app->show_manual;
            BOOL motion=TRUE;SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&motion,0);
            app->animating=motion && SetTimer(window,panel_timer,16,nullptr)!=0;
            update_controls(*app);break;
        }
        case disconnect_id:
            if(app->runtime){app->allow_receiver.store(false);app->runtime->request_stop();set_pair_status(*app,L"正在断开连接…",L"Disconnecting…");}
            break;
        case edge_id:
            if(!arm_live_edge(*app))MessageBoxW(window,localized(*app,L"无法启用边缘穿越，请确认显示器未变化并检查布局。",L"Could not arm edge crossing. Check that displays and layout are unchanged."),L"Hop",MB_OK|MB_ICONWARNING);
            else {app->edge_armed=true;app->auto_edge_desired=true;update_controls(*app);set_pair_status(*app,L"正在启用边缘监听…",L"Arming edge monitoring…");}
            break;
        case discovery_id: {
            if(!app->discovery.start()){MessageBoxW(window,localized(*app,L"无法启动局域网发现，请手动输入地址。",L"Discovery could not start. Enter the address manually."),L"Hop",MB_OK|MB_ICONWARNING);break;}
            const auto candidates=app->discovery.candidates();
            if(candidates.empty()){MessageBoxW(window,localized(*app,L"正在查找，请稍后再次点击。发现结果需要继续配对验证。",L"Searching. Try again shortly. Discovered devices still require pairing."),L"Hop",MB_OK);break;}
            HMENU found=CreatePopupMenu();if(!found)break;
            for(std::size_t i=0;i<candidates.size();++i){const auto& label=candidates[i].instance;AppendMenuW(found,MF_STRING,static_cast<UINT_PTR>(i+1),label.c_str());}
            POINT point{};GetCursorPos(&point);const auto chosen=TrackPopupMenu(found,TPM_RETURNCMD|TPM_NONOTIFY,point.x,point.y,0,window,nullptr);DestroyMenu(found);
            if(chosen>0 && static_cast<std::size_t>(chosen)<=candidates.size()){const auto& candidate=candidates[static_cast<std::size_t>(chosen)-1];SetWindowTextW(app->address,candidate.address.c_str());SetWindowTextW(app->port,std::to_wstring(candidate.port).c_str());
                if(!start_pairing(*app,false)){app->show_manual=true;update_controls(*app);}}
            break;
        }
        case pair_listen_id:case pair_connect_id:
            try{if(!start_pairing(*app,LOWORD(wparam)==pair_listen_id))MessageBoxW(window,localized(*app,L"无法启动配对，请检查地址、端口与显示器权限。",L"Could not start pairing. Check address, port and display access."),L"Hop",MB_OK|MB_ICONWARNING);}catch(...){MessageBoxW(window,L"Pairing initialization failed.",L"Hop",MB_OK|MB_ICONWARNING);}break;
        case peer_settings_id:
            if(app->runtime){const auto peer=app->runtime->peer_fingerprint();if(online_peer(*app,peer)){LayoutBox box{};box.device_id=peer;POINT point{270,245};pointer_menu(*app,window,box,point);}}
            break;
        case peer_list_id: {
            if(!can_change_peer(*app))break;
            const auto peers=app->runtime->connected_peers();
            HMENU choices=CreatePopupMenu();if(!choices)break;
            const auto current=app->runtime->peer_fingerprint();
            for(std::size_t i=0;i<peers.size();++i) {
                const auto label=peer_label(*app,peers[i]);
                AppendMenuW(choices,MF_STRING|(peers[i].fingerprint==current?MF_CHECKED:MF_UNCHECKED),static_cast<UINT_PTR>(i+1),label.c_str());
            }
            POINT point{};GetCursorPos(&point);
            const auto choice=TrackPopupMenu(choices,TPM_RETURNCMD|TPM_NONOTIFY,point.x,point.y,0,window,nullptr);
            DestroyMenu(choices);
            if(choice>0 && static_cast<std::size_t>(choice)<=peers.size() && can_change_peer(*app) &&
               app->runtime->request_select_peer(peers[static_cast<std::size_t>(choice)-1].fingerprint)) {
                app->peer_change_pending=true;update_controls(*app);
            }
            break;
        }
#endif
        case layout_id: show_layout(*app); break;
        case quit_id: DestroyWindow(window); break;
        default: break;
        }
        return 0;
    case WM_CLOSE: if (app->tray_added) ShowWindow(window, SW_HIDE); else DestroyWindow(window); return 0;
#ifdef HOP_WINDOWS_EXPERIMENTAL_TLS
    case WM_DISPLAYCHANGE:
        if(app->runtime && !same_local_geometry(app->transport_geometry)){
            app->allow_receiver.store(false);
            app->runtime->request_stop();
            if(++app->runtime_generation==0)++app->runtime_generation;
            app->discovery.stop_advertising();
            clear_live_layout(*app);
            app->edge_armed=false;app->auto_edge_desired=false;app->control_pending=false;app->peer_change_pending=false;app->state.connected(false);
            update_controls(*app);
            set_pair_status(*app,L"显示器已变化，连接正在关闭，请重新连接。",L"Displays changed; closing connection. Please reconnect.");
        }
        return 0;
#endif
    case WM_DESTROY:
#ifdef HOP_WINDOWS_EXPERIMENTAL_TLS
        app->event_window.store(nullptr);app->allow_receiver.store(false);if(app->runtime)app->runtime->request_stop();
#endif
        if(app->layout_window)DestroyWindow(app->layout_window);
        for(auto brush:{app->page_brush,app->card_brush})if(brush)DeleteObject(brush);
        for(auto font:{app->headline,app->body})if(font)DeleteObject(font);
        remove_tray(*app);PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
UiLanguage language_from_system() {
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE ? UiLanguage::chinese : UiLanguage::english;
}
UiLanguage language_from_args() {
    UiLanguage result = language_from_system();
    int count = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (args) {
        for (int i = 1; i < count; ++i) {
            if (std::wcscmp(args[i], L"--lang=zh") == 0) result = UiLanguage::chinese;
            else if (std::wcscmp(args[i], L"--lang=en") == 0) result = UiLanguage::english;
        }
        LocalFree(args);
    }
    return result;
}
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    const auto mutex = CreateMutexW(nullptr, FALSE, lifecycle_mutex);
    const auto mutex_error = GetLastError();
    if (!mutex) return 1;
    struct LifecycleGuard {
        HANDLE handle;
        ~LifecycleGuard() { CloseHandle(handle); }
    } lifecycle{mutex};
    if (mutex_error == ERROR_ALREADY_EXISTS) {
        if (const auto existing = FindWindowW(window_class, nullptr)) PostMessageW(existing, WM_COMMAND, show_id, 0);
        return 0;
    }
#ifdef HOP_WINDOWS_EXPERIMENTAL_TLS
    WSADATA winsock{};if(WSAStartup(MAKEWORD(2,2),&winsock)!=0)return 1;
    struct WinsockGuard {~WinsockGuard(){WSACleanup();}} winsock_guard;
#endif
    App app;
    app.language = language_from_args();
    bool found=false;
    app.layout_storage_ok=load_layout(app.layout_storage,app.editor,found);
    app.taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSW type{};
    type.lpfnWndProc = window_proc; type.hInstance = instance; type.lpszClassName = window_class;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW); type.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassW(&type)) return 1;
    WNDCLASSW layout_type=type;
    layout_type.lpfnWndProc=layout_proc; layout_type.lpszClassName=layout_class; layout_type.style=CS_DBLCLKS;
    if (!RegisterClassW(&layout_type)) return 1;
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, window_class, strings(app).title,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, 420,
#ifdef HOP_WINDOWS_EXPERIMENTAL_TLS
        455,
#else
        190,
#endif
        nullptr, nullptr, instance, &app);
    if (!window) return 1;
    if (!add_tray(app)) show_status(app);
    MSG msg{};
    int result;
    while ((result = static_cast<int>(GetMessageW(&msg, nullptr, 0, 0))) > 0) {
        if (!(app.layout_window && IsDialogMessageW(app.layout_window,&msg)) && !IsDialogMessageW(window,&msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
#ifdef HOP_WINDOWS_EXPERIMENTAL_TLS
    if(app.runtime)app.runtime->stop();
#endif
    return result == -1 ? 1 : static_cast<int>(msg.wParam);
}
