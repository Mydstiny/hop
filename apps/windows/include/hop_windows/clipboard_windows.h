#pragma once
#include "hop_windows/clipboard.h"
#include <array>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace hop::windows {
// Construct/use only as the backend of an authenticated ClipboardSync. No
// clipboard is opened by construction; the application explicitly starts a
// listener and calls the policy on WM_CLIPBOARDUPDATE.
class WindowsClipboardBackend final : public ClipboardBackend {
public:
    explicit WindowsClipboardBackend(HWND owner);
    bool read(std::uint32_t expected_sequence, ClipboardPayload& out) override;
    bool write(const ClipboardPayload& payload, std::uint32_t expected_sequence, std::uint32_t& written_sequence) override;
    ClipboardWriteResult write_guarded(const ClipboardPayload& payload,std::uint32_t expected_sequence,std::uint32_t& written_sequence) override;
private:
    HWND owner_;
    UINT png_format_;
    std::array<UINT,5> file_formats_{};
};
class ClipboardListener {
public:
    ClipboardListener() = default;
    ~ClipboardListener();
    ClipboardListener(const ClipboardListener&) = delete;
    ClipboardListener& operator=(const ClipboardListener&) = delete;
    bool start(HWND window);
    void stop();
private:
    HWND window_ = nullptr;
};
} // namespace hop::windows
