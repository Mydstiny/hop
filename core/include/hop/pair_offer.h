#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace hop {
// Unauthenticated public candidate only. The certificate MUST be compared via
// a trusted independent channel and fixed in TLS before it can create trust.
struct PairOffer {
    std::uint8_t role = 0; // 1 host, 2 receiver
    std::array<std::uint8_t,16> nonce{};
    std::string name, certificate_pem;
};
std::vector<std::uint8_t> encode_pair_offer(const PairOffer& offer);
class PairOfferDecoder {
public:
    bool feed(const std::uint8_t* data, std::size_t count);
    std::optional<PairOffer> finish();
    bool failed() const { return failed_; }
private:
    std::vector<std::uint8_t> bytes_;
    std::size_t expected_ = 0;
    bool failed_ = false, finished_ = false;
};
// A public fingerprint preface is only a lookup hint for an already trusted or
// explicitly confirmed candidate. The subsequent TLS handshake must still pin.
std::vector<std::uint8_t> encode_peer_selector(const std::string& fingerprint);
std::optional<std::string> decode_peer_selector(const std::uint8_t* data, std::size_t count);
}
