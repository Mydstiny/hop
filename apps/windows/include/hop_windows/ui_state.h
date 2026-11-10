#pragma once

namespace hop::windows {
enum class UiLanguage { english, chinese };
struct UiStrings {
    const wchar_t* title;
    const wchar_t* disconnected;
    const wchar_t* connected;
    const wchar_t* sharing;
    const wchar_t* show;
    const wchar_t* enable;
    const wchar_t* pause;
    const wchar_t* layout;
    const wchar_t* layout_empty;
    const wchar_t* layout_help;
    const wchar_t* layout_instructions;
    const wchar_t* layout_invalid;
    const wchar_t* layout_save_failed;
    const wchar_t* sensitivity;
    const wchar_t* motion_frequency;
    const wchar_t* scroll_horizontal;
    const wchar_t* scroll_vertical;
    const wchar_t* reverse_horizontal;
    const wchar_t* reverse_vertical;
    const wchar_t* settings_unavailable;
    const wchar_t* quit;
    const wchar_t* unavailable;
};
const UiStrings& ui_strings(UiLanguage language);

// No backend is created by this model. The owner must call connected(true)
// only after a real authenticated v2 session exists. Disconnect always pauses.
class UiState {
public:
    bool connected() const { return connected_; }
    bool sharing() const { return sharing_; }
    bool can_enable() const { return connected_ && !sharing_; }
    bool can_pause() const { return connected_ && sharing_; }
    void connected(bool value) { connected_ = value; if (!value) sharing_ = false; }
    bool enable() { if (!can_enable()) return false; sharing_ = true; return true; }
    bool pause() { if (!can_pause()) return false; sharing_ = false; return true; }
private:
    bool connected_ = false;
    bool sharing_ = false;
};
} // namespace hop::windows
