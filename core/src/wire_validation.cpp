#include "hop/wire_validation.h"
#include <cstdint>
#include <limits>
namespace hop {
namespace {
std::uint16_t u16(const std::uint8_t* p) { return (std::uint16_t(p[0]) << 8) | p[1]; }
std::uint32_t u32(const std::uint8_t* p) { return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3]; }
std::uint64_t u64(const std::uint8_t* p) { return (std::uint64_t(u32(p)) << 32) | u32(p+4); }
std::int64_t i32(const std::uint8_t* p) { const auto n=u32(p); return n <= 0x7fffffffU ? n : std::int64_t(n) - 0x100000000LL; }
bool zero(const std::uint8_t* p, std::size_t n) { for (std::size_t i=0;i<n;++i) if (p[i]) return false; return true; }
bool keymask(std::uint16_t mask) { return (mask & ~std::uint16_t(0x1ff)) == 0; }
bool screen_geometry(const std::uint8_t* p) {
    const auto x=i32(p+8), y=i32(p+12); const auto w=u32(p+16), h=u32(p+20);
    return u64(p) != 0 && w >= 1 && w <= 65535 && h >= 1 && h <= 65535 &&
        x + w <= std::numeric_limits<std::int32_t>::max() && y + h <= std::numeric_limits<std::int32_t>::max();
}
bool control(const std::uint8_t* p, std::size_t n) {
    switch (p[1]) {
    case 0x01: case 0x02:
        return n == 16 && p[4] == (p[1] == 1 ? 1 : 2) && p[5] >= 1 && p[5] <= 4 && zero(p+6,2) && (u64(p+8) & ~std::uint64_t(63)) == 0;
    case 0x03: {
        if (n < 8 || !zero(p+6,2)) return false;
        const auto count=u16(p+4); if (count > 64 || n != 8 + std::size_t(count)*24) return false;
        for (std::size_t i=0;i<count;++i) {
            const auto* screen=p+8+i*24;
            if (!screen_geometry(screen)) return false;
            for (std::size_t j=0;j<i;++j) if (u64(screen)==u64(p+8+j*24)) return false;
        }
        return true;
    }
    case 0x04: case 0x05: return n == 4;
    case 0x10: return n == 20 && u64(p+4) && keymask(u16(p+16)) && !(p[18]&~31) && !p[19];
    case 0x11: return n == 12 && u64(p+4);
    case 0x12: return n == 8 && p[4] <= 2 && zero(p+5,3);
    case 0x13: case 0x15: case 0x16: return n == 4;
    case 0x14: return n == 16 && u64(p+4);
    case 0x1e: return n == 12;
    case 0x1f: return n == 8 && p[4] <= 2 && zero(p+5,3);
    default: return false;
    }
}
bool input(const std::uint8_t* p, std::size_t n) {
    switch (p[1]) {
    case 0x01: return n == 16 && u64(p+4);
    case 0x02:
        return n == 20 && u64(p+4) && p[16] >= 1 && p[16] <= 5 && p[17] <= 1 && !(p[18]&~31) && !p[19] &&
            bool(p[18] & (1u << (p[16]-1))) == bool(p[17]);
    case 0x03: return n == 12 && i32(p+4) >= -120000 && i32(p+4) <= 120000 && i32(p+8) >= -120000 && i32(p+8) <= 120000;
    case 0x04: return n == 12 && u16(p+4) >= 1 && u16(p+4) <= 0xe7 && keymask(u16(p+6)) && p[8] <= 1 && p[9] <= 1 && (p[8] || !p[9]) && zero(p+10,2);
    default: return false;
    }
}
bool clipboard_control(const std::uint8_t* p, std::size_t n) {
    switch (p[1]) {
    case 0x01: return n == 20 && u64(p+4) && zero(p+13,3) &&
        ((p[12] == 1 && u32(p+16) <= 2u*1024*1024) || (p[12] == 2 && u32(p+16) >= 1 && u32(p+16) <= 16u*1024*1024));
    case 0x02: return n == 16 && u64(p+4) && zero(p+13,3);
    case 0x03: return n == 4;
    default: return false;
    }
}
}
bool valid_wire_payload(const Frame& frame) {
    const auto& body=frame.payload; const auto n=body.size();
    if (!frame.sequence || !frame.epoch || n<4 || body[0]!=1 || body[2] || body[3]) return false;
    const auto* p=body.data();
    switch (frame.kind) {
    case FrameKind::control: return n <= max_control_payload && control(p,n);
    case FrameKind::input: return n <= max_control_payload && input(p,n);
    case FrameKind::clipboard_control: return n <= max_control_payload && clipboard_control(p,n);
    case FrameKind::clipboard_chunk: return n >= 17 && n <= max_clipboard_chunk && p[1] == 1 && u64(p+4) &&
        u32(p+12) <= 16u*1024*1024 && n - 16 <= 16u*1024*1024 - u32(p+12);
    }
    return false;
}
}
