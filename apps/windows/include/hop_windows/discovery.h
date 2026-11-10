#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hop::windows {
struct DiscoveryCandidate {std::wstring instance,address;std::uint16_t port=0;};
// Results are untrusted connection hints. Selecting one never grants trust;
// the usual explicit pairing and certificate verification are still required.
class Discovery {
public:
    Discovery();~Discovery();
    Discovery(const Discovery&)=delete;Discovery& operator=(const Discovery&)=delete;
    bool start();void stop();
    bool advertise(std::uint16_t port);void stop_advertising();
    std::vector<DiscoveryCandidate> candidates() const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
