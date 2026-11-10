#include "hop_windows/clipboard.h"
#include <algorithm>
#include <array>
#include <utility>

namespace hop::windows {
namespace {
bool utf8(const std::vector<std::uint8_t>& text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto c = text[i++];
        if (c == 0) return false; // CF_UNICODETEXT terminates at the first NUL
        if (c < 0x80) continue;
        unsigned extra = 0; std::uint32_t code = 0, min = 0;
        if (c >= 0xc2 && c <= 0xdf) { extra = 1; code = c & 0x1f; min = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { extra = 2; code = c & 0x0f; min = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { extra = 3; code = c & 0x07; min = 0x10000; }
        else return false;
        if (i + extra > text.size()) return false;
        while (extra--) { const auto b = text[i++]; if ((b & 0xc0) != 0x80) return false; code = (code << 6) | (b & 0x3f); }
        if (code < min || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return true;
}
std::uint32_t be32(const std::vector<std::uint8_t>& v, std::size_t offset) {
    return (std::uint32_t(v[offset]) << 24) | (std::uint32_t(v[offset+1]) << 16) |
           (std::uint32_t(v[offset+2]) << 8) | std::uint32_t(v[offset+3]);
}
constexpr std::array<std::uint32_t,256> crc_table() {
    std::array<std::uint32_t,256> result{};
    for (std::uint32_t i=0; i<256; ++i) {
        auto v=i;
        for (int bit=0; bit<8; ++bit) v=(v & 1) ? (v >> 1) ^ 0xedb88320u : v >> 1;
        result[i]=v;
    }
    return result;
}
std::uint32_t crc32(const std::vector<std::uint8_t>& data, std::size_t offset, std::size_t length) {
    static constexpr auto table=crc_table();
    std::uint32_t crc=0xffffffffu;
    for (std::size_t i=0; i<length; ++i) crc=table[(crc ^ data[offset+i]) & 0xff] ^ (crc >> 8);
    return crc ^ 0xffffffffu;
}
bool png_header(const std::vector<std::uint8_t>& data) {
    constexpr std::uint8_t signature[] = {0x89,'P','N','G',0x0d,0x0a,0x1a,0x0a};
    if (data.size() < 33 || data.size() > max_png_bytes || !std::equal(signature, signature + 8, data.begin()) ||
        be32(data, 8) != 13 || data[12] != 'I' || data[13] != 'H' || data[14] != 'D' || data[15] != 'R') return false;
    const auto width = be32(data, 16), height = be32(data, 20);
    if (!width || !height || width > 16384 || height > 16384 || std::uint64_t(width) * height > max_png_pixels) return false;
    const auto depth = data[24], color = data[25];
    const bool valid_depth = (color == 0 && (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16)) ||
        ((color == 2 || color == 4 || color == 6) && (depth == 8 || depth == 16)) ||
        (color == 3 && (depth == 1 || depth == 2 || depth == 4 || depth == 8));
    if (!valid_depth || data[26] || data[27] || data[28] > 1) return false;
    bool idat = false, idat_ended = false;
    std::size_t offset = 8, chunks = 0;
    while (offset < data.size()) {
        if (++chunks > 4096 || data.size() - offset < 12) return false;
        const auto length = be32(data, offset);
        if (length > data.size() - offset - 12) return false;
        const auto* kind = data.data() + offset + 4;
        for (int i=0; i<4; ++i) if (!((kind[i] >= 'A' && kind[i] <= 'Z') || (kind[i] >= 'a' && kind[i] <= 'z'))) return false;
        if (kind[2] & 0x20) return false; // reserved bit must be zero
        if (crc32(data, offset + 4, std::size_t(length) + 4) != be32(data, offset + 8 + length)) return false;
        const bool is_ihdr = kind[0]=='I' && kind[1]=='H' && kind[2]=='D' && kind[3]=='R';
        const bool is_idat = kind[0]=='I' && kind[1]=='D' && kind[2]=='A' && kind[3]=='T';
        const bool is_iend = kind[0]=='I' && kind[1]=='E' && kind[2]=='N' && kind[3]=='D';
        const bool is_plte = kind[0]=='P' && kind[1]=='L' && kind[2]=='T' && kind[3]=='E';
        // Clipboard contract is a static image; APNG animation is not silently flattened.
        if ((kind[0]=='a' && kind[1]=='c' && kind[2]=='T' && kind[3]=='L') ||
            (kind[0]=='f' && kind[1]=='c' && kind[2]=='T' && kind[3]=='L') ||
            (kind[0]=='f' && kind[1]=='d' && kind[2]=='A' && kind[3]=='T')) return false;
        // Avoid inflating unbounded compressed metadata in a platform codec.
        if ((kind[0]=='i' && kind[1]=='C' && kind[2]=='C' && kind[3]=='P') ||
            (kind[0]=='z' && kind[1]=='T' && kind[2]=='X' && kind[3]=='t') ||
            (kind[0]=='i' && kind[1]=='T' && kind[2]=='X' && kind[3]=='t')) return false;
        if ((chunks == 1 && !is_ihdr) || (chunks != 1 && is_ihdr)) return false;
        if (!(kind[0] & 0x20) && !is_ihdr && !is_idat && !is_iend && !is_plte) return false;
        if (is_idat) { if (idat_ended) return false; idat = true; }
        else if (idat) idat_ended = true;
        offset += std::size_t(length) + 12;
        if (is_iend) return idat && !length && offset == data.size();
    }
    return false;
}
}
bool valid_png_bytes(const std::vector<std::uint8_t>& bytes) { return png_header(bytes); }
bool valid_clipboard_payload(const ClipboardPayload& payload) {
    switch (payload.format) {
    case ClipboardFormat::utf8_text: return payload.bytes.size() <= max_text_bytes && utf8(payload.bytes);
    case ClipboardFormat::png: return valid_png_bytes(payload.bytes);
    }
    return false;
}
ClipboardSelection select_clipboard_format(bool file_list, bool png, bool dibv5, bool dib, bool text) {
    if (file_list) return ClipboardSelection::none;
    if (png) return ClipboardSelection::png;
    if (dibv5) return ClipboardSelection::dibv5;
    if (dib) return ClipboardSelection::dib;
    if (text) return ClipboardSelection::text;
    return ClipboardSelection::none;
}
void ClipboardSync::configure(bool enabled, bool trusted, std::string peer, std::uint64_t generation, std::uint32_t baseline_sequence) {
    if (enabled_ != enabled || trusted_ != trusted || peer_ != peer || generation_ != generation ||
        (!baseline_ && baseline_sequence && !faulted_)) {
        echo_ = 0; baseline_ = baseline_sequence; seen_ = baseline_sequence; faulted_ = false;snapshot_taken_=false;
    }
    enabled_ = enabled; trusted_ = trusted; peer_ = std::move(peer); generation_ = generation;
}
bool ClipboardSync::active() const { return enabled_ && trusted_ && !peer_.empty() && generation_ != 0 && baseline_ != 0 && !faulted_; }
std::optional<ClipboardPayload> ClipboardSync::initial_snapshot(std::uint32_t sequence,std::uint64_t generation) {
    if(!active() || generation!=generation_ || !sequence || snapshot_taken_)return std::nullopt;
    snapshot_taken_=true;
    if(sequence==echo_)return std::nullopt;
    ClipboardPayload result{};
    if(!backend_.read(sequence,result) || !valid_clipboard_payload(result))return std::nullopt;
    seen_=sequence;return result;
}
std::optional<ClipboardPayload> ClipboardSync::on_change(std::uint32_t sequence, std::uint64_t generation) {
    if (!active() || generation != generation_ || !sequence || sequence == seen_) return std::nullopt;
    if (sequence == echo_) { seen_ = sequence; return std::nullopt; }
    ClipboardPayload result{};
    if (!backend_.read(sequence, result) || !valid_clipboard_payload(result)) return std::nullopt;
    seen_ = sequence;
    return result;
}
bool ClipboardSync::apply_remote(const std::string& peer, std::uint64_t generation, const ClipboardPayload& payload, std::uint32_t expected_sequence) {
    if (!active() || peer != peer_ || generation != generation_ || !expected_sequence || !valid_clipboard_payload(payload)) return false;
    std::uint32_t sequence = 0;
    const auto result=backend_.write_guarded(payload,expected_sequence,sequence);
    if(result==ClipboardWriteResult::conflict)return true;
    if(result!=ClipboardWriteResult::written || !sequence) { faulted_ = true; return false; }
    echo_ = sequence; seen_ = 0;
    return true;
}
} // namespace hop::windows
