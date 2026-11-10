#include "hop_windows/pairing_review.h"
#include "hop/secure_channel_c.h"
namespace hop::windows {
namespace {
bool fingerprint(const std::string& pem,std::string& output) {
    char hex[65]{}; if(hop_tls_certificate_fingerprint(pem.data(),pem.size(),hex,sizeof(hex))!=0)return false;
    output=hex;return true;
}
}
std::optional<PairingReview> PairingReview::create(const std::string& local,UntrustedCandidate candidate,std::uint64_t now) {
    std::string own,remote;
    if(!candidate.valid_at(now) || !fingerprint(local,own) || !fingerprint(candidate.offer.certificate_pem,remote) ||
       remote!=candidate.fingerprint || remote==own)return std::nullopt;
    return PairingReview(std::move(own),std::move(candidate));
}
bool PairingReview::approve_after_independent_comparison(std::uint64_t now) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if(state_->cancelled || state_->approved || !state_->candidate.valid_at(now))return false;
    state_->approved=true;return true;
}
void PairingReview::cancel() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->approved=false;state_->cancelled=true;state_->candidate.offer.certificate_pem.clear();
}
bool PairingReview::cancelled() const {std::lock_guard<std::mutex> lock(state_->mutex);return state_->cancelled;}
bool PairingReview::approved_peer(std::uint64_t now,VerifiedPeerCertificate& peer,std::uint64_t& expires) const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    peer.pem.clear();expires=0;
    if(!state_->approved || state_->cancelled || !state_->candidate.valid_at(now))return false;
    peer.pem=state_->candidate.offer.certificate_pem;expires=state_->candidate.expires_ms;return true;
}
bool PairingReview::resolve_hint(const std::string& hint,std::uint64_t now,TrustStore& store,VerifiedPeerCertificate& peer) const {
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        peer.pem.clear();
        if(state_->approved && !state_->cancelled && state_->candidate.valid_at(now) && hint==state_->candidate.fingerprint) {peer.pem=state_->candidate.offer.certificate_pem;return true;}
    }
    return store.load_peer(hint,peer);
}
}
