#include "hop_windows/displays.h"
#include <iostream>
using namespace hop::windows;
// Read-only diagnostic for the process's current Windows session. It does not
// create a window, install hooks, inject input or touch the clipboard.
int main() {
    LocalDisplaySnapshot snapshot;
    if(!enumerate_local_displays(snapshot)) {std::cout<<"NO_DISPLAY_SNAPSHOT\n";return 2;}
    std::cout<<"DISPLAY_SNAPSHOT count="<<snapshot.displays.size()<<" virtual="<<snapshot.desktop.width<<"x"<<snapshot.desktop.height<<'\n';
    return 0;
}
