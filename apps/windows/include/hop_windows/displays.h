#pragma once
#include "hop_windows/receiver_session_adapter.h"
#include <vector>
namespace hop::windows {
struct LocalDisplaySnapshot { std::vector<LocalDisplay> displays; PixelRect desktop{}; };
// Enumerates the caller's active desktop in a temporary per-monitor-V2 DPI
// context. Requires Windows support for that context; returns false rather
// than mixing scaled and physical coordinates. IDs are unique only within the
// returned snapshot/connection. Never start an input hook here.
bool enumerate_local_displays(LocalDisplaySnapshot& output);
}
