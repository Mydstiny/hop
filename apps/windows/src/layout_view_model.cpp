#include "hop_windows/layout_view_model.h"
#include "hop_windows/pointer_settings.h"
#include <algorithm>
#include <cmath>
#include <limits>
namespace hop::windows {
LayoutView project_layout(const LayoutEditor& editor,int width,int height) {
    LayoutView result;
    if(width<100 || height<150 || editor.devices().empty()) return result;
    std::int64_t minx=std::numeric_limits<std::int64_t>::max(), miny=minx;
    std::int64_t maxx=std::numeric_limits<std::int64_t>::min(), maxy=maxx;
    for(const auto& device:editor.devices()) for(const auto& screen:device.screens) {
        minx=std::min(minx,std::int64_t(screen.x));miny=std::min(miny,std::int64_t(screen.y));
        maxx=std::max(maxx,std::int64_t(screen.x)+screen.width);maxy=std::max(maxy,std::int64_t(screen.y)+screen.height);
    }
    if(maxx<=minx || maxy<=miny) return result;
    result.scale=std::min(double(width-40)/double(maxx-minx),double(height-110)/double(maxy-miny));
    if(!std::isfinite(result.scale) || result.scale<=0) {result.scale=0;return result;}
    const auto xcoord=[&](std::int64_t x){return 20+int(std::round(double(x-minx)*result.scale));};
    const auto ycoord=[&](std::int64_t y){return 45+int(std::round(double(y-miny)*result.scale));};
    for(const auto& d:editor.devices()) {
        auto add=[&](std::int64_t x,std::int64_t y,std::int64_t right,std::int64_t bottom,const std::string& sid) {
            const auto l=xcoord(x),t=ycoord(y);
            result.boxes.push_back({l,t,std::max(l+2,xcoord(right)),std::max(t+2,ycoord(bottom)),d.id,sid,d.name,d.online,editor.selected_device()==d.id,d.expanded});
        };
        if(d.expanded) {
            for(const auto& s:d.screens)add(s.x,s.y,std::int64_t(s.x)+s.width,std::int64_t(s.y)+s.height,s.id);
        } else {
            std::int64_t l=std::numeric_limits<std::int64_t>::max(),t=l,r=std::numeric_limits<std::int64_t>::min(),b=r;
            for(const auto& s:d.screens){l=std::min(l,std::int64_t(s.x));t=std::min(t,std::int64_t(s.y));r=std::max(r,std::int64_t(s.x)+s.width);b=std::max(b,std::int64_t(s.y)+s.height);}
            add(l,t,r,b,{});
        }
    }
    return result;
}
std::optional<LayoutBox> hit_layout(const LayoutView& view,int x,int y) {
    // Topmost (last painted) box wins for an overlapping collapsed envelope.
    for(auto it=view.boxes.rbegin();it!=view.boxes.rend();++it)
        if(x>=it->left && x<it->right && y>=it->top && y<it->bottom)return *it;
    return std::nullopt;
}
bool device_allows_pointer_settings(const LayoutDevice& device) {
    DeviceFingerprint fingerprint{};
    return device.online && device.trusted && parse_device_fingerprint(device.id,fingerprint);
}
} // namespace hop::windows
