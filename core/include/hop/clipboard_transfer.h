#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>
namespace hop {
enum class ClipboardFormat : std::uint8_t { text = 1, png = 2 };
struct ClipboardValue {
    ClipboardFormat format;
    std::uint64_t transfer_id, epoch;
    std::vector<std::uint8_t> data;
};
// Only for plaintext from an authenticated, authorized TLS session. The
// application must validate content, perform actual pasteboard access and
// check local conflicts before applying a completed value.
class ClipboardAssembler {
public:
    static constexpr std::size_t max_text = 2 * 1024 * 1024;
    static constexpr std::size_t max_png = 16 * 1024 * 1024;
    // 0 disables reception. Changing focus cancels partial/completed values.
    void focus(std::uint64_t epoch);
    // Strictly increasing IDs per connection; no duplicate/replayed transfers.
    bool begin(std::uint64_t epoch, std::uint64_t id, ClipboardFormat format, std::size_t bytes);
    bool chunk(std::uint64_t epoch, std::uint64_t id, std::size_t offset, const std::uint8_t* bytes, std::size_t count);
    // Borrowed pointer remains valid until any mutating method is called.
    const ClipboardValue* peek() const { return completed_ ? &value_ : nullptr; }
    std::optional<ClipboardValue> take();
    void cancel();
    ~ClipboardAssembler();
    ClipboardAssembler() = default;
    ClipboardAssembler(const ClipboardAssembler&) = delete;
    ClipboardAssembler& operator=(const ClipboardAssembler&) = delete;
private:
    std::uint64_t epoch_ = 0, last_id_ = 0;
    std::size_t expected_ = 0;
    bool active_ = false, completed_ = false;
    ClipboardValue value_{};
};
} // namespace hop
