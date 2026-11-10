#pragma once
#include "hop_windows/pair_transport.h"
#include "hop_windows/trust_store.h"
#include <memory>
#include <mutex>
#include <optional>
namespace hop::windows {
// UI must display both full fingerprints and obtain an explicit user action
// after an independent-channel comparison. Candidate name is untrusted.
// Approval is temporary and cannot write the trust store by itself. Copies
// share the same cancellation/approval state across UI and session threads.
class PairingReview {
public:
    static std::optional<PairingReview> create(const std::string& local_certificate,UntrustedCandidate candidate,std::uint64_t now_ms);
    const std::string& local_fingerprint() const { return state_->local; }
    const std::string& remote_fingerprint() const { return state_->candidate.fingerprint; }
    const std::string& untrusted_name() const { return state_->candidate.offer.name; }
    bool approve_after_independent_comparison(std::uint64_t now_ms);
    void cancel();
    bool cancelled() const;
    bool approved_peer(std::uint64_t now_ms,VerifiedPeerCertificate& peer,std::uint64_t& expires_ms) const;
    bool resolve_hint(const std::string& sender_fingerprint,std::uint64_t now_ms,TrustStore& store,VerifiedPeerCertificate& peer) const;
private:
    struct State {
        State(std::string fingerprint,UntrustedCandidate value) : local(std::move(fingerprint)),candidate(std::move(value)) {}
        const std::string local; UntrustedCandidate candidate; mutable std::mutex mutex; bool approved=false,cancelled=false;
    };
    PairingReview(std::string local,UntrustedCandidate candidate) : state_(std::make_shared<State>(std::move(local),std::move(candidate))) {}
    std::shared_ptr<State> state_;
};
}
