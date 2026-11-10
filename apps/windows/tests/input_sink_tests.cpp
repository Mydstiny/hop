#include "hop_windows/input_sink.h"
#include "hop_windows/input_capture.h"
#include <cstdlib>
#include <iostream>
#include <vector>
using namespace hop::windows;
namespace {
int checks = 0;
void require(bool value, const char* text, int line) {
    ++checks;
    if (!value) { std::cerr << "FAIL line " << line << ": " << text << '\n'; std::exit(1); }
}
#define CHECK(x) require(bool(x), #x, __LINE__)
}
int main() {
    // Construction/destruction alone must not install hooks or query input.
    InputCapture unopened;
    CaptureEvent empty{};
    CHECK(!unopened.active() && unopened.drained() && !unopened.poll(empty));
    CHECK(!unopened.request_activation() && !unopened.request_deactivation() && !unopened.request_force_recovery());
    CHECK(unopened.stop());
    std::vector<INPUT> calls;
    bool fail_next = false;
    InputSink sink([&](UINT count, LPINPUT input, int size) -> UINT {
        CHECK(count == 1 && size == sizeof(INPUT));
        calls.push_back(*input);
        if (fail_next) { fail_next = false; return 0; }
        return 1;
    });
    CHECK(!sink.key({0,false}, true));
    CHECK(sink.last_error() == SinkError::invalid_input);
    CHECK(!sink.key({0x45,false,true}, true)); // E1 is never silently injected as NumLock
    CHECK(sink.last_error() == SinkError::unsupported_sequence);
    CHECK(!sink.key({0x45,true,true}, true) && sink.last_error() == SinkError::invalid_input);
    CHECK(!sink.key({30,false}, false));
    CHECK(sink.last_error() == SinkError::unmatched_release);
    CHECK(calls.empty());
    CHECK(sink.key({30,false}, true));
    CHECK(sink.held_keys() == 1 && calls.back().type == INPUT_KEYBOARD);
    CHECK(calls.back().ki.wScan == 30 && calls.back().ki.wVk == 0);
    CHECK(calls.back().ki.dwFlags == KEYEVENTF_SCANCODE);
    CHECK(sink.key({30,false}, true)); // repeat down remains held once
    CHECK(sink.held_keys() == 1);
    CHECK(sink.key({29,true}, true));
    CHECK(calls.back().ki.dwFlags == (KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY));
    CHECK(sink.key({29,true}, false));
    CHECK(calls.back().ki.dwFlags == (KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP));
    CHECK(!sink.key({29,true}, false));
    CHECK(!sink.button(static_cast<Button>(255), true));
    CHECK(!sink.button(Button::right, false));
    CHECK(sink.button(Button::back, true));
    CHECK(calls.back().mi.dwFlags == MOUSEEVENTF_XDOWN && calls.back().mi.mouseData == XBUTTON1);
    CHECK(!sink.button(Button::back, true));
    CHECK(sink.last_error() == SinkError::duplicate_button_down);
    CHECK(sink.button(Button::forward, true));
    CHECK(calls.back().mi.mouseData == XBUTTON2);
    CHECK(sink.button(Button::forward, false));
    CHECK(calls.back().mi.dwFlags == MOUSEEVENTF_XUP && calls.back().mi.mouseData == XBUTTON2);
    CHECK(sink.move_absolute(123, 65000));
    CHECK(calls.back().mi.dx == 123 && calls.back().mi.dy == 65000);
    CHECK((calls.back().mi.dwFlags & (MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK|MOUSEEVENTF_MOVE)) ==
          (MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK|MOUSEEVENTF_MOVE));
    auto before = calls.size();
    CHECK(sink.scroll(0, 0) && calls.size() == before);
    CHECK(sink.scroll(120, 240));
    CHECK(calls.size() == before + 2);
    CHECK(calls[before].mi.dwFlags == MOUSEEVENTF_HWHEEL && static_cast<LONG>(calls[before].mi.mouseData) == 120);
    CHECK(calls[before+1].mi.dwFlags == MOUSEEVENTF_WHEEL && static_cast<LONG>(calls[before+1].mi.mouseData) == -240);
    CHECK(!sink.scroll(120001, 0));
    CHECK(!sink.scroll(0, -120001));
    CHECK(sink.key({42,false}, true));
    CHECK(sink.button(Button::left, true));
    fail_next = true;
    CHECK(!sink.release_all());
    CHECK(sink.last_error() == SinkError::system_rejected);
    CHECK(sink.held_keys() == 1); // failed first release is retained
    CHECK(sink.held_buttons() == 0); // still attempted all other releases
    CHECK(sink.release_all());
    CHECK(sink.held_keys() == 0 && sink.held_buttons() == 0);
    CHECK(sink.release_all());
    fail_next = true;
    CHECK(!sink.key({20,false}, true));
    CHECK(sink.held_keys() == 0);
    fail_next = true;
    CHECK(!sink.button(Button::middle, true));
    CHECK(sink.held_buttons() == 0);
    CHECK(!calls.empty());
    for (const auto& event : calls) {
        CHECK((event.type == INPUT_MOUSE ? event.mi.dwExtraInfo : event.ki.dwExtraInfo) == 0x484f5032u);
    }
    std::cout << "PASS " << checks << " Windows input adapter checks; fake SendInput only\n";
}
