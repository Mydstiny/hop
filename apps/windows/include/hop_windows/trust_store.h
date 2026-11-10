#pragma once
#include "hop_windows/session.h"
#include "hop_windows/tls_socket.h"
#include <cstdint>
#include <string>
#include <vector>
namespace hop::windows {
enum class CredentialRead { found, missing, error };
class CredentialBackend {
public:
    virtual ~CredentialBackend() = default;
    virtual CredentialRead read(const std::wstring& name, std::vector<std::uint8_t>& value) = 0;
    virtual bool write(const std::wstring& name, const std::vector<std::uint8_t>& value) = 0;
    virtual bool erase(const std::wstring& name) = 0;
};
// Windows Credential Manager is per-user. No method is called automatically.
class WindowsCredentialBackend final : public CredentialBackend {
public:
    CredentialRead read(const std::wstring& name, std::vector<std::uint8_t>& value) override;
    bool write(const std::wstring& name, const std::vector<std::uint8_t>& value) override;
    bool erase(const std::wstring& name) override;
};
struct DeviceIdentity {
    std::string certificate, private_key, fingerprint;
    DeviceIdentity() = default;
    DeviceIdentity(const DeviceIdentity&) = delete;
    DeviceIdentity& operator=(const DeviceIdentity&) = delete;
    ~DeviceIdentity();
    void clear();
};
// Single-writer owner for one Windows user. A corrupt or unreadable identity
// is never silently replaced. Pairing persistence requires the live TLS pin
// and both explicit in-session confirmations. Call only on a worker, never on
// an input hook or UI thread. Memory returned to callers must also be cleared.
class TrustStore {
public:
    explicit TrustStore(CredentialBackend& backend) : backend_(backend) {}
    bool load_or_create(DeviceIdentity& identity);
    bool remember(const Session& session, const TlsSocket& socket, const std::string& peer_certificate);
    bool load_peer(const std::string& fingerprint, VerifiedPeerCertificate& peer);
    bool forget(const std::string& fingerprint);
private:
    CredentialBackend& backend_;
};
}
