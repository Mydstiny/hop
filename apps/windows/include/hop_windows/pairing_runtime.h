#pragma once
#include "hop_windows/pairing_worker.h"
#include "hop_windows/host_capture_pump.h"
#include "hop_windows/clipboard_worker.h"
#include "hop_windows/edge_detector.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
namespace hop::windows {
enum class PairingRuntimeEvent { listener_ready,reviewing,confirming,connected,screens_changed,sharing_started,sharing_stopped,edge_monitor_started,action_failed,failed,stopped,peer_action_failed,peers_changed,target_changed };
struct PointerProfile {hop::PointerSettings settings;std::uint16_t motion_rate_hz=240;};
struct PairingRuntimeConfig {
    bool listen=false;TcpAddress address{};SessionRole role=SessionRole::host;std::string name;
    std::vector<std::uint64_t> screens;std::vector<std::uint8_t> screen_payload;
    std::function<SessionCallbacks()> callbacks;
    CaptureSource* host_capture=nullptr;hop::PointerSettings pointer_settings{};std::uint16_t motion_rate_hz=240;
    std::function<std::optional<PointerProfile>(const std::string&)> pointer_profile;
    // Desktop entry points verify native geometry again immediately before
    // acting on an asynchronous edge request. Fake capture sources may omit it.
    std::function<bool(const std::vector<LiveScreen>&)> verify_local_geometry;
    // Both are supplied only after the user explicitly enables clipboard.
    std::shared_ptr<ClipboardBackend> clipboard_backend;
    std::function<std::uint32_t()> clipboard_sequence;
};
struct PeerDisplaySnapshot {std::string peer;std::vector<RemoteScreen> displays;};
struct ConnectedPeer {std::string fingerprint,name;std::vector<RemoteScreen> displays;};
// Explicitly started background owner. The event callback must only post a UI
// message and return, never block or touch Session. CredentialBackend and any
// objects captured by callbacks outlive the runtime. stop() joins the worker and
// must be called by a control worker rather than the UI or hook thread.
class PairingRuntime {
public:
    PairingRuntime(CredentialBackend& backend,std::function<void(PairingRuntimeEvent)> event) : backend_(backend),event_(std::move(event)) {}
    ~PairingRuntime();
    bool start(PairingRuntimeConfig config);
    void request_stop(); // nonblocking; safe from UI/control thread
    void stop();
    std::optional<PairingReview> review() const;
    std::string peer_fingerprint() const;
    std::vector<ConnectedPeer> connected_peers() const;
    // Host-only. Pairing or changing targets is accepted only when local
    // input, the previous RELEASE_ACK and clipboard revocation are settled.
    bool request_add_peer(bool listen,const TcpAddress& address);
    bool request_select_peer(std::string fingerprint);
    bool listen_address(TcpAddress& out) const;
    bool confirm_tls(bool confirmed);
    std::vector<RemoteScreen> peer_displays() const;
    PeerDisplaySnapshot selected_displays() const;
    bool request_control(const std::string& peer,std::uint64_t screen_id);
    // Explicit opt-in. Layout/IDs describe this connection only and are
    // invalidated if the authenticated peer advertises different screens.
    bool request_edge_monitor(const std::string& peer,std::vector<LiveScreen> local,
                              std::vector<LiveScreen> expected_peer,std::vector<LivePlacement> placements);
    bool request_workspace_edges(const std::string& selected,std::vector<LiveScreen> local,
                                 std::vector<LivePeerScreens> peers,std::vector<LiveScreenPlacement> placements);
    bool request_pause();
    bool update_pointer_settings(const std::string& peer_fingerprint,hop::PointerSettings settings,std::uint16_t motion_rate_hz);
    PairingState state() const {return state_.load();}
    bool finished() const {return finished_.load();}
private:
    void run(PairingRuntimeConfig config);
    void notify(PairingRuntimeEvent event) noexcept;
    struct PointerUpdate {std::string peer;hop::PointerSettings settings;std::uint16_t rate;};
    struct EdgeRequest {std::vector<LiveScreen> local;std::vector<LivePeerScreens> peers;std::vector<LiveScreenPlacement> placements;};
    struct AddPeer {bool listen;TcpAddress address;};
    CredentialBackend& backend_;std::function<void(PairingRuntimeEvent)> event_;
    mutable std::mutex mutex_;std::condition_variable wake_;std::thread thread_;
    std::optional<PairingReview> review_;std::string peer_fingerprint_;TcpAddress listen_address_{};std::vector<RemoteScreen> peer_displays_;std::uint64_t control_screen_=0;std::optional<PointerUpdate> pending_pointer_;
    std::optional<EdgeRequest> pending_edge_;
    std::vector<ConnectedPeer> connected_peers_;
    std::optional<AddPeer> pending_add_peer_;
    std::optional<std::string> pending_select_peer_;
    bool retargeting_=false;
    int control_command_=0; // protected by mutex_ along with command arguments
    std::atomic<bool> stop_{false},finished_{true},host_mode_{false};std::atomic<int> confirmation_{0};std::atomic<PairingState> state_{PairingState::idle};
};
}
