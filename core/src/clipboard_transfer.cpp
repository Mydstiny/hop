#include "hop/clipboard_transfer.h"
#include "hop/frame.h"
#include <utility>
namespace hop {
void ClipboardAssembler::cancel() {
    volatile std::uint8_t* p = value_.data.data();
    for (std::size_t i = 0; i < value_.data.size(); ++i) p[i] = 0;
    std::vector<std::uint8_t>().swap(value_.data);
    active_ = false; completed_ = false; expected_ = 0;
}
ClipboardAssembler::~ClipboardAssembler() { cancel(); }
void ClipboardAssembler::focus(std::uint64_t epoch) {
    if (epoch_ != epoch) { cancel(); epoch_ = epoch; }
}
bool ClipboardAssembler::begin(std::uint64_t epoch, std::uint64_t id, ClipboardFormat format, std::size_t bytes) {
    if (!epoch_ || epoch != epoch_ || !id || id <= last_id_ || active_ || completed_) return false;
    if ((format == ClipboardFormat::text && bytes > max_text) ||
        (format == ClipboardFormat::png && (bytes == 0 || bytes > max_png)) ||
        (format != ClipboardFormat::text && format != ClipboardFormat::png)) return false;
    try {
        value_.format = format; value_.transfer_id = id; value_.epoch = epoch;
        expected_ = bytes;
        value_.data.reserve(bytes);
        active_ = bytes != 0; completed_ = bytes == 0; last_id_ = id;
        return true;
    } catch (...) { cancel(); return false; }
}
bool ClipboardAssembler::chunk(std::uint64_t epoch, std::uint64_t id, std::size_t offset, const std::uint8_t* bytes, std::size_t count) {
    // 16 bytes of each framed chunk are reserved for schema, transfer ID and offset.
    if (!active_ || epoch != epoch_ || epoch != value_.epoch || id != value_.transfer_id) return false;
    if (!bytes || !count || count > max_clipboard_chunk - 16 || offset != value_.data.size() ||
        count > expected_ - value_.data.size()) { cancel(); return false; }
    try {
        value_.data.insert(value_.data.end(),bytes,bytes+count);
        if (value_.data.size() == expected_) { active_ = false; completed_ = true; }
        return true;
    } catch (...) { cancel(); return false; }
}
std::optional<ClipboardValue> ClipboardAssembler::take() {
    if (!completed_ || !epoch_ || value_.epoch != epoch_) return std::nullopt;
    completed_ = false; expected_ = 0;
    ClipboardValue out = std::move(value_); value_ = ClipboardValue{};
    return out;
}
} // namespace hop
