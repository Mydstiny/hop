#pragma once
#include "hop/pair_offer.h"
#include <winsock2.h>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
namespace hop::windows {
enum class PairIo { pending, ready, failed };
struct UntrustedCandidate {
    hop::PairOffer offer; std::string fingerprint; std::uint64_t created_ms=0, expires_ms=0;
    bool valid_at(std::uint64_t now) const { return now>=created_ms && now<expires_ms; }
};
// Caller explicitly chooses to exchange candidates, then compares full
// fingerprints independently. Neither the name nor nonce establishes trust.
// start() consumes the connected socket even on failure. No method creates a
// listener or touches input, clipboard, credentials, or the desktop.
class CandidateExchange {
public:
    static std::unique_ptr<CandidateExchange> start(SOCKET socket, bool explicitly_requested,bool initiator,
        std::uint8_t role, std::string name, std::string own_certificate, std::uint64_t now_ms);
    ~CandidateExchange();
    PairIo step(std::uint64_t now_ms);
    std::optional<UntrustedCandidate> take(std::uint64_t now_ms);
private:
    explicit CandidateExchange(SOCKET socket) : socket_(socket) {}
    PairIo fail();
    SOCKET socket_=INVALID_SOCKET;
    std::uint8_t role_=0;
    std::uint64_t started_=0,last_now_=0;
    bool initiator_=false,sent_=false, received_=false, done_=false, failed_=false;
    std::size_t sent_bytes_=0;
    std::vector<std::uint8_t> outgoing_;
    hop::PairOfferDecoder decoder_;
    std::optional<UntrustedCandidate> candidate_;
};
// Exactly 68 bytes. The receiver reads no further bytes, preserving any TLS
// ClientHello already queued on the same stream. A returned fingerprint is an
// untrusted lookup hint; the caller must select a verified certificate and pin
// it in TLS before processing application data.
class PeerSelector {
public:
    static std::unique_ptr<PeerSelector> sender(SOCKET socket, const std::string& own_fingerprint, std::uint64_t now_ms);
    static std::unique_ptr<PeerSelector> receiver(SOCKET socket, std::uint64_t now_ms);
    ~PeerSelector();
    PairIo step(std::uint64_t now_ms);
    const std::string& hint() const { return hint_; }
    SOCKET release(); // only after ready; transfers socket ownership
private:
    PeerSelector(SOCKET socket,bool sending,std::uint64_t now) : socket_(socket), sending_(sending), started_(now),last_now_(now) {}
    PairIo fail();
    SOCKET socket_=INVALID_SOCKET;
    bool sending_=false, ready_=false, failed_=false;
    std::uint64_t started_=0,last_now_=0;
    std::array<std::uint8_t,68> bytes_{};
    std::size_t used_=0;
    std::string hint_;
};
}
