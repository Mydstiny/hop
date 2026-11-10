#include "hop_windows/input_capture.h"
#include <chrono>

namespace hop::windows {
namespace {
constexpr UINT activate_message = WM_APP + 41;
constexpr UINT deactivate_message = WM_APP + 42;
constexpr UINT stop_message = WM_APP + 43;
constexpr UINT force_recovery_message = WM_APP + 44;
bool physical_input_held() {
    // Called only on activation, outside the hook callback. Include buttons.
    for (int vk = 1; vk < 256; ++vk) if (GetAsyncKeyState(vk) & 0x8000) return true;
    return false;
}
}
std::atomic<InputCapture*> InputCapture::singleton_{nullptr};
InputCapture::~InputCapture() { force_stop(); }
bool InputCapture::start() {
    if (thread_.joinable()) return false;
    InputCapture* expected = nullptr;
    if (!singleton_.compare_exchange_strong(expected, this)) return false;
    ownership_ = InputOwnership{};
    head_.store(0); tail_.store(0); active_.store(false); drained_.store(true);
    overflow_.store(false); emergency_.store(false);
    left_ctrl_ = right_ctrl_ = left_alt_ = right_alt_ = false;
    { std::lock_guard<std::mutex> guard(init_mutex_); init_done_ = false; init_ok_ = false; }
    try { thread_ = std::thread(&InputCapture::run, this); }
    catch (...) { singleton_.store(nullptr); return false; }
    std::unique_lock<std::mutex> lock(init_mutex_);
    init_cv_.wait(lock, [&] { return init_done_; });
    bool ok = init_ok_;
    lock.unlock();
    if (!ok) { thread_.join(); singleton_.store(nullptr); }
    return ok;
}
bool InputCapture::request_activation() {
    const auto id = thread_id_.load();
    return id && PostThreadMessageW(id, activate_message, 0, 0);
}
bool InputCapture::request_deactivation() {
    const auto id = thread_id_.load();
    return id && PostThreadMessageW(id, deactivate_message, 0, 0);
}
bool InputCapture::request_force_recovery() {
    const auto id = thread_id_.load();
    return id && PostThreadMessageW(id, force_recovery_message, 0, 0);
}
bool InputCapture::enqueue(const CaptureEvent& event) {
    const auto head = head_.load(std::memory_order_relaxed);
    const auto next = (head + 1) % capacity;
    if (next == tail_.load(std::memory_order_acquire)) return false;
    queue_[head] = event;
    head_.store(next, std::memory_order_release);
    return true;
}
bool InputCapture::poll(CaptureEvent& event) {
    const auto tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire)) return false;
    event = queue_[tail];
    tail_.store((tail + 1) % capacity, std::memory_order_release);
    return true;
}
void InputCapture::overflow() {
    ownership_.deactivate(); active_.store(false); overflow_.store(true);
    drained_.store(ownership_.drained());
}
LRESULT CALLBACK InputCapture::keyboard(int code, WPARAM message, LPARAM payload) {
    auto* self = singleton_.load();
    if (code < 0 || !self) return CallNextHookEx(nullptr, code, message, payload);
    const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(payload);
    if (!key || (key->flags & LLKHF_INJECTED)) return CallNextHookEx(nullptr, code, message, payload);
    const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const bool up = message == WM_KEYUP || message == WM_SYSKEYUP;
    if (!down && !up) return CallNextHookEx(nullptr, code, message, payload);
    const bool extended = (key->flags & LLKHF_EXTENDED) != 0;
    // Low-level callbacks expose the legacy Pause scan 0x45, not the raw E1
    // sequence. Preserve VK_PAUSE to disambiguate it from the NumLock key.
    const bool e1 = !extended && key->vkCode == VK_PAUSE &&
                    (key->scanCode == 0x45 || key->scanCode == 0xe11d45);
    const auto identity = static_cast<std::uint16_t>((key->scanCode & 0xff) | (extended ? 0x100 : 0) | (e1 ? 0x200 : 0));
    try {
        if (key->vkCode == VK_LCONTROL || (key->vkCode == VK_CONTROL && !extended)) self->left_ctrl_ = down;
        if (key->vkCode == VK_RCONTROL || (key->vkCode == VK_CONTROL && extended)) self->right_ctrl_ = down;
        if (key->vkCode == VK_LMENU || (key->vkCode == VK_MENU && !extended)) self->left_alt_ = down;
        if (key->vkCode == VK_RMENU || (key->vkCode == VK_MENU && extended)) self->right_alt_ = down;
        if (down && key->vkCode == VK_ESCAPE && self->ownership_.active() &&
            (self->left_ctrl_ || self->right_ctrl_) && (self->left_alt_ || self->right_alt_)) {
            // Esc and its physical tail belong to the remote capture. Do not
            // transmit the emergency trigger or leak its release locally.
            self->ownership_.key(identity, true);
            self->ownership_.deactivate(); self->active_.store(false);
            self->drained_.store(self->ownership_.drained()); self->emergency_.store(true);
            return 1;
        }
        const auto decision = self->ownership_.key(identity, down);
        self->drained_.store(self->ownership_.drained());
        CaptureEvent event{CaptureKind::key, static_cast<std::uint16_t>(key->scanCode & 0xff), down, extended};
        event.e1 = e1; event.repeat = decision.repeat; event.virtual_key = static_cast<std::uint16_t>(key->vkCode);
        if (decision.forward_remote && !self->enqueue(event)) self->overflow();
        return decision.suppress_local ? 1 : CallNextHookEx(nullptr, code, message, payload);
    } catch (...) { self->overflow(); return CallNextHookEx(nullptr, code, message, payload); }
}
LRESULT CALLBACK InputCapture::mouse(int code, WPARAM message, LPARAM payload) {
    auto* self = singleton_.load();
    if (code < 0 || !self) return CallNextHookEx(nullptr, code, message, payload);
    const auto* data = reinterpret_cast<const MSLLHOOKSTRUCT*>(payload);
    if (!data || (data->flags & LLMHF_INJECTED)) return CallNextHookEx(nullptr, code, message, payload);
    int button = -1; bool down = false; bool is_button = true;
    switch (message) {
    case WM_LBUTTONDOWN: button=0; down=true; break; case WM_LBUTTONUP: button=0; break;
    case WM_RBUTTONDOWN: button=1; down=true; break; case WM_RBUTTONUP: button=1; break;
    case WM_MBUTTONDOWN: button=2; down=true; break; case WM_MBUTTONUP: button=2; break;
    case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        button = HIWORD(data->mouseData) == XBUTTON1 ? 3 : 4; down = message == WM_XBUTTONDOWN; break;
    default: is_button=false; break;
    }
    try {
        if (is_button) {
            const auto decision = self->ownership_.button(static_cast<std::uint8_t>(button), down);
            self->drained_.store(self->ownership_.drained());
            if (decision.forward_remote && !self->enqueue({CaptureKind::button, static_cast<std::uint16_t>(button), down})) self->overflow();
            return decision.suppress_local ? 1 : CallNextHookEx(nullptr, code, message, payload);
        }
        if (message == WM_MOUSEMOVE) {
            const auto decision = self->ownership_.motion(0, false);
            // Passive moves support edge detection. If the queue is full in
            // passive mode, dropping them is safe; in remote mode it is a fault.
            if (!self->enqueue({CaptureKind::move, 0, false, false, data->pt.x, data->pt.y}) && decision.forward_remote) self->overflow();
            return decision.suppress_local ? 1 : CallNextHookEx(nullptr, code, message, payload);
        }
        if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) {
            const auto decision = self->ownership_.motion(0, false);
            const auto delta = static_cast<short>(HIWORD(data->mouseData));
            CaptureEvent event{CaptureKind::wheel};
            if (message == WM_MOUSEHWHEEL) event.x = delta; else event.y = -static_cast<std::int32_t>(delta);
            if (decision.forward_remote && !self->enqueue(event)) self->overflow();
            return decision.suppress_local ? 1 : CallNextHookEx(nullptr, code, message, payload);
        }
    } catch (...) { self->overflow(); }
    return CallNextHookEx(nullptr, code, message, payload);
}
void InputCapture::run() {
    MSG message{};
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // create message queue
    thread_id_.store(GetCurrentThreadId());
    key_hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, keyboard, GetModuleHandleW(nullptr), 0);
    mouse_hook_ = SetWindowsHookExW(WH_MOUSE_LL, mouse, GetModuleHandleW(nullptr), 0);
    {
        std::lock_guard<std::mutex> guard(init_mutex_);
        init_ok_ = key_hook_ && mouse_hook_; init_done_ = true;
    }
    init_cv_.notify_one();
    if (key_hook_ && mouse_hook_) {
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (message.message == activate_message) {
                // Do not enter remote mode until passive edge events have
                // drained; a stale full queue must not cause an immediate fault.
                if (head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire) &&
                    !physical_input_held() && ownership_.activate()) active_.store(true);
            } else if (message.message == deactivate_message) {
                ownership_.deactivate(); active_.store(false); drained_.store(ownership_.drained());
            } else if (message.message == force_recovery_message) {
                ownership_.abandon_remote(); active_.store(false); drained_.store(true);
            } else if (message.message == stop_message) {
                const bool ok = !ownership_.active() && ownership_.drained();
                { std::lock_guard<std::mutex> guard(stop_mutex_); stop_ok_ = ok; stop_done_ = true; }
                stop_cv_.notify_one();
                if (ok) break;
            } else { TranslateMessage(&message); DispatchMessageW(&message); }
        }
    }
    active_.store(false);
    if (key_hook_) UnhookWindowsHookEx(key_hook_);
    if (mouse_hook_) UnhookWindowsHookEx(mouse_hook_);
    key_hook_ = nullptr; mouse_hook_ = nullptr; thread_id_.store(0);
}
bool InputCapture::stop() {
    if (!thread_.joinable()) return true;
    const auto id = thread_id_.load();
    if (!id) { thread_.join(); singleton_.store(nullptr); return true; }
    std::unique_lock<std::mutex> lock(stop_mutex_);
    if (!stop_pending_) {
        stop_done_ = false; stop_ok_ = false; stop_pending_ = true;
        if (!PostThreadMessageW(id, stop_message, 0, 0)) { stop_pending_ = false; return false; }
    }
    if (!stop_cv_.wait_for(lock, std::chrono::seconds(1), [&] { return stop_done_; })) return false;
    stop_pending_ = false;
    if (!stop_ok_) return false;
    lock.unlock();
    thread_.join(); singleton_.store(nullptr); return true;
}
void InputCapture::force_stop() {
    if (!thread_.joinable()) return;
    const auto id = thread_id_.load();
    if (id) PostThreadMessageW(id, WM_QUIT, 0, 0);
    thread_.join();
    singleton_.store(nullptr);
}
} // namespace hop::windows
