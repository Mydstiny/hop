#include "hop_windows/displays.h"
#include <windows.h>
#include <cstdint>
#include <limits>
namespace hop::windows {
namespace {
struct Collector { std::vector<LocalDisplay> displays; bool failed=false; };
BOOL CALLBACK visit(HMONITOR monitor,HDC,LPRECT,LPARAM data) {
    auto& result=*reinterpret_cast<Collector*>(data); MONITORINFO info{}; info.cbSize=sizeof(info);
    if(result.displays.size()>=64 || !GetMonitorInfoW(monitor,&info)) {result.failed=true;return FALSE;}
    const auto width=std::int64_t(info.rcMonitor.right)-info.rcMonitor.left,height=std::int64_t(info.rcMonitor.bottom)-info.rcMonitor.top;
    if(width<=0 || height<=0 || width>65535 || height>65535) {result.failed=true;return FALSE;}
    try { result.displays.push_back({result.displays.size()+1,{info.rcMonitor.left,info.rcMonitor.top,static_cast<std::uint32_t>(width),static_cast<std::uint32_t>(height)}}); }
    catch (...) {result.failed=true;return FALSE;}
    return TRUE;
}
}
bool enumerate_local_displays(LocalDisplaySnapshot& output) {
    output={};
    using SetContext=HANDLE (WINAPI*)(HANDLE);
    auto user32=GetModuleHandleW(L"user32.dll"); if(!user32)return false;
    auto set_context=reinterpret_cast<SetContext>(GetProcAddress(user32,"SetThreadDpiAwarenessContext")); if(!set_context)return false;
    // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 is the documented -4 handle.
    auto old=set_context(reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(-4))); if(!old)return false;
    Collector collector; const BOOL ok=EnumDisplayMonitors(nullptr,nullptr,visit,reinterpret_cast<LPARAM>(&collector));
    const int x=GetSystemMetrics(SM_XVIRTUALSCREEN), y=GetSystemMetrics(SM_YVIRTUALSCREEN),w=GetSystemMetrics(SM_CXVIRTUALSCREEN),h=GetSystemMetrics(SM_CYVIRTUALSCREEN);
    const bool restored=set_context(old)!=nullptr;
    if(!ok || collector.failed || collector.displays.empty() || !restored || w<=0 || h<=0)return false;
    output.displays=std::move(collector.displays);output.desktop={x,y,static_cast<std::uint32_t>(w),static_cast<std::uint32_t>(h)};
    return true;
}
}
