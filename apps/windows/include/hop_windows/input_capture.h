#pragma once
#include "hop_windows/input_ownership.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace hop::windows {
enum class CaptureKind { key, button, move, wheel };
struct CaptureEvent {
    CaptureKind kind;
    std::uint16_t code = 0;
    bool down = false;
    bool extended = false;
    std::int32_t x = 0, y = 0;
    bool e1 = false;
    bool repeat = false;
    std::uint16_t virtual_key = 0; // retained to distinguish legacy Pause/NumLock
};
// Experimental low-level hooks. No network, timers, UI or injection here.
// Exactly one instance may be installed per process. Methods except poll are
// called from the owner's control thread. Events are consumed by one thread.
class InputCapture {
public:
    InputCapture() = default;
    ~InputCapture();
    InputCapture(const InputCapture&) = delete;
    InputCapture& operator=(const InputCapture&) = delete;
    bool start();
    // Asynchronous: active() only becomes true after the hook worker processes
    // this request and checks that no physical keyboard/mouse button is held.
    bool request_activation();
    bool request_deactivation();
    // Explicit last-resort escape if a disconnected peripheral never sends up.
    bool request_force_recovery();
    bool active() const { return active_.load(); }
    bool drained() const { return drained_.load(); }
    bool take_overflow() { return overflow_.exchange(false); }
    bool take_emergency() { return emergency_.exchange(false); }
    bool poll(CaptureEvent& event);
    // Refuses normal stop while remote-owned releases still need draining.
    bool stop();
private:
    static LRESULT CALLBACK keyboard(int, WPARAM, LPARAM);
    static LRESULT CALLBACK mouse(int, WPARAM, LPARAM);
    bool enqueue(const CaptureEvent& event);
    void overflow();
    void run();
    void force_stop();
    static std::atomic<InputCapture*> singleton_;
    static constexpr std::size_t capacity = 512;
    std::array<CaptureEvent, capacity> queue_{};
    std::atomic<std::size_t> head_{0}, tail_{0};
    std::atomic<bool> active_{false}, drained_{true}, overflow_{false}, emergency_{false};
    std::atomic<DWORD> thread_id_{0};
    InputOwnership ownership_; // hook thread only
    std::thread thread_;
    std::mutex init_mutex_;
    std::condition_variable init_cv_;
    bool init_done_ = false, init_ok_ = false;
    std::mutex stop_mutex_;
    std::condition_variable stop_cv_;
    bool stop_done_ = false, stop_ok_ = false, stop_pending_ = false;
    bool left_ctrl_ = false, right_ctrl_ = false, left_alt_ = false, right_alt_ = false;
    HHOOK key_hook_ = nullptr, mouse_hook_ = nullptr;
};
} // namespace hop::windows
