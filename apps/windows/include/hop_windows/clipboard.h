#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hop::windows {
enum class ClipboardFormat { utf8_text, png };
struct ClipboardPayload {
    ClipboardFormat format;
    std::vector<std::uint8_t> bytes;
};
constexpr std::size_t max_text_bytes = 2u * 1024 * 1024;
constexpr std::size_t max_png_bytes = 16u * 1024 * 1024;
constexpr std::uint64_t max_png_pixels = 40000000;

// Checks UTF-8 or PNG structure, chunk CRCs and bounds. It does not decompress
// image pixels; decoder validation/normalization remains a separate step.
bool valid_clipboard_payload(const ClipboardPayload& payload);
bool valid_png_bytes(const std::vector<std::uint8_t>& bytes);
enum class ClipboardSelection { none, text, png, dibv5, dib };
ClipboardSelection select_clipboard_format(bool file_list, bool png, bool dibv5, bool dib, bool text);

enum class ClipboardWriteResult { written, conflict, error };
class ClipboardBackend {
public:
    virtual ~ClipboardBackend() = default;
    // expected_sequence must still match after the clipboard is opened.
    virtual bool read(std::uint32_t expected_sequence, ClipboardPayload& out) = 0;
    // Capture the written sequence while ownership is still held. If the
    // platform cannot provide a reliable sequence, report false/zero.
    // Compare expected_sequence after opening and before changing the clipboard.
    virtual bool write(const ClipboardPayload& payload, std::uint32_t expected_sequence, std::uint32_t& written_sequence) = 0;
    virtual ClipboardWriteResult write_guarded(const ClipboardPayload& payload,std::uint32_t expected_sequence,std::uint32_t& written_sequence) {
        return write(payload,expected_sequence,written_sequence) ? ClipboardWriteResult::written : ClipboardWriteResult::error;
    }
};

class ClipboardSync {
public:
    explicit ClipboardSync(ClipboardBackend& backend) : backend_(backend) {}
    // Single-threaded; set only after authenticated encrypted session/focus verification by the
    // owner; generation identifies that exact focus, not merely a device name.
    // baseline_sequence is the current clipboard sequence sampled at target
    // activation without reading contents; zero leaves synchronization inactive.
    void configure(bool enabled, bool trusted, std::string peer, std::uint64_t generation, std::uint32_t baseline_sequence);
    bool active() const;
    bool faulted() const { return faulted_; }
    // One output at most per observed sequence. No backend call while inactive,
    // on an echo, on a duplicate, or for invalid/unknown sequence zero.
    std::optional<ClipboardPayload> on_change(std::uint32_t sequence, std::uint64_t generation);
    // One explicitly requested snapshot per focus; call only after both peers
    // are ready and the protocol has selected this side as the initial sender.
    std::optional<ClipboardPayload> initial_snapshot(std::uint32_t sequence,std::uint64_t generation);
    // An unsuccessful platform write pauses synchronization until explicit
    // disable/re-enable or a different target generation is configured.
    // expected_sequence must be sampled when this remote transfer begins.
    bool apply_remote(const std::string& peer, std::uint64_t generation, const ClipboardPayload& payload, std::uint32_t expected_sequence);
private:
    ClipboardBackend& backend_;
    bool enabled_ = false, trusted_ = false, faulted_ = false, snapshot_taken_ = false;
    std::string peer_;
    std::uint64_t generation_ = 0;
    std::uint32_t echo_ = 0, seen_ = 0, baseline_ = 0;
};
} // namespace hop::windows
