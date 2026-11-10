#pragma once
#include "hop_windows/input_sink.h"
#include <cstdint>

namespace hop::windows {
enum class KeyMapStatus { ok, unsupported_usage, unknown_scan, invalid_scan };
struct ScanResult { KeyMapStatus status; ScanCode scan; };
struct UsageResult { KeyMapStatus status; std::uint16_t usage; };
// Physical USB HID keyboard page (0x07), not characters or virtual keys.
// Pause is represented as {0x45, false, true} for E1 1D 45, but SendInput
// rejects E1 until its injection behavior is independently verified.
ScanResult usage_to_scan(std::uint16_t usage);
UsageResult scan_to_usage(ScanCode scan);
} // namespace hop::windows
