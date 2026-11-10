#include "hop_windows/pairing_runtime.h"
#include <windows.h>
#include <chrono>
#include <algorithm>
#include <cstring>
namespace hop::windows {
namespace {
struct ClipboardSend {
    ClipboardPayload payload;std::uint64_t epoch=0,id=0;std::size_t offset=0;bool started=false;
    ~ClipboardSend(){volatile std::uint8_t* p=payload.bytes.data();for(std::size_t i=0;i<payload.bytes.size();++i)p[i]=0;}
};
}
PairingRuntime::~PairingRuntime(){stop();}
void PairingRuntime::notify(PairingRuntimeEvent event) noexcept {try{if(event_)event_(event);}catch(...){}}
bool PairingRuntime::start(PairingRuntimeConfig config) {
    if(thread_.joinable() || !config.callbacks || config.name.empty())return false;
    stop_.store(false);finished_.store(false);confirmation_.store(0);state_.store(PairingState::idle);
    host_mode_.store(config.role==SessionRole::host);
    {std::lock_guard<std::mutex> lock(mutex_);review_.reset();peer_fingerprint_.clear();connected_peers_.clear();listen_address_={};peer_displays_.clear();control_command_=0;control_screen_=0;pending_pointer_.reset();pending_edge_.reset();pending_add_peer_.reset();pending_select_peer_.reset();retargeting_=false;}
    try{thread_=std::thread(&PairingRuntime::run,this,std::move(config));return true;}catch(...){finished_.store(true);return false;}
}
void PairingRuntime::request_stop() {stop_.store(true);wake_.notify_all();}
void PairingRuntime::stop() {request_stop();if(thread_.joinable())thread_.join();}
std::optional<PairingReview> PairingRuntime::review() const {std::lock_guard<std::mutex> lock(mutex_);return review_;}
std::string PairingRuntime::peer_fingerprint() const {std::lock_guard<std::mutex> lock(mutex_);return peer_fingerprint_;}
std::vector<ConnectedPeer> PairingRuntime::connected_peers() const {std::lock_guard<std::mutex> lock(mutex_);return connected_peers_;}
bool PairingRuntime::request_add_peer(bool listen,const TcpAddress& address) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!host_mode_.load() || stop_.load() || state_.load()!=PairingState::connected || !address.size ||
       control_command_ || retargeting_)return false;
    pending_add_peer_=AddPeer{listen,address};
    retargeting_=true;
    wake_.notify_all();return true;
}
bool PairingRuntime::request_select_peer(std::string fingerprint) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!host_mode_.load() || stop_.load() || state_.load()!=PairingState::connected ||
       control_command_ || retargeting_ ||
       std::none_of(connected_peers_.begin(),connected_peers_.end(),[&](const auto& peer){return peer.fingerprint==fingerprint;}))return false;
    pending_select_peer_=std::move(fingerprint);
    retargeting_=true;
    wake_.notify_all();return true;
}
bool PairingRuntime::listen_address(TcpAddress& out) const {std::lock_guard<std::mutex> lock(mutex_);out=listen_address_;return out.size!=0;}
bool PairingRuntime::confirm_tls(bool confirmed) {
    if(state_.load()!=PairingState::confirming)return false;int expected=0;
    if(!confirmation_.compare_exchange_strong(expected,confirmed?1:-1))return false;wake_.notify_all();return true;
}
std::vector<RemoteScreen> PairingRuntime::peer_displays() const {std::lock_guard<std::mutex> lock(mutex_);return peer_displays_;}
PeerDisplaySnapshot PairingRuntime::selected_displays() const {std::lock_guard<std::mutex> lock(mutex_);return {peer_fingerprint_,peer_displays_};}
bool PairingRuntime::request_control(const std::string& peer,std::uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(state_.load()!=PairingState::connected || peer.empty() || peer!=peer_fingerprint_ || !id ||
       control_command_ || retargeting_ || stop_.load())return false;
    control_screen_=id;control_command_=1;wake_.notify_all();return true;
}
bool PairingRuntime::request_edge_monitor(const std::string& fingerprint,std::vector<LiveScreen> local,
                                           std::vector<LiveScreen> peer,std::vector<LivePlacement> placements) {
    std::vector<LiveScreenPlacement> named;
    for(const auto& placement:placements)
        named.push_back({placement.peer?fingerprint:std::string{},placement.screen,placement.x,placement.y});
    return request_workspace_edges(fingerprint,std::move(local),{{fingerprint,std::move(peer)}},std::move(named));
}
bool PairingRuntime::request_workspace_edges(const std::string& fingerprint,std::vector<LiveScreen> local,
                                             std::vector<LivePeerScreens> peers,std::vector<LiveScreenPlacement> placements) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(state_.load()!=PairingState::connected || fingerprint.empty() || fingerprint!=peer_fingerprint_ ||
       local.empty() || peers.empty() || placements.empty() || control_command_ || retargeting_ || stop_.load())return false;
    pending_edge_=EdgeRequest{std::move(local),std::move(peers),std::move(placements)};control_command_=3;wake_.notify_all();return true;
}
bool PairingRuntime::request_pause() {
    std::lock_guard<std::mutex> lock(mutex_);if(state_.load()!=PairingState::connected || control_command_ || stop_.load())return false;
    control_command_=2;wake_.notify_all();return true;
}
bool PairingRuntime::update_pointer_settings(const std::string& peer,hop::PointerSettings settings,std::uint16_t rate) {
    if(!hop::valid_pointer_settings(settings) || rate<20 || rate>1000)return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if(peer.empty() || peer_fingerprint_!=peer)return false;
    pending_pointer_=PointerUpdate{peer,settings,rate};wake_.notify_all();return true;
}
void PairingRuntime::run(PairingRuntimeConfig config) {
    struct FinishedGuard {PairingRuntime* self;~FinishedGuard(){self->notify(PairingRuntimeEvent::stopped);self->finished_.store(true);}} finished{this};
    WSADATA winsock{};if(WSAStartup(MAKEWORD(2,2),&winsock)!=0){state_.store(PairingState::failed);notify(PairingRuntimeEvent::failed);return;}
    struct WinsockGuard {~WinsockGuard(){WSACleanup();}} winsock_guard;
    try {
        DeviceIdentity identity;TrustStore store(backend_);
        if(!store.load_or_create(identity)){state_.store(PairingState::failed);notify(PairingRuntimeEvent::failed);return;}
        std::unique_ptr<ClipboardWorker> clip;
        if(bool(config.clipboard_backend)!=bool(config.clipboard_sequence)){state_.store(PairingState::failed);notify(PairingRuntimeEvent::failed);return;}
        if(config.clipboard_backend){clip=ClipboardWorker::start(config.clipboard_backend);if(!clip){state_.store(PairingState::failed);notify(PairingRuntimeEvent::failed);return;}}
        std::unique_ptr<HostCapturePump> capture;
        std::uint64_t clip_epoch=0,clip_activate=0,clip_revoke=0,clip_revoke_started=0,last_clip_poll=0,transfer_id=0;
        bool clip_ready_sent=false,initial_requested=false;std::unique_ptr<ClipboardSend> sending;
        struct PeerLink {ConnectedPeer identity;std::unique_ptr<PairingWorker> worker;};
        PeerLink* selected_peer=nullptr;
        std::vector<std::unique_ptr<PeerLink>> peers;
        std::shared_ptr<LanServer> listener;
        TcpAddress listener_request{};
        auto configure_session=[&](Session& session) {
            if(!clip)return true;auto* current=&session;
            if(config.role==SessionRole::receiver && !session.set_release_barrier(
                [&](std::uint64_t now) {
                    if(!clip_epoch && !clip_activate && !clip_revoke)return true;
                    if(!clip_revoke){clip_revoke=clip->actor().revoke(now);clip_revoke_started=now;}
                    sending.reset();clip_ready_sent=false;return clip_revoke!=0;
                },
                [&] {return !clip_revoke || clip->actor().acknowledged(clip_revoke);}))return false;
            return session.enable_clipboard(config.clipboard_sequence,[&,current](const hop::ClipboardValue& value,std::uint32_t baseline) {
                const auto& peer=current->peer_fingerprint();
                if(peer.empty() || current->phase()!=SessionPhase::active || current->focus_epoch()!=value.epoch ||
                   (config.role==SessionRole::host && (!selected_peer || selected_peer->worker->session()!=current)))return false;
                const auto format=value.format==hop::ClipboardFormat::text ? hop::windows::ClipboardFormat::utf8_text : hop::windows::ClipboardFormat::png;
                return clip->actor().submit_remote(peer,value.epoch,baseline,{format,value.data});
            });
        };
        const auto same_endpoint=[](const TcpAddress& a,const TcpAddress& b) {
            return a.size>0 && a.size==b.size && static_cast<std::size_t>(a.size)<=sizeof(a.value) &&
                std::memcmp(&a.value,&b.value,static_cast<std::size_t>(a.size))==0;
        };
        auto start_link=[&](const AddPeer& request,std::uint64_t now)->std::unique_ptr<PeerLink> {
            auto link=std::make_unique<PeerLink>();
            auto* owner=link.get();
            auto wrapped_callbacks=[&,owner] {
                auto callbacks=config.callbacks();auto restore=callbacks.restore;
                callbacks.restore=[&,owner,restore] {
                    if(config.role==SessionRole::host && selected_peer!=owner)return true;
                    const bool stopped=!capture || capture->deactivate();
                    return restore && restore() && stopped;
                };
                return callbacks;
            };
            link->worker=std::make_unique<PairingWorker>(identity,store,config.role,config.name,config.screens,
                                                        std::move(wrapped_callbacks),configure_session);
            if(request.listen) {
                TcpAddress actual{};
                if(!listener || (!same_endpoint(request.address,listener_request) &&
                    !(listener->local_address(actual) && same_endpoint(request.address,actual)))) {
                    auto next=LanServer::start(request.address,true);
                    if(!next)return nullptr;
                    listener=std::move(next);
                    listener_request=request.address;
                }
                if(!link->worker->begin_listening(listener,now) || !listener->local_address(actual))return nullptr;
                {std::lock_guard<std::mutex> lock(mutex_);listen_address_=actual;}
                notify(PairingRuntimeEvent::listener_ready);
            } else if(!link->worker->begin_connecting(request.address,now))return nullptr;
            return link;
        };
        auto pending=start_link({config.listen,config.address},GetTickCount64());
        if(!pending){state_.store(PairingState::failed);notify(PairingRuntimeEvent::failed);return;}
        PairingState prior=PairingState::idle;
        bool announced=false,awaiting_focus=false,sharing=false,passive=false;
        RemoteScreen selected{};std::vector<RemoteScreen> last_displays;
        EdgeDetector detector;LiveLayout return_layout;bool edge_enter=false,auto_edge_enabled=false;std::optional<EdgeRequest> edge_config;
        std::optional<PeerDestination> handoff;
        std::uint64_t edge_generation=0;std::uint16_t selected_x=32768,selected_y=32768;
        const auto local_geometry_valid=[&](const EdgeRequest& request) {
            return !config.verify_local_geometry || config.verify_local_geometry(request.local);
        };
        const auto peer_index=[&](const std::string& fingerprint) {
            return std::find_if(peers.begin(),peers.end(),[&](const auto& link){return link->identity.fingerprint==fingerprint;});
        };
        const auto same_screen=[](const auto& a,const auto& b) {
            return a.id==b.id && a.x==b.x && a.y==b.y && a.width==b.width && a.height==b.height;
        };
        const auto world_current=[&](const EdgeRequest& request) {
            if(request.peers.size()!=peers.size())return false;
            for(const auto& expected:request.peers) {
                const auto live=peer_index(expected.fingerprint);
                if(live==peers.end())return false;
                const auto& screens=(*live)->identity.displays;
                if(screens.size()!=expected.screens.size() ||
                   !std::equal(screens.begin(),screens.end(),expected.screens.begin(),same_screen))return false;
            }
            return true;
        };
        const auto arm_workspace=[&](const EdgeRequest& request) {
            return edge_generation!=UINT64_MAX && local_geometry_valid(request) &&
                detector.configure_peers(++edge_generation,request.local,request.peers,request.placements) &&
                return_layout.bind_peers(edge_generation,request.local,request.peers,request.placements) &&
                config.host_capture->start();
        };
        const auto reset_workspace=[&] {
            detector.disarm();return_layout.clear();edge_config.reset();handoff.reset();auto_edge_enabled=false;
        };
        const auto update_screens=[&](PeerLink& link) {
            auto displays=link.worker->peer_displays();
            if(displays.size()==link.identity.displays.size() &&
               std::equal(displays.begin(),displays.end(),link.identity.displays.begin(),same_screen))return false;
            link.identity.displays=std::move(displays);
            return true;
        };
        const auto publish_peers=[&] {
            std::vector<ConnectedPeer> connected;
            for(const auto& peer:peers)connected.push_back(peer->identity);
            {std::lock_guard<std::mutex> lock(mutex_);connected_peers_=std::move(connected);}
            notify(PairingRuntimeEvent::peers_changed);
        };
        const auto publish_selection=[&](PairingRuntimeEvent event=PairingRuntimeEvent::connected) {
            last_displays=selected_peer->identity.displays;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                review_.reset();
                peer_fingerprint_=selected_peer->identity.fingerprint;
                peer_displays_=last_displays;
                retargeting_=false;
            }
            state_.store(PairingState::connected);
            notify(event);
        };
        const auto select_link=[&](PeerLink* link) {
            if(selected_peer==link)return;
            if(selected_peer){config.pointer_settings={};config.motion_rate_hz=240;}
            selected_peer=link;
            const auto profile=config.pointer_profile ? config.pointer_profile(link->identity.fingerprint) : std::nullopt;
            if(profile && hop::valid_pointer_settings(profile->settings) && profile->motion_rate_hz>=20 && profile->motion_rate_hz<=1000) {
                config.pointer_settings=profile->settings;config.motion_rate_hz=profile->motion_rate_hz;
            }
        };
        const auto reject_retarget=[&] {
            {std::lock_guard<std::mutex> lock(mutex_);retargeting_=false;}
            notify(PairingRuntimeEvent::peer_action_failed);
        };
        const auto can_retarget=[&] {
            const auto* session=selected_peer ? selected_peer->worker->session() : nullptr;
            return session && session->phase()==SessionPhase::ready && !capture &&
                !awaiting_focus && !sharing && !passive && !auto_edge_enabled && !clip_epoch && !clip_revoke;
        };
        const auto wait_for_work=[&] {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait_for(lock,std::chrono::milliseconds(10),[&]{
                return stop_.load() || confirmation_.load()!=0 || control_command_!=0 ||
                    pending_pointer_ || pending_add_peer_ || pending_select_peer_;
            });
        };
        while(!stop_.load()) {
            const auto now=GetTickCount64();
            // Check the deadline before accepting a new control command, even
            // if the actor completed while this thread was not scheduled.
            if(clip_revoke && now-clip_revoke_started>=2000){state_.store(PairingState::failed);notify(PairingRuntimeEvent::failed);break;}
            std::optional<PointerUpdate> pointer;
            std::optional<EdgeRequest> request;
            std::optional<AddPeer> add_peer;
            std::optional<std::string> select_peer;
            std::uint64_t requested_screen=0;
            int command=0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                pointer=std::move(pending_pointer_);
                pending_pointer_.reset();
                command=control_command_;
                control_command_=0;
                if(command==1)requested_screen=control_screen_;
                if(command==3){request=std::move(pending_edge_);pending_edge_.reset();}
                add_peer=std::move(pending_add_peer_);pending_add_peer_.reset();
                select_peer=std::move(pending_select_peer_);pending_select_peer_.reset();
            }
            bool changed_peers=false;
            for(auto it=peers.begin();it!=peers.end();) {
                if(it->get()==selected_peer){++it;continue;}
                if(!(*it)->worker->step(now)){it=peers.erase(it);changed_peers=true;}
                else {changed_peers=update_screens(**it) || changed_peers;++it;}
            }
            if(changed_peers)publish_peers();
            if(add_peer) {
                if(!can_retarget() || pending || peers.size()>=8 || !(pending=start_link(*add_peer,now)))
                    reject_retarget();
                else {prior=PairingState::idle;state_.store(PairingState::waiting_candidate);}
            }
            if(select_peer) {
                const auto next=peer_index(*select_peer);
                if(!can_retarget() || pending || next==peers.end())reject_retarget();
                else {
                    select_link(next->get());
                    publish_selection();
                }
            }
            if(pending) {
                if(selected_peer && !selected_peer->worker->step(now)) {
                    state_.store(PairingState::failed);notify(PairingRuntimeEvent::failed);break;
                }
                auto& pairing=*pending->worker;
                const int confirmation=confirmation_.exchange(0);
                if((confirmation && !pairing.confirm_authenticated_pair(confirmation>0)) || !pairing.step(now)) {
                    pending.reset();
                    if(!selected_peer){state_.store(PairingState::failed);notify(PairingRuntimeEvent::failed);break;}
                    publish_selection();
                    reject_retarget();
                } else if(pairing.state()==PairingState::connected) {
                    const auto review=pairing.review();
                    pending->identity.fingerprint=pairing.session()->peer_fingerprint();
                    if(review && review->remote_fingerprint()==pending->identity.fingerprint)pending->identity.name=review->untrusted_name();
                    if(pending->identity.fingerprint.empty() || peer_index(pending->identity.fingerprint)!=peers.end()) {
                        pending.reset();
                        if(!selected_peer){state_.store(PairingState::failed);notify(PairingRuntimeEvent::failed);break;}
                        publish_selection();
                        reject_retarget();
                    } else {
                        update_screens(*pending);
                        select_link(pending.get());
                        peers.push_back(std::move(pending));
                        publish_peers();
                        publish_selection();
                    }
                } else if(pairing.state()!=prior) {
                    prior=pairing.state();
                    if(prior==PairingState::reviewing) {
                        {std::lock_guard<std::mutex> lock(mutex_);review_=pairing.review();}
                        state_.store(prior);notify(PairingRuntimeEvent::reviewing);
                    } else {
                        state_.store(prior);
                        if(prior==PairingState::confirming)notify(PairingRuntimeEvent::confirming);
                    }
                }
                wait_for_work();
                continue;
            }
            if(listener) {
                std::unique_ptr<ClassifiedSocket> ignored;
                if(!listener->poll(now,ignored))listener.reset();
            }
            auto& worker=*selected_peer->worker;
            if(pointer) {
                if(!worker.session() || worker.session()->peer_fingerprint()!=pointer->peer)notify(PairingRuntimeEvent::action_failed);
                else if(capture && !capture->update_pointer_settings(pointer->settings,pointer->rate,now))notify(PairingRuntimeEvent::action_failed);
                else {config.pointer_settings=pointer->settings;config.motion_rate_hz=pointer->rate;}
            }
            if(command==1) {
                const auto displays=worker.peer_displays();const auto match=std::find_if(displays.begin(),displays.end(),[&](const auto& d){return d.id==requested_screen;});
                if(config.role!=SessionRole::host || !config.host_capture || passive || capture || awaiting_focus || auto_edge_enabled || clip_epoch || clip_revoke || match==displays.end() || !config.host_capture->inputs_clear() || !worker.request_focus(requested_screen,32768,32768))notify(PairingRuntimeEvent::action_failed);
                else {selected=*match;selected_x=selected_y=32768;edge_enter=false;awaiting_focus=true;}
            } else if(command==2) {
                reset_workspace();
                if(passive){detector.disarm();if(!config.host_capture->stop()){state_.store(PairingState::failed);notify(PairingRuntimeEvent::failed);break;}passive=false;}
                if(worker.session() && worker.session()->phase()==SessionPhase::active) {if(!worker.session()->release(1))worker.session()->disconnect();}
                else if(awaiting_focus && worker.session())worker.session()->disconnect();
                awaiting_focus=false;
            } else if(command==3) {
                if(config.role!=SessionRole::host || !config.host_capture || !can_retarget() || !request ||
                   !world_current(*request) || !arm_workspace(*request)) {
                    reset_workspace();notify(PairingRuntimeEvent::action_failed);
                } else {passive=true;edge_config=std::move(request);auto_edge_enabled=true;notify(PairingRuntimeEvent::edge_monitor_started);}
            }
            if(!worker.step(now)){state_.store(PairingState::failed);notify(PairingRuntimeEvent::failed);break;}
            if(update_screens(*selected_peer))publish_peers();
            if(clip) {
                if(!clip->healthy()){state_.store(PairingState::failed);notify(PairingRuntimeEvent::failed);break;}
                auto* session=worker.session();const bool focus=session && session->phase()==SessionPhase::active && session->clipboard_negotiated() && !session->clipboard_paused();
                const auto epoch=focus ? session->focus_epoch() : 0;
                if(clip_epoch && clip_epoch!=epoch && !clip_revoke) {
                    clip_revoke=clip->actor().revoke(now);clip_revoke_started=now;sending.reset();clip_ready_sent=false;
                    if(!clip_revoke){state_.store(PairingState::failed);notify(PairingRuntimeEvent::failed);break;}
                }
                if(clip_revoke) {
                    if(now-clip_revoke_started>=2000){state_.store(PairingState::failed);notify(PairingRuntimeEvent::failed);break;}
                    if(clip->actor().acknowledged(clip_revoke)){clip_revoke=0;clip_epoch=clip_activate=0;}
                }
                if(focus && !clip_epoch && !clip_revoke) {
                    const auto& peer=session->peer_fingerprint();
                    const auto sequence=config.role==SessionRole::receiver ? session->focus_clipboard_baseline() : config.clipboard_sequence();
                    if(peer.empty() || !sequence || !(clip_activate=clip->actor().activate(peer,epoch,sequence,now))){state_.store(PairingState::failed);notify(PairingRuntimeEvent::failed);break;}
                    clip_epoch=epoch;clip_ready_sent=false;initial_requested=false;
                }
                if(focus && clip_epoch==epoch && clip_activate && !clip_ready_sent && clip->actor().acknowledged(clip_activate)) {
                    if(!session->request_clipboard_ready()){state_.store(PairingState::failed);notify(PairingRuntimeEvent::failed);break;}
                    clip_ready_sent=true;
                }
                if(clip_activate && !clip->actor().acknowledged(clip_activate) && clip->actor().timed_out(clip_activate,now)){state_.store(PairingState::failed);notify(PairingRuntimeEvent::failed);break;}
                if(focus && clip_epoch==epoch && session->clipboard_can_sync()) {
                    if(config.role==SessionRole::receiver && session->clipboard_initial_done() && !session->clipboard_initial_drained() && clip->actor().quiescent(epoch)) {
                        if(!session->mark_clipboard_initial_drained()){state_.store(PairingState::failed);notify(PairingRuntimeEvent::failed);break;}
                    }
                    if(config.role==SessionRole::host && !session->clipboard_initial_done() && !initial_requested) {
                        const auto sequence=config.clipboard_sequence();
                        if(!sequence || !clip->actor().request_initial_snapshot(epoch,sequence)){state_.store(PairingState::failed);notify(PairingRuntimeEvent::failed);break;}
                        initial_requested=true;
                    }
                    const bool initial_host=config.role==SessionRole::host && !session->clipboard_initial_done();
                    const bool can_send=initial_host ? initial_requested && clip->actor().quiescent(epoch) :
                        (config.role==SessionRole::host ? session->clipboard_initial_done() : session->clipboard_initial_drained()) && clip->actor().quiescent(epoch);
                    if(!initial_host && can_send && now-last_clip_poll>=100){const auto sequence=config.clipboard_sequence();if(sequence)clip->actor().observe_local(epoch,sequence);last_clip_poll=now;}
                    if(!sending && can_send)if(auto value=clip->actor().take_local(epoch)){
                        sending=std::make_unique<ClipboardSend>();sending->payload=std::move(*value);sending->epoch=epoch;
                        const auto format=sending->payload.format==hop::windows::ClipboardFormat::utf8_text?hop::ClipboardFormat::text:hop::ClipboardFormat::png;
                        if(!session->clipboard_supports(format))sending.reset();
                        else {sending->id=++transfer_id;if(!sending->id){state_.store(PairingState::failed);notify(PairingRuntimeEvent::failed);break;}}
                    }
                    if(sending && session->clipboard_queue_empty()) {
                        if(!sending->started){const auto format=sending->payload.format==hop::windows::ClipboardFormat::utf8_text?hop::ClipboardFormat::text:hop::ClipboardFormat::png;
                            if(!session->clipboard_start(sending->id,format,static_cast<std::uint32_t>(sending->payload.bytes.size()))){state_.store(PairingState::failed);notify(PairingRuntimeEvent::failed);break;}sending->started=true;
                        } else {
                            const auto remaining=sending->payload.bytes.size()-sending->offset;
                            if(remaining){const auto count=std::min<std::size_t>(remaining,65520);if(!session->clipboard_chunk(sending->id,sending->payload.bytes.data()+sending->offset,count)){state_.store(PairingState::failed);notify(PairingRuntimeEvent::failed);break;}sending->offset+=count;}
                        }
                        if(sending->started && sending->offset==sending->payload.bytes.size())sending.reset();
                    }
                    if(initial_host && can_send && !sending && session->clipboard_queue_empty()) {
                        if(!session->send_clipboard_initial_done()){state_.store(PairingState::failed);notify(PairingRuntimeEvent::failed);break;}
                    }
                }
            }
            const auto& displays=selected_peer->identity.displays;
            if(displays.size()!=last_displays.size() || !std::equal(displays.begin(),displays.end(),last_displays.begin(),same_screen)) {
                last_displays=displays;{std::lock_guard<std::mutex> lock(mutex_);peer_displays_=displays;}notify(PairingRuntimeEvent::screens_changed);
            }
            const bool valid_workspace=edge_config && world_current(*edge_config);
            if(edge_enter && (capture || awaiting_focus) && !valid_workspace && auto_edge_enabled) {
                if(worker.session())worker.session()->disconnect();
                edge_enter=false;reset_workspace();notify(PairingRuntimeEvent::action_failed);
            }
            if(passive) {
                if(!valid_workspace || !worker.session() || worker.session()->phase()!=SessionPhase::ready) {
                    if(!config.host_capture->stop()){state_.store(PairingState::failed);notify(PairingRuntimeEvent::failed);break;}
                    passive=false;reset_workspace();notify(PairingRuntimeEvent::action_failed);
                } else {
                    CaptureEvent event{};for(unsigned i=0;i<256 && config.host_capture->poll(event);++i)if(event.kind==CaptureKind::move) {
                        auto target=detector.sample_target(edge_generation,event.x,event.y,config.host_capture->inputs_clear());if(!target)continue;
                        if(!config.host_capture->stop()){state_.store(PairingState::failed);notify(PairingRuntimeEvent::failed);break;}passive=false;
                        handoff=std::move(*target);
                        break;
                    }
                    if(state_.load()==PairingState::failed)break;
                }
            }
            if(awaiting_focus && worker.session() && worker.session()->phase()==SessionPhase::active) {
                capture=std::make_unique<HostCapturePump>(*worker.session(),*config.host_capture);
                if(edge_enter && !capture->set_route_edge([&](std::uint64_t screen,std::uint16_t x,std::uint16_t y,std::int32_t dx,std::int32_t dy)->std::optional<LiveDestination>{
                    const auto& peer=selected_peer->identity.fingerprint;
                    std::optional<LiveDestination> target;
                    if(dx<0)target=return_layout.route_peer(edge_generation,peer,screen,hop::Edge::left,x,y);
                    if(!target && dx>0)target=return_layout.route_peer(edge_generation,peer,screen,hop::Edge::right,x,y);
                    if(!target && dy<0)target=return_layout.route_peer(edge_generation,peer,screen,hop::Edge::top,x,y);
                    if(!target && dy>0)target=return_layout.route_peer(edge_generation,peer,screen,hop::Edge::bottom,x,y);
                    return target;
                })){worker.session()->disconnect();awaiting_focus=false;reset_workspace();notify(PairingRuntimeEvent::action_failed);}
                else
                if(!capture->begin(selected.id,selected.width,selected.height,selected_x,selected_y,config.pointer_settings,now,config.motion_rate_hz)){awaiting_focus=false;reset_workspace();notify(PairingRuntimeEvent::action_failed);}
                else awaiting_focus=false;
            }
            if(capture && worker.session() && worker.session()->phase()==SessionPhase::active) {
                if(!capture->step(now)){reset_workspace();notify(PairingRuntimeEvent::action_failed);}
                else {
                    if(auto route=capture->take_handoff())handoff=std::move(*route);
                    if(capture->active() && !sharing){sharing=true;notify(PairingRuntimeEvent::sharing_started);}
                }
            }
            if(capture && (!worker.session() || worker.session()->phase()!=SessionPhase::active) && capture->drain()) {capture.reset();if(sharing){sharing=false;notify(PairingRuntimeEvent::sharing_stopped);}}
            if(auto_edge_enabled && !capture && !passive && !awaiting_focus && worker.session() &&
               worker.session()->phase()==SessionPhase::ready && !clip_epoch && !clip_revoke) {
                if(handoff) {
                    const auto next=peer_index(handoff->peer);
                    const auto position=handoff->position;
                    const auto* target=next==peers.end()?nullptr:next->get();
                    const auto screen=target?std::find_if(target->identity.displays.begin(),target->identity.displays.end(),
                        [&](const auto& d){return d.id==position.screen;}):std::vector<RemoteScreen>::const_iterator{};
                    if(!valid_workspace || !target || screen==target->identity.displays.end() ||
                       !local_geometry_valid(*edge_config) || !config.host_capture->inputs_clear() ||
                       !target->worker->request_focus(position.screen,position.x,position.y)) {
                        reset_workspace();notify(PairingRuntimeEvent::action_failed);
                    } else {
                        selected=*screen;selected_x=position.x;selected_y=position.y;
                        edge_enter=true;awaiting_focus=true;handoff.reset();
                        if(target!=selected_peer){select_link(next->get());publish_selection(PairingRuntimeEvent::target_changed);}
                    }
                } else if(edge_enter) {
                    if(!valid_workspace || !arm_workspace(*edge_config)) {
                        reset_workspace();notify(PairingRuntimeEvent::action_failed);
                    } else {edge_enter=false;passive=true;notify(PairingRuntimeEvent::edge_monitor_started);}
                }
            }
            if(!announced && config.role==SessionRole::receiver && !config.screen_payload.empty()) {
                if(!worker.session() || !worker.session()->advertise_screens(config.screen_payload)){state_.store(PairingState::failed);notify(PairingRuntimeEvent::failed);break;}announced=true;
            }
            wait_for_work();
        }
        if(pending)pending->worker->cancel();
        if(selected_peer)selected_peer->worker->cancel();
        peers.clear();selected_peer=nullptr;
        sending.reset();
        if(passive && config.host_capture){detector.disarm();config.host_capture->stop();}
        if(clip) {const auto now=GetTickCount64();if(clip->request_stop(now))while(!clip->poll_stop(GetTickCount64()))Sleep(10);clip.reset();}
        if(capture) {
            capture->deactivate();const auto deadline=GetTickCount64()+2000;
            while(GetTickCount64()<deadline && !capture->drain())Sleep(10);
            if(!config.host_capture->drained()){config.host_capture->force_recovery();const auto final_deadline=GetTickCount64()+1000;while(GetTickCount64()<final_deadline && !capture->drain())Sleep(10);}
            capture.reset();
        }
        {std::lock_guard<std::mutex> lock(mutex_);review_.reset();peer_fingerprint_.clear();connected_peers_.clear();listen_address_={};peer_displays_.clear();}
    } catch(...) {state_.store(PairingState::failed);notify(PairingRuntimeEvent::failed);}
}
}
