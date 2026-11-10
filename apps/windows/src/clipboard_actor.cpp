#include "hop_windows/clipboard_actor.h"
#include <limits>
#include <utility>
namespace hop::windows {
namespace {
void wipe(ClipboardPayload& payload) {volatile std::uint8_t* p=payload.bytes.data();for(std::size_t i=0;i<payload.bytes.size();++i)p[i]=0;payload.bytes.clear();}
}
ClipboardActor::~ClipboardActor(){std::lock_guard<std::mutex> lock(mutex_);clear_pending();clear_local();}
void ClipboardActor::clear_pending(){if(pending_){wipe(pending_->payload);pending_.reset();}}
void ClipboardActor::clear_local(){pending_sequence_=0;pending_snapshot_=false;if(outgoing_){wipe(*outgoing_);outgoing_.reset();}outgoing_epoch_=0;}
std::uint64_t ClipboardActor::activate(std::string peer,std::uint64_t epoch,std::uint32_t baseline,std::uint64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(peer.empty() || !epoch || !baseline || requested_.enabled || busy_ || requested_ticket_!=completed_ticket_ || requested_ticket_==std::numeric_limits<std::uint64_t>::max() || now>std::numeric_limits<std::uint64_t>::max()-2000)return 0;
    clear_pending();clear_local();requested_={true,std::move(peer),epoch,baseline};deadline_=now+2000;return ++requested_ticket_;
}
std::uint64_t ClipboardActor::revoke(std::uint64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(requested_ticket_==std::numeric_limits<std::uint64_t>::max() || now>std::numeric_limits<std::uint64_t>::max()-2000)return 0;
    clear_pending();clear_local();requested_={};deadline_=now+2000;return ++requested_ticket_;
}
bool ClipboardActor::acknowledged(std::uint64_t ticket) const {std::lock_guard<std::mutex> lock(mutex_);return ticket && completed_ticket_>=ticket;}
bool ClipboardActor::timed_out(std::uint64_t ticket,std::uint64_t now) const {
    std::lock_guard<std::mutex> lock(mutex_);return ticket && ticket==requested_ticket_ && completed_ticket_<ticket && now>=deadline_;
}
bool ClipboardActor::observe_local(std::uint64_t epoch,std::uint32_t sequence) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!requested_.enabled || requested_ticket_!=completed_ticket_ || epoch!=requested_.epoch || !sequence)return false;
    pending_sequence_=sequence;return true;
}
bool ClipboardActor::request_initial_snapshot(std::uint64_t epoch,std::uint32_t sequence) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!requested_.enabled || requested_ticket_!=completed_ticket_ || epoch!=requested_.epoch || !sequence || pending_snapshot_)return false;
    pending_sequence_=sequence;pending_snapshot_=true;return true;
}
bool ClipboardActor::quiescent(std::uint64_t epoch) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return requested_.enabled && requested_ticket_==completed_ticket_ && requested_.epoch==epoch && !busy_ && !pending_ && !pending_sequence_;
}
std::optional<ClipboardPayload> ClipboardActor::take_local(std::uint64_t epoch) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!requested_.enabled || requested_ticket_!=completed_ticket_ || epoch!=requested_.epoch || epoch!=outgoing_epoch_ || !outgoing_)return std::nullopt;
    auto result=std::move(outgoing_);outgoing_.reset();outgoing_epoch_=0;return result;
}
bool ClipboardActor::submit_remote(const std::string& peer,std::uint64_t epoch,std::uint32_t baseline,ClipboardPayload payload) {
    std::lock_guard<std::mutex> lock(mutex_);
    if((payload.format!=ClipboardFormat::utf8_text && payload.format!=ClipboardFormat::png) || !requested_.enabled || requested_ticket_!=completed_ticket_ || busy_ || pending_ || peer!=requested_.peer || epoch!=requested_.epoch || !baseline ||
       payload.bytes.size()>(payload.format==ClipboardFormat::utf8_text?max_text_bytes:max_png_bytes)) {wipe(payload);return false;}
    try {pending_=Pending{peer,epoch,baseline,std::move(payload)};return true;}catch(...){wipe(payload);return false;}
}
bool ClipboardActor::work_once() {
    Configuration config;std::uint64_t ticket=0,read_epoch=0;std::uint32_t read_sequence=0;bool snapshot=false;std::optional<Pending> value;
    {
        std::lock_guard<std::mutex> lock(mutex_);if(busy_)return false;
        if(requested_ticket_!=completed_ticket_) {config=requested_;ticket=requested_ticket_;busy_=true;}
        else if(pending_) {value=std::move(pending_);pending_.reset();busy_=true;}
        else if(pending_sequence_) {read_sequence=pending_sequence_;pending_sequence_=0;snapshot=pending_snapshot_;pending_snapshot_=false;read_epoch=requested_.epoch;busy_=true;}
        else return true;
    }
    bool ok=true;
    try {
        if(ticket) sync_.configure(config.enabled,config.enabled,config.peer,config.epoch,config.baseline);
        else if(value) {
            bool current=false;{std::lock_guard<std::mutex> lock(mutex_);current=requested_.enabled && requested_ticket_==completed_ticket_ && value->peer==requested_.peer && value->epoch==requested_.epoch;}
            if(current)ok=sync_.apply_remote(value->peer,value->epoch,value->payload,value->baseline);
            wipe(value->payload);
        } else if(read_sequence) {
            auto result=snapshot ? sync_.initial_snapshot(read_sequence,read_epoch) : sync_.on_change(read_sequence,read_epoch);
            if(result) {
                std::lock_guard<std::mutex> lock(mutex_);
                if(requested_.enabled && requested_ticket_==completed_ticket_ && requested_.epoch==read_epoch) {
                    if(outgoing_)wipe(*outgoing_);outgoing_=std::move(*result);outgoing_epoch_=read_epoch;
                } else wipe(*result);
            }
        }
    } catch(...) {if(value)wipe(value->payload);ok=false;}
    {
        std::lock_guard<std::mutex> lock(mutex_);if(ticket && ok)completed_ticket_=ticket;busy_=false;
    }
    return ok;
}
}
