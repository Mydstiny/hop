#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
namespace hop::windows {
struct BgraImage {
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> pixels; // top-down, straight-alpha BGRA
};
// Strict subset: 24/32-bit BI_RGB and standard 32-bit BI_BITFIELDS V4/V5,
// no palette, embedded/linked ICC profile or nonstandard channel masks.
bool decode_dib(const std::uint8_t* data, std::size_t size, BgraImage& image);
bool encode_dibv5(const BgraImage& image, std::vector<std::uint8_t>& dib);
} // namespace hop::windows
