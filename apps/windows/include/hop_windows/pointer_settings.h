#pragma once
#include "hop/input_policy.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hop::windows {
// Stable device identity: SHA-256 fingerprint of the separately verified peer
// certificate, represented as raw bytes. Never use a hostname/IP as identity.
using DeviceFingerprint = std::array<std::uint8_t,32>;
bool parse_device_fingerprint(const std::string& hex, DeviceFingerprint& out);
class PointerSettingsStore {
public:
    virtual ~PointerSettingsStore() = default;
    virtual bool load(const DeviceFingerprint& peer, std::vector<std::uint8_t>& bytes, bool& found) = 0;
    virtual bool save(const DeviceFingerprint& peer, const std::vector<std::uint8_t>& bytes) = 0;
};
class RegistryPointerSettingsStore final : public PointerSettingsStore {
public:
    bool load(const DeviceFingerprint& peer, std::vector<std::uint8_t>& bytes, bool& found) override;
    bool save(const DeviceFingerprint& peer, const std::vector<std::uint8_t>& bytes) override;
};
// select/update/clear have one control-plane writer and may perform persistence;
// never call them from a low-level hook. Motion/wheel use one atomic snapshot
// for concurrent consumers and never access registry or hardware. Hot-path
// multipliers are quantized to 0.0001; the persisted values retain full precision.
class TargetPointerSettings {
public:
    explicit TargetPointerSettings(PointerSettingsStore& store) : store_(store) {}
    bool select(const DeviceFingerprint& peer);
    void clear();
    bool update(const DeviceFingerprint& peer, const hop::PointerSettings& value);
    bool update_motion_rate(const DeviceFingerprint& peer, std::uint16_t maximum_hz);
    std::optional<std::uint16_t> motion_rate() const;
    std::optional<hop::PointerSettings> current() const;
    std::optional<hop::Delta> motion(hop::Delta input) const;
    std::optional<hop::Delta> scroll(hop::Delta input) const;
private:
    void publish(bool selected, const hop::PointerSettings& value,std::uint16_t rate);
    std::optional<hop::PointerSettings> snapshot() const;
    PointerSettingsStore& store_;
    DeviceFingerprint peer_{};
    hop::PointerSettings current_{};
    std::uint16_t rate_=240;
    bool selected_ = false;
    std::atomic<std::uint64_t> snapshot_bits_{0};
};
} // namespace hop::windows
