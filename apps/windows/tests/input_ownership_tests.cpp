#include "hop_windows/input_ownership.h"
#include <cstdlib>
#include <iostream>
using namespace hop::windows;
int n = 0;
void check(bool b, const char* s, int line) { ++n; if (!b) { std::cerr << line << ": " << s << '\n'; std::exit(1); } }
#define CHECK(x) check(bool(x), #x, __LINE__)
int main() {
    InputOwnership o;
    o.seed_local_key(7); o.seed_local_button(1);
    CHECK(!o.activate());
    auto a=o.key(7,false); CHECK(!a.suppress_local && !a.forward_remote);
    CHECK(!o.activate());
    a=o.button(1,false); CHECK(!a.suppress_local && !a.forward_remote);
    CHECK(o.activate() && o.active());
    a=o.key(7,true); CHECK(a.suppress_local && a.forward_remote && !a.repeat);
    a=o.key(7,true); CHECK(a.suppress_local && a.forward_remote && a.repeat);
    a=o.button(0,true); CHECK(a.suppress_local && a.forward_remote);
    a=o.motion(0,true); CHECK(a.suppress_local && a.forward_remote);
    o.deactivate(); CHECK(!o.active() && !o.drained());
    CHECK(!o.activate());
    a=o.key(7,true); CHECK(a.suppress_local && !a.forward_remote);
    a=o.motion(0,true); CHECK(a.suppress_local && !a.forward_remote);
    a=o.motion(0,false); CHECK(a.suppress_local && !a.forward_remote);
    a=o.key(8,true); CHECK(!a.suppress_local && !a.forward_remote);
    a=o.key(7,false); CHECK(a.suppress_local && !a.forward_remote && !a.repeat);
    a=o.button(0,false); CHECK(a.suppress_local && !a.forward_remote);
    CHECK(o.drained() && !o.activate());
    a=o.key(8,false); CHECK(!a.suppress_local && !a.forward_remote);
    CHECK(o.activate());
    a=o.key(999,false); CHECK(!a.suppress_local && !a.forward_remote);
    a=o.key(999,true); CHECK(a.suppress_local && a.forward_remote);
    a=o.key(999,false); CHECK(a.suppress_local && a.forward_remote);
    a=o.button(5,true); CHECK(a.suppress_local && a.forward_remote);
    a=o.button(5,false); CHECK(a.suppress_local && a.forward_remote);
    CHECK(o.drained());
    a=o.button(1,true); CHECK(a.suppress_local && a.forward_remote);
    o.deactivate(); CHECK(!o.drained());
    o.abandon_remote(); CHECK(o.drained() && !o.active());
    a=o.motion(0,false); CHECK(!a.suppress_local && !a.forward_remote);
    a=o.button(1,false); CHECK(!a.suppress_local && !a.forward_remote);
    std::cout << "PASS " << n << " Windows input ownership checks; no hooks or input queries\n";
}
