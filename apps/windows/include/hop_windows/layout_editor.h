#pragma once
#include "hop/layout.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace hop::windows {
struct LayoutDevice {
    std::string id;                 // stable verified device identity, not address
    std::wstring name;              // display only; never treated as identity
    bool online = false;
    bool expanded = false;
    std::vector<hop::Screen> screens;
    bool trusted = false;           // supplied by authenticated session only
};
// A single-threaded UI/control-plane model. Changes are atomic: if a proposal
// overlaps another screen or violates shared geometry limits, the previous
// layout and selection remain unchanged. This model does not discover devices,
// persist data, establish trust, or alter real displays.
class LayoutEditor {
public:
    bool replace(std::vector<LayoutDevice> devices, std::string* error = nullptr);
    bool select(const std::string& device_id);
    bool expand(const std::string& device_id, bool value);
    bool move_device(const std::string& device_id, std::int32_t dx, std::int32_t dy, std::string* error = nullptr);
    bool move_screen(const std::string& screen_id, std::int32_t x, std::int32_t y, std::string* error = nullptr);
    const std::vector<LayoutDevice>& devices() const { return devices_; }
    const std::string& selected_device() const { return selected_; }
    const hop::Layout& layout() const { return layout_; }
private:
    std::vector<LayoutDevice> devices_;
    std::string selected_;
    hop::Layout layout_;
};
} // namespace hop::windows
