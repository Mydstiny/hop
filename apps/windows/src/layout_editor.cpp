#include "hop_windows/layout_editor.h"
#include <algorithm>
#include <limits>
#include <unordered_set>
#include <utility>
namespace hop::windows {
namespace {
bool fail(std::string* error,const char* text){if(error)*error=text;return false;}
}
bool LayoutEditor::replace(std::vector<LayoutDevice> devices,std::string* error) {
    if(devices.size()>64) return fail(error,"too many devices");
    std::unordered_set<std::string> ids; std::vector<hop::Screen> screens;
    for(const auto& device:devices) {
        if(device.id.empty() || device.id.size()>128 || device.name.empty() || device.name.size()>128 || device.screens.empty() || device.screens.size()>16)
            return fail(error,"invalid device");
        if(!ids.insert(device.id).second) return fail(error,"duplicate device id");
        for(const auto& screen:device.screens) {
            if(screen.device_id!=device.id || screen.id.size()>128) return fail(error,"invalid screen owner");
            screens.push_back(screen);
        }
    }
    hop::Layout candidate;
    if(!candidate.replace(std::move(screens),error)) return false;
    devices_=std::move(devices); layout_=std::move(candidate);
    if(!selected_.empty() && !ids.count(selected_)) selected_.clear();
    if(error) error->clear();
    return true;
}
bool LayoutEditor::select(const std::string& id) {
    if(std::none_of(devices_.begin(),devices_.end(),[&](const LayoutDevice& d){return d.id==id;})) return false;
    selected_=id;return true;
}
bool LayoutEditor::expand(const std::string& id,bool value) {
    for(auto& d:devices_) if(d.id==id){d.expanded=value;return true;}
    return false;
}
bool LayoutEditor::move_device(const std::string& id,std::int32_t dx,std::int32_t dy,std::string* error) {
    auto candidate=devices_; bool found=false;
    for(auto& device:candidate) if(device.id==id) {
        found=true;
        for(auto& screen:device.screens) {
            const auto x=std::int64_t(screen.x)+dx, y=std::int64_t(screen.y)+dy;
            if(x<std::numeric_limits<std::int32_t>::min() || x>std::numeric_limits<std::int32_t>::max() ||
               y<std::numeric_limits<std::int32_t>::min() || y>std::numeric_limits<std::int32_t>::max()) return fail(error,"coordinate overflow");
            screen.x=static_cast<std::int32_t>(x); screen.y=static_cast<std::int32_t>(y);
        }
    }
    return found ? replace(std::move(candidate),error) : fail(error,"unknown device");
}
bool LayoutEditor::move_screen(const std::string& id,std::int32_t x,std::int32_t y,std::string* error) {
    auto candidate=devices_; bool found=false;
    for(auto& device:candidate) for(auto& screen:device.screens) if(screen.id==id) {found=true;screen.x=x;screen.y=y;}
    return found ? replace(std::move(candidate),error) : fail(error,"unknown screen");
}
} // namespace hop::windows
