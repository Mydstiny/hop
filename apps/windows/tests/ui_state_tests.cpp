#include "hop_windows/ui_state.h"
#include <cstdlib>
#include <iostream>
using namespace hop::windows;
int n=0;
void check(bool b, const char* text, int line) { ++n; if(!b) { std::cerr<<line<<": "<<text<<'\n';std::exit(1); } }
#define CHECK(x) check(bool(x),#x,__LINE__)
int main() {
    UiState s;
    CHECK(!s.connected() && !s.sharing());
    CHECK(!s.can_enable() && !s.can_pause());
    CHECK(!s.enable() && !s.pause());
    CHECK(!s.sharing());
    s.connected(true);
    CHECK(s.can_enable() && !s.can_pause());
    CHECK(s.enable() && s.sharing());
    CHECK(!s.enable() && s.can_pause());
    s.connected(false);
    CHECK(!s.sharing() && !s.can_enable() && !s.can_pause());
    s.connected(true);
    CHECK(!s.sharing() && s.enable() && s.pause());
    CHECK(!s.sharing() && !s.pause());
    const auto& en=ui_strings(UiLanguage::english);
    const auto& zh=ui_strings(UiLanguage::chinese);
    CHECK(en.disconnected && *en.disconnected && zh.disconnected && *zh.disconnected);
    CHECK(en.unavailable && *en.unavailable && zh.unavailable && *zh.unavailable);
    CHECK(en.layout && *en.layout && zh.layout && *zh.layout && en.layout_empty && zh.layout_empty);
    std::cout << "PASS " << n << " UI state checks; no windows, tray icons or input capture\n";
}
