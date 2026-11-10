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

namespace hop::windows {
// Development-only state machine. The owner must call it serially from a
// non-hook worker, only while the pinned TLS transport reports authenticated.
// It never touches system input or the clipboard. Callbacks must not re-enter.
struct RemoteScreen { std::uint64_t id; std::int32_t x,y; std::uint32_t width,height; };
enum class SessionRole { host, receiver };
enum class SessionPhase { initial, hello_wait, pair_wait, pair_cancel_wait, ready, enter_wait, active, release_wait, release_ack_wait, failed };
struct SessionCallbacks {
    std::function<bool(const Frame&)> enter;
    // Apply only the indicated key/button transition; modifier masks are
    // descriptive and must never themselves cause arbitrary key injection.
    std::function<bool(const Frame&)> input;
    // Stop capture, restore local control, release injected input and cancel
    // clipboard work. Must be idempotent and must not block on network I/O.
    std::function<bool()> restore;
};
class Session {
public:
    Session(SessionRole role, std::uint64_t session_epoch,
            std::vector<std::uint64_t> local_screens, SessionCallbacks callbacks);
    // Must be explicitly enabled before start. sample_sequence must not read
    // clipboard contents; apply must promptly copy the value into a bounded
    // background job (it is wiped after the callback). That job must validate
    // data, recheck peer/focus, and atomically compare the supplied baseline
    // under OS clipboard ownership before writing. Do not block this worker.
    bool enable_clipboard(std::function<std::uint32_t()> sample_sequence,
                          std::function<bool(const hop::ClipboardValue&,std::uint32_t)> apply);
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
    // Optional nonblocking receiver barrier: begin revokes clipboard state;
    // ready becomes true only after its dedicated worker has quiesced.
    bool set_release_barrier(std::function<bool(std::uint64_t)> begin,std::function<bool()> ready);
    bool set_key_mode(hop::KeyMode mode);
    bool set_leave_handler(std::function<bool(std::uint64_t,std::uint16_t,std::uint16_t)> handler);
    bool start(bool tls_authenticated, std::uint64_t now_ms);
    bool receive(bool tls_authenticated, const std::uint8_t* bytes, std::size_t count, std::uint64_t now_ms);
    bool take(bool tls_authenticated, std::vector<std::uint8_t>& encoded);
    bool tick(bool tls_authenticated, std::uint64_t now_ms);
    bool advertise_screens(std::vector<std::uint8_t> payload);
    bool enter(std::uint64_t epoch, std::uint64_t screen, std::uint16_t x, std::uint16_t y);
    bool release(std::uint8_t reason);
    bool request_leave(std::uint16_t x, std::uint16_t y);
    bool send_input(std::vector<std::uint8_t> payload);
    // Call only after the local clipboard actor confirms this focus is ready.
    bool request_clipboard_ready();
    bool pause_clipboard();
    bool clipboard_negotiated() const { return (negotiated_caps_&6)!=0; }
    bool clipboard_supports(hop::ClipboardFormat format) const {return format==hop::ClipboardFormat::text ? (negotiated_caps_&2)!=0 : format==hop::ClipboardFormat::png && (negotiated_caps_&4)!=0;}
    bool clipboard_paused() const { return clip_paused_; }
    bool clipboard_queue_empty() const { return queue_.clipboard_bytes()==0; }
    bool clipboard_can_sync() const { return phase_==SessionPhase::active && clip_local_ready_ && clip_peer_ready_ && !clip_paused_; }
    bool clipboard_initial_done() const {return clip_initial_done_;}
    bool clipboard_initial_drained() const {return clip_initial_drained_;}
    bool mark_clipboard_initial_drained();
    std::uint32_t focus_clipboard_baseline() const {return focus_clipboard_baseline_;}
    bool send_clipboard_initial_done();
    bool clipboard_start(std::uint64_t id, hop::ClipboardFormat format, std::uint32_t total);
    bool clipboard_chunk(std::uint64_t id, const std::uint8_t* bytes, std::size_t count);
    bool clipboard_cancel(std::uint64_t id, std::uint8_t reason);
    void disconnect();
    SessionPhase phase() const { return phase_; }
    std::uint64_t focus_epoch() const { return focus_epoch_; }
    // Same serial worker as all Session calls; returns a detached snapshot.
    std::vector<RemoteScreen> peer_displays() const { return peer_displays_; }
private:
    bool deliver(Frame&& frame);
    bool enqueue(FrameKind kind, std::uint64_t epoch, std::vector<std::uint8_t> payload);
    bool fail();
    bool transition_input(const Frame& frame);
    bool finish_clipboard();
    bool queue_key(const hop::KeyEvent& key);
    SessionRole role_;
    SessionPhase phase_ = SessionPhase::initial;
    std::uint64_t session_epoch_, focus_epoch_ = 0, last_focus_ = 0, screen_ = 0, negotiated_caps_ = 0, local_caps_ = 25;
    std::uint64_t last_received_ = 0, last_heartbeat_ = 0, now_ = 0, phase_started_ = 0;
    bool local_pair_confirmed_ = false, peer_pair_confirmed_ = false, pairing_confirmed_ = false;
    std::vector<std::uint64_t> local_screens_, peer_screens_;
    std::vector<RemoteScreen> peer_displays_;
    SessionCallbacks callbacks_;
    FrameDecoder decoder_;
    OutboundQueue queue_;
    std::array<bool, 256> keys_{};
    std::string peer_fingerprint_;
    hop::Platform peer_platform_ = hop::Platform::windows;
    hop::KeyMode key_mode_ = hop::KeyMode::common_shortcuts;
    std::optional<hop::ShortcutTranslator> translator_;
    std::uint8_t buttons_ = 0;
    bool clip_local_ready_=false,clip_peer_ready_=false,clip_paused_=false;
    hop::ClipboardAssembler clipboard_;
    std::function<bool(std::uint64_t,std::uint16_t,std::uint16_t)> leave_handler_;
    std::function<bool(std::uint64_t)> release_barrier_begin_;std::function<bool()> release_barrier_ready_;
    std::function<std::uint32_t()> clipboard_sequence_;
    std::function<bool(const hop::ClipboardValue&,std::uint32_t)> clipboard_apply_;
    std::uint64_t incoming_clip_id_ = 0, outgoing_clip_id_ = 0, outgoing_last_id_ = 0;
    std::uint32_t clipboard_baseline_ = 0, focus_clipboard_baseline_ = 0, outgoing_total_ = 0, outgoing_offset_ = 0;
    bool clip_initial_done_ = false,clip_initial_transfer_seen_ = false,clip_initial_drained_ = false;
};
} // namespace hop::windows
