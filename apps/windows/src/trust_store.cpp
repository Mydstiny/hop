#include "hop_windows/trust_store.h"
#include <windows.h>
#include <wincred.h>
#include <algorithm>
#include <array>
#include <limits>
namespace hop::windows {
namespace {
// Current CREDENTIALW limit documented by Microsoft is 5*512. Some MinGW
// headers still define the legacy 512-byte constant.
constexpr std::size_t max_blob = 5u * 512u;
constexpr wchar_t identity_name[]=L"Hop/v2/device-identity";
void wipe(std::vector<std::uint8_t>& bytes) { if (!bytes.empty()) SecureZeroMemory(bytes.data(),bytes.size()); bytes.clear(); }
void wipe(std::string& value) { if (!value.empty()) SecureZeroMemory(&value[0],value.size()); value.clear(); }
bool fingerprint(const std::string& pem,std::string& out) {
    char hex[65]{}; out.clear();
    if (hop_tls_certificate_fingerprint(pem.data(),pem.size(),hex,sizeof(hex)) != 0) return false;
    out=hex; return true;
}
bool valid_fingerprint(const std::string& hex) {
    if (hex.size()!=64) return false;
    return std::all_of(hex.begin(),hex.end(),[](char c){return (c>='0'&&c<='9') || (c>='A'&&c<='F');});
}
std::wstring peer_name(const std::string& hex) { return L"Hop/v2/peer/" + std::wstring(hex.begin(),hex.end()); }
bool identity_matches(const std::string& cert,const std::string& key) {
    return hop_tls_validate_identity(cert.data(),cert.size(),key.data(),key.size())==0;
}
struct SensitiveString { std::string value; ~SensitiveString() { wipe(value); } };
struct SensitiveBytes { std::vector<std::uint8_t> value; ~SensitiveBytes() { wipe(value); } };
struct Generated { std::array<char,8192> cert{}, key{}; std::array<char,65> hex{}; ~Generated() { SecureZeroMemory(key.data(),key.size()); } };
}
CredentialRead WindowsCredentialBackend::read(const std::wstring& name,std::vector<std::uint8_t>& value) {
    wipe(value); PCREDENTIALW credential=nullptr;
    if (!CredReadW(name.c_str(),CRED_TYPE_GENERIC,0,&credential)) return GetLastError()==ERROR_NOT_FOUND ? CredentialRead::missing : CredentialRead::error;
    const auto length=credential->CredentialBlobSize;
    if (!length || length>max_blob || !credential->CredentialBlob) { if (credential->CredentialBlob && length) SecureZeroMemory(credential->CredentialBlob,length); CredFree(credential); return CredentialRead::error; }
    try { value.assign(credential->CredentialBlob,credential->CredentialBlob+length); }
    catch (...) { SecureZeroMemory(credential->CredentialBlob,length); CredFree(credential); return CredentialRead::error; }
    SecureZeroMemory(credential->CredentialBlob,length); CredFree(credential); return CredentialRead::found;
}
bool WindowsCredentialBackend::write(const std::wstring& name,const std::vector<std::uint8_t>& value) {
    if (name.empty() || value.empty() || value.size()>max_blob) return false;
    CREDENTIALW credential{}; credential.Type=CRED_TYPE_GENERIC; credential.TargetName=const_cast<LPWSTR>(name.c_str());
    credential.Persist=CRED_PERSIST_LOCAL_MACHINE; credential.CredentialBlobSize=static_cast<DWORD>(value.size());
    credential.CredentialBlob=const_cast<LPBYTE>(value.data());
    return CredWriteW(&credential,0)!=FALSE;
}
bool WindowsCredentialBackend::erase(const std::wstring& name) {
    if (name.empty()) return false;
    if (CredDeleteW(name.c_str(),CRED_TYPE_GENERIC,0)) return true;
    return GetLastError()==ERROR_NOT_FOUND;
}
void DeviceIdentity::clear() { wipe(private_key); certificate.clear(); fingerprint.clear(); }
DeviceIdentity::~DeviceIdentity() { clear(); }
bool TrustStore::load_or_create(DeviceIdentity& identity) {
    identity.clear(); SensitiveBytes record; const auto result=backend_.read(identity_name,record.value);
    if (result==CredentialRead::error) return false;
    if (result==CredentialRead::found) {
        const auto& b=record.value;
        if (b.size()<8 || b[0]!='H' || b[1]!='P' || b[2]!='I' || b[3]!='1') return false;
        const auto cert_size=(std::size_t(b[4])<<8)|b[5], key_size=(std::size_t(b[6])<<8)|b[7];
        if (!cert_size || !key_size || b.size()!=8+cert_size+key_size) return false;
        identity.certificate.assign(b.begin()+8,b.begin()+8+cert_size);
        identity.private_key.assign(b.begin()+8+cert_size,b.end());
        if (identity.private_key.find('\0')!=std::string::npos || !fingerprint(identity.certificate,identity.fingerprint) || !identity_matches(identity.certificate,identity.private_key)) { identity.clear(); return false; }
        return true;
    }
    Generated generated;
    if (hop_tls_generate_identity(generated.cert.data(),generated.cert.size(),generated.key.data(),generated.key.size(),generated.hex.data(),generated.hex.size()) != 0) return false;
    const std::string cert(generated.cert.data()); SensitiveString secret; secret.value=generated.key.data(); const auto& key=secret.value;
    if (cert.empty() || key.empty() || cert.size()>65535 || key.size()>65535 || cert.size()+key.size()+8>max_blob || !identity_matches(cert,key)) return false;
    auto& b=record.value; b.reserve(8+cert.size()+key.size()); b.insert(b.end(),{'H','P','I','1'});
    b.push_back(static_cast<std::uint8_t>(cert.size()>>8)); b.push_back(static_cast<std::uint8_t>(cert.size()));
    b.push_back(static_cast<std::uint8_t>(key.size()>>8)); b.push_back(static_cast<std::uint8_t>(key.size()));
    b.insert(b.end(),cert.begin(),cert.end()); b.insert(b.end(),key.begin(),key.end());
    if (!backend_.write(identity_name,b)) return false;
    identity.certificate=cert; identity.private_key=key; identity.fingerprint=generated.hex.data(); return true;
}
bool TrustStore::remember(const Session& session,const TlsSocket& socket,const std::string& cert) {
    std::string hex;
    if (!session.pairing_confirmed() || !socket.authenticated() || !fingerprint(cert,hex) || (hex!=socket.pinned_peer_fingerprint() || hex!=session.peer_fingerprint()) || !valid_fingerprint(hex) || cert.size()>max_blob) return false;
    std::vector<std::uint8_t> bytes(cert.begin(),cert.end()); return backend_.write(peer_name(hex),bytes);
}
bool TrustStore::load_peer(const std::string& hex,VerifiedPeerCertificate& peer) {
    peer.pem.clear(); if (!valid_fingerprint(hex)) return false;
    SensitiveBytes bytes; if (backend_.read(peer_name(hex),bytes.value)!=CredentialRead::found) return false;
    std::string pem(bytes.value.begin(),bytes.value.end()),actual;
    if (!fingerprint(pem,actual) || actual!=hex) return false;
    peer.pem=std::move(pem); return true;
}
bool TrustStore::forget(const std::string& hex) { return valid_fingerprint(hex) && backend_.erase(peer_name(hex)); }
}
