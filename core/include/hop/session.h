#pragma once
#include "hop/frame.h"
#include "hop/clipboard_transfer.h"
#include "hop/outbound_queue.h"
#include "hop/shortcut_translator.h"
#include <optional>
#include <string>
#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace hop::v2 {
// Development-only state machine. The owner must call it serially from a
// non-hook worker, only while the pinned TLS transport reports authenticated.
// It never touches system input or the clipboard. Callbacks must not re-enter.
enum class SessionRole { host, receiver };
enum class SessionPhase { initial, hello_wait, pair_wait, pair_cancel_wait, ready, enter_wait, active, release_wait, failed, release_ack_wait };
struct PeerScreen {
    std::uint64_t id;
    std::int32_t x,y;
    std::uint32_t width,height;
};
struct SessionCallbacks {
    std::function<bool(const Frame&)> enter;
    // Apply only the indicated key/button transition; modifier masks are
    // descriptive and must never themselves cause arbitrary key injection.
    std::function<bool(const Frame&)> input;
    // Stop capture, restore local control, release injected input and cancel
    // clipboard work. Must be idempotent and must not block on network I/O.
    std::function<bool()> restore;
    // Must enqueue an immediate nonblocking actor revocation; safe to call repeatedly.
    std::function<void()> clipboard_revoke;
};
class Session {
public:
    Session(SessionRole role, std::uint64_t session_epoch,
            std::vector<std::uint64_t> local_screens, SessionCallbacks callbacks,
            hop::Platform platform = hop::Platform::windows, std::uint64_t capabilities = 25);
    // Must be explicitly enabled before start. sample_sequence must not read
    // clipboard contents; apply must promptly copy the value into a bounded
    // background job (it is wiped after the callback). That job must validate
    // data, recheck peer/focus, and compare the supplied baseline before
    // writing. A later local-change conflict is a normal skip, not a session
    // error; report false here only when the bounded job cannot be accepted.
    // OS clipboard comparison/write atomicity depends on the platform API.
    bool enable_clipboard(std::function<std::optional<std::uint64_t>()> sample_sequence,
                          std::function<bool(const hop::ClipboardValue&,std::uint64_t)> apply,
                          bool allow_text = true, bool allow_png = true);
    // Handler only schedules a layout decision; it must not re-enter Session.
    // Set only for a connection whose certificate fingerprints were already
    // compared through an independent trusted channel. Both users must still
    // explicitly confirm within this TLS session before trust is persisted.
    // Bind first-pair confirmation to the exact previously compared TLS pin.
    bool bind_verified_peer(std::string fingerprint);
    const std::string& peer_fingerprint() const { return peer_fingerprint_; }
    bool require_pairing_confirmation();
    bool confirm_pairing();
    bool cancel_pairing();
    bool pairing_confirmed() const { return pairing_confirmed_; }
    bool set_key_mode(hop::KeyMode mode);
    bool set_leave_handler(std::function<bool(std::uint64_t,std::uint16_t,std::uint16_t)> handler);
    bool start(bool tls_authenticated, std::uint64_t now_ms);
    bool receive(bool tls_authenticated, const std::uint8_t* bytes, std::size_t count, std::uint64_t now_ms);
    bool take(bool tls_authenticated, std::vector<std::uint8_t>& encoded);
    bool tick(bool tls_authenticated, std::uint64_t now_ms);
    bool advertise_screens(std::vector<std::uint8_t> payload);
    bool enter(std::uint64_t epoch, std::uint64_t screen, std::uint16_t x, std::uint16_t y);
    bool release(std::uint8_t reason);
    // Called by the session worker only after the local clipboard actor has
    // completed its revocation barrier. Never block the input/network worker.
    // A receiver queues RELEASE_ACK; a host waits for both barriers and ACK.
    bool complete_release(std::uint64_t monotonic_ms);
    bool request_leave(std::uint16_t x, std::uint16_t y);
    bool send_input(std::vector<std::uint8_t> payload);
    bool clipboard_ready();
    bool clipboard_pause();
    std::uint64_t clipboard_formats() const { return negotiated_caps_ & 6; }
    bool clipboard_has_room(std::size_t bytes) const { return clipboard_available() && queue_.clipboard_has_room(bytes); }
    bool clipboard_available() const { return phase_==SessionPhase::active && clip_local_ready_ && clip_remote_ready_ && !clip_paused_; }
    // Host appends this to the clipboard FIFO after its initial snapshot (or
    // an explicit skip). Receiver must drain its actor before polling locally.
    bool clipboard_initial_done();
    bool clipboard_initial_received() const { return role_==SessionRole::receiver && clipboard_available() && clip_initial_done_; }
    // The receiver calls this only after prior initial-snapshot actor work has drained.
    bool clipboard_initial_drained();
    bool clipboard_start(std::uint64_t id, hop::ClipboardFormat format, std::uint32_t total);
    bool clipboard_chunk(std::uint64_t id, const std::uint8_t* bytes, std::size_t count);
    bool clipboard_cancel(std::uint64_t id, std::uint8_t reason);
    void disconnect();
    SessionPhase phase() const { return phase_; }
    std::uint64_t focus_epoch() const { return focus_epoch_; }
    // Copy only from this session's serialized owner thread.
    std::vector<PeerScreen> peer_screen_snapshot() const { return peer_geometry_; }
private:
    bool deliver(Frame&& frame);
    bool enqueue(FrameKind kind, std::uint64_t epoch, std::vector<std::uint8_t> payload);
    bool fail();
    bool transition_input(const Frame& frame);
    bool finish_clipboard();
    void revoke_clipboard();
    void begin_clipboard_focus(std::optional<std::uint64_t> initial_baseline = std::nullopt);
    bool queue_key(const hop::KeyEvent& key);
    SessionRole role_;
    hop::Platform platform_;
    SessionPhase phase_ = SessionPhase::initial;
    std::uint64_t session_epoch_, focus_epoch_ = 0, last_focus_ = 0, screen_ = 0, negotiated_caps_ = 0, local_caps_ = 25;
    std::uint64_t last_received_ = 0, last_heartbeat_ = 0, now_ = 0, phase_started_ = 0;
    bool local_pair_confirmed_ = false, peer_pair_confirmed_ = false, pairing_confirmed_ = false;
    bool clip_local_ready_ = false, clip_remote_ready_ = false, clip_paused_ = false;
    bool clip_initial_done_ = false, clip_initial_started_ = false, clip_initial_drained_ = false;
    bool release_local_drained_ = false, release_remote_acked_ = false;
    std::vector<std::uint64_t> local_screens_, peer_screens_;
    std::vector<PeerScreen> peer_geometry_;
    SessionCallbacks callbacks_;
    FrameDecoder decoder_;
    OutboundQueue queue_;
    std::array<bool, 256> keys_{};
    std::string peer_fingerprint_;
    hop::Platform peer_platform_ = hop::Platform::windows;
    hop::KeyMode key_mode_ = hop::KeyMode::common_shortcuts;
    std::optional<hop::ShortcutTranslator> translator_;
    std::uint8_t buttons_ = 0;
    hop::ClipboardAssembler clipboard_;
    std::function<bool(std::uint64_t,std::uint16_t,std::uint16_t)> leave_handler_;
    std::function<std::optional<std::uint64_t>()> clipboard_sequence_;
    std::function<bool(const hop::ClipboardValue&,std::uint64_t)> clipboard_apply_;
    std::uint64_t incoming_clip_id_ = 0, outgoing_clip_id_ = 0, outgoing_last_id_ = 0;
    std::optional<std::uint64_t> clipboard_baseline_;
    std::optional<std::uint64_t> initial_clipboard_baseline_;
    std::uint32_t outgoing_total_ = 0, outgoing_offset_ = 0;
};
} // namespace hop::v2
