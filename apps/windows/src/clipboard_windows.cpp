#include "hop_windows/clipboard_windows.h"
#include "hop_windows/image_codec.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace hop::windows {
namespace {
struct Clip {
    bool opened;
    explicit Clip(HWND owner) : opened(OpenClipboard(owner) != FALSE) {}
    ~Clip() { if (opened) CloseClipboard(); }
};
struct Locked {
    HGLOBAL h;
    void* p;
    explicit Locked(HGLOBAL value) : h(value), p(GlobalLock(value)) {}
    ~Locked() { if (p) GlobalUnlock(h); }
};
bool text_from_clipboard(HGLOBAL memory, ClipboardPayload& out) {
    const auto size = GlobalSize(memory);
    if (size < sizeof(wchar_t) || size % sizeof(wchar_t) || size > (max_text_bytes + 1) * sizeof(wchar_t)) return false;
    Locked locked(memory); if (!locked.p) return false;
    const auto* wide = static_cast<const wchar_t*>(locked.p);
    const auto max_chars = size / sizeof(wchar_t);
    const auto* end = std::find(wide, wide + max_chars, L'\0');
    if (end == wide + max_chars) return false;
    const auto len = static_cast<int>(end - wide);
    ClipboardPayload candidate{ClipboardFormat::utf8_text, {}};
    if (len) {
        const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, len, nullptr, 0, nullptr, nullptr);
        if (count <= 0 || static_cast<std::size_t>(count) > max_text_bytes) return false;
        candidate.bytes.resize(count);
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, len, reinterpret_cast<char*>(candidate.bytes.data()), count, nullptr, nullptr) != count) return false;
    }
    if (!valid_clipboard_payload(candidate)) return false;
    out = std::move(candidate); return true;
}
bool png_from_clipboard(HGLOBAL memory, ClipboardPayload& out) {
    const auto size = GlobalSize(memory);
    if (size < 33 || size > max_png_bytes) return false;
    Locked locked(memory); if (!locked.p) return false;
    const auto* p = static_cast<const std::uint8_t*>(locked.p);
    ClipboardPayload candidate{ClipboardFormat::png, std::vector<std::uint8_t>(p, p + size)};
    if (!valid_clipboard_payload(candidate)) return false;
    out = std::move(candidate); return true;
}
HGLOBAL make_payload(const ClipboardPayload& payload) {
    if (!valid_clipboard_payload(payload)) return nullptr;
    if (payload.format == ClipboardFormat::png) {
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, payload.bytes.size());
        if (!memory) return nullptr;
        void* data = GlobalLock(memory);
        if (!data) { GlobalFree(memory); return nullptr; }
        std::memcpy(data, payload.bytes.data(), payload.bytes.size()); GlobalUnlock(memory);
        return memory;
    }
    if (payload.bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return nullptr;
    int length = 0;
    if (!payload.bytes.empty()) {
        length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                     reinterpret_cast<const char*>(payload.bytes.data()), static_cast<int>(payload.bytes.size()), nullptr, 0);
        if (length <= 0) return nullptr;
    }
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, (static_cast<std::size_t>(length) + 1) * sizeof(wchar_t));
    if (!memory) return nullptr;
    auto* data = static_cast<wchar_t*>(GlobalLock(memory));
    if (!data) { GlobalFree(memory); return nullptr; }
    bool ok = !length || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(payload.bytes.data()),
                                            static_cast<int>(payload.bytes.size()), data, length) == length;
    GlobalUnlock(memory);
    if (!ok) { GlobalFree(memory); return nullptr; }
    return memory;
}
HGLOBAL copy_bytes(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) return nullptr;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    if (!memory) return nullptr;
    void* data = GlobalLock(memory);
    if (!data) { GlobalFree(memory); return nullptr; }
    std::memcpy(data, bytes.data(), bytes.size()); GlobalUnlock(memory);
    return memory;
}
}
WindowsClipboardBackend::WindowsClipboardBackend(HWND owner) : owner_(owner), png_format_(RegisterClipboardFormatW(L"PNG")) {
    const wchar_t* names[]={L"FileGroupDescriptorW",L"FileGroupDescriptor",L"FileContents",L"FileNameW",L"text/uri-list"};
    for(std::size_t i=0;i<file_formats_.size();++i)file_formats_[i]=RegisterClipboardFormatW(names[i]);
}
bool WindowsClipboardBackend::read(std::uint32_t expected_sequence, ClipboardPayload& out) {
    if (!expected_sequence || !owner_ || !IsWindow(owner_)) return false;
    std::vector<std::uint8_t> dib;
    try {
        {
            Clip clip(owner_); if (!clip.opened || GetClipboardSequenceNumber() != expected_sequence) return false;
            // A file list is never examined or transferred, even when the owner
            // also advertises a text or image representation.
            bool file_object=IsClipboardFormatAvailable(CF_HDROP)!=FALSE;
            for(const auto format:file_formats_) {if(!format)return false;file_object=file_object || IsClipboardFormatAvailable(format)!=FALSE;}
            const auto selected = select_clipboard_format(file_object,
                png_format_ && IsClipboardFormatAvailable(png_format_), IsClipboardFormatAvailable(CF_DIBV5) != FALSE,
                IsClipboardFormatAvailable(CF_DIB) != FALSE, IsClipboardFormatAvailable(CF_UNICODETEXT) != FALSE);
            if (selected == ClipboardSelection::png) {
                auto memory = static_cast<HGLOBAL>(GetClipboardData(png_format_));
                return memory && png_from_clipboard(memory, out) && GetClipboardSequenceNumber() == expected_sequence;
            }
            if (selected == ClipboardSelection::text) {
                auto memory = static_cast<HGLOBAL>(GetClipboardData(CF_UNICODETEXT));
                return memory && text_from_clipboard(memory, out) && GetClipboardSequenceNumber() == expected_sequence;
            }
            if (selected != ClipboardSelection::dib && selected != ClipboardSelection::dibv5) return false;
            auto memory = static_cast<HGLOBAL>(GetClipboardData(selected == ClipboardSelection::dibv5 ? CF_DIBV5 : CF_DIB));
            if (!memory) return false;
            const auto size = GlobalSize(memory);
            constexpr std::uint64_t max_dib_bytes = max_png_pixels * 4 + 65536 + 124;
            if (size < 40 || size > max_dib_bytes) return false;
            Locked locked(memory); if (!locked.p) return false;
            const auto* first = static_cast<const std::uint8_t*>(locked.p);
            dib.assign(first, first + size);
            if (GetClipboardSequenceNumber() != expected_sequence) return false;
        }
        // Encoding can be expensive: the clipboard is closed before invoking WIC.
        ClipboardPayload result{ClipboardFormat::png, {}};
        if (!dib_to_png(dib.data(), dib.size(), result.bytes) || GetClipboardSequenceNumber() != expected_sequence) return false;
        out = std::move(result); return true;
    } catch (...) { return false; }
}
bool WindowsClipboardBackend::write(const ClipboardPayload& payload, std::uint32_t expected_sequence, std::uint32_t& written_sequence) {
    return write_guarded(payload,expected_sequence,written_sequence)==ClipboardWriteResult::written;
}
ClipboardWriteResult WindowsClipboardBackend::write_guarded(const ClipboardPayload& payload, std::uint32_t expected_sequence, std::uint32_t& written_sequence) {
    written_sequence = 0;
    if (!owner_ || !IsWindow(owner_) || !expected_sequence || !valid_clipboard_payload(payload)) return ClipboardWriteResult::error;
    const UINT format = payload.format == ClipboardFormat::png ? png_format_ : CF_UNICODETEXT;
    if (!format) return ClipboardWriteResult::error;
    HGLOBAL memory = make_payload(payload); if (!memory) return ClipboardWriteResult::error;
    HGLOBAL dib_memory = nullptr;
    if (payload.format == ClipboardFormat::png) {
        std::vector<std::uint8_t> dib;
        if (!png_to_dibv5(payload.bytes, dib) || !(dib_memory = copy_bytes(dib))) { GlobalFree(memory); return ClipboardWriteResult::error; }
    }
    Clip clip(owner_);
    if (!clip.opened) { GlobalFree(memory); if (dib_memory) GlobalFree(dib_memory); return ClipboardWriteResult::error; }
    if (GetClipboardSequenceNumber() != expected_sequence) { GlobalFree(memory); if (dib_memory) GlobalFree(dib_memory); return ClipboardWriteResult::conflict; }
    if (!EmptyClipboard()) { GlobalFree(memory); if (dib_memory) GlobalFree(dib_memory); return ClipboardWriteResult::error; }
    if (!SetClipboardData(format, memory)) { GlobalFree(memory); if (dib_memory) GlobalFree(dib_memory); return ClipboardWriteResult::error; }
    // Clipboard now owns memory; never free it ourselves.
    if (dib_memory && !SetClipboardData(CF_DIBV5, dib_memory)) { GlobalFree(dib_memory); return ClipboardWriteResult::error; }
    written_sequence = GetClipboardSequenceNumber();
    return written_sequence ? ClipboardWriteResult::written : ClipboardWriteResult::error;
}
ClipboardListener::~ClipboardListener() { stop(); }
bool ClipboardListener::start(HWND window) {
    if (window_ || !window || !IsWindow(window)) return false;
    if (!AddClipboardFormatListener(window)) return false;
    window_ = window; return true;
}
void ClipboardListener::stop() {
    if (window_) { RemoveClipboardFormatListener(window_); window_ = nullptr; }
}
} // namespace hop::windows
