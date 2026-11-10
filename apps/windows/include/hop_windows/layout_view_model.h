#pragma once
#include "hop_windows/layout_editor.h"
#include <optional>
#include <vector>
namespace hop::windows {
struct LayoutBox {
    int left, top, right, bottom;
    std::string device_id, screen_id; // empty screen_id means collapsed device
    std::wstring name;
    bool online, selected, expanded;
};
struct LayoutView {
    double scale = 0;
    std::vector<LayoutBox> boxes;
};
// A pure projection of the editor model. No real desktop display is queried.
LayoutView project_layout(const LayoutEditor& editor, int width, int height);
std::optional<LayoutBox> hit_layout(const LayoutView& view, int x, int y);
bool device_allows_pointer_settings(const LayoutDevice& device);
} // namespace hop::windows
