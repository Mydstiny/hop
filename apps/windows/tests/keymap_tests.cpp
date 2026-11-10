#include "hop_windows/keymap.h"
#include "hop_windows/input_capture.h"
#include <cstdlib>
#include <iostream>
using namespace hop::windows;
int n = 0;
void check(bool b, const char* s, int line) { ++n; if (!b) { std::cerr << line << ": " << s << '\n'; std::exit(1); } }
#define CHECK(x) check(bool(x), #x, __LINE__)
int main() {
    int mapped = 0;
    for (unsigned usage = 0; usage <= 0xffff; ++usage) {
        const auto result = usage_to_scan(static_cast<std::uint16_t>(usage));
        if (result.status != KeyMapStatus::ok) continue;
        ++mapped;
        const auto inverse = scan_to_usage(result.scan);
        CHECK(inverse.status == KeyMapStatus::ok && inverse.usage == usage);
    }
    CHECK(mapped >= 110);
    auto a = usage_to_scan(0x04); CHECK(a.status == KeyMapStatus::ok && a.scan.code == 0x1e && !a.scan.extended && !a.scan.e1);
    auto lc = usage_to_scan(0xe0); auto rc = usage_to_scan(0xe4);
    CHECK(lc.scan.code == 0x1d && !lc.scan.extended && rc.scan.code == 0x1d && rc.scan.extended);
    auto ls = usage_to_scan(0xe1); auto rs = usage_to_scan(0xe5);
    CHECK(ls.scan.code == 0x2a && rs.scan.code == 0x36 && !ls.scan.extended && !rs.scan.extended);
    auto la = usage_to_scan(0xe2); auto ra = usage_to_scan(0xe6);
    CHECK(la.scan.code == 0x38 && !la.scan.extended && ra.scan.code == 0x38 && ra.scan.extended);
    auto lm = usage_to_scan(0xe3); auto rm = usage_to_scan(0xe7);
    CHECK(lm.scan.code == 0x5b && lm.scan.extended && rm.scan.code == 0x5c && rm.scan.extended);
    auto right = usage_to_scan(0x4f); auto keypad6 = usage_to_scan(0x5e);
    CHECK(right.scan.code == keypad6.scan.code && right.scan.extended && !keypad6.scan.extended);
    CHECK(scan_to_usage({0x4d,true}).usage == 0x4f && scan_to_usage({0x4d,false}).usage == 0x5e);
    auto enter = usage_to_scan(0x28); auto keypad_enter = usage_to_scan(0x58);
    CHECK(enter.scan.code == keypad_enter.scan.code && !enter.scan.extended && keypad_enter.scan.extended);
    auto pause = usage_to_scan(0x48); auto numlock = usage_to_scan(0x53);
    CHECK(pause.status == KeyMapStatus::ok && pause.scan.code == 0x45 && pause.scan.e1 && !pause.scan.extended);
    CHECK(numlock.scan.code == 0x45 && !numlock.scan.e1 && !numlock.scan.extended);
    CHECK(scan_to_usage({0x45,false,true}).usage == 0x48);
    CHECK(scan_to_usage({0x45,false,false}).usage == 0x53);
    CHECK(scan_to_usage({0x45,true,false}).usage == 0x53); // legacy NumLock alias
    CHECK(scan_to_usage({0x46,true,false}).status == KeyMapStatus::unknown_scan); // Ctrl+Pause/Break is distinct
    CHECK(scan_to_usage({0x45,true,true}).status == KeyMapStatus::invalid_scan);
    CHECK(scan_to_usage({0,false,false}).status == KeyMapStatus::invalid_scan);
    CHECK(scan_to_usage({0x44,true,false}).status == KeyMapStatus::unknown_scan);
    CHECK(usage_to_scan(0x32).status == KeyMapStatus::unsupported_usage); // alias of 0x31
    CHECK(usage_to_scan(0x66).status == KeyMapStatus::unsupported_usage); // power
    CHECK(usage_to_scan(0x90).status == KeyMapStatus::unsupported_usage); // release-only
    CHECK(usage_to_scan(0xffff).status == KeyMapStatus::unsupported_usage);
    CaptureEvent event{CaptureKind::key, 0x45, true, false};
    event.e1 = true; event.repeat = true; event.virtual_key = VK_PAUSE;
    CHECK(event.e1 && event.repeat && event.virtual_key == VK_PAUSE && !event.extended);
    std::cout << "PASS " << n << " keymap checks over " << mapped << " mapped HID usages; no input services\n";
}
