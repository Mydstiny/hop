#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
namespace hop::windows {
// In-memory WIC conversion; never accesses clipboard, files, or network.
// PNG input/output follows ClipboardPayload byte/pixel limits. Unsupported or
// color-managed DIB subformats fail closed rather than being misinterpreted.
bool png_to_dibv5(const std::vector<std::uint8_t>& png, std::vector<std::uint8_t>& dib);
bool dib_to_png(const std::uint8_t* dib, std::size_t size, std::vector<std::uint8_t>& png);
} // namespace hop::windows
