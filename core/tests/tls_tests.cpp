#include "hop/secure_channel.h"
#include "hop/secure_channel_c.h"
#include "hop/tls_identity.h"
#include "hop/pair_offer.h"
#include <mbedtls/ssl.h>
#include <algorithm>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {
int checks = 0;
void check(bool value, const char* message) { ++checks; if (!value) { std::cerr << "FAIL " << message << '\n'; std::exit(1); } }
std::string file(const char* path) { std::ifstream f(path, std::ios::binary); check(bool(f), "open test certificate"); return {std::istreambuf_iterator<char>(f), {}}; }
struct Endpoint { std::deque<unsigned char> incoming; Endpoint* other = nullptr; std::vector<unsigned char> sent; };
int send(void* ctx, const unsigned char* data, std::size_t size) {
    auto* e = static_cast<Endpoint*>(ctx); auto n = std::min<std::size_t>(size, 37);
    for (std::size_t i=0;i<n;++i) { e->other->incoming.push_back(data[i]); e->sent.push_back(data[i]); }
    return int(n);
}
int receive(void* ctx, unsigned char* data, std::size_t size) {
    auto* e = static_cast<Endpoint*>(ctx); if (e->incoming.empty()) return MBEDTLS_ERR_SSL_WANT_READ;
    auto n = std::min(size, e->incoming.size());
    for (std::size_t i=0;i<n;++i) { data[i]=e->incoming.front(); e->incoming.pop_front(); }
    return int(n);
}
void exchange(hop::SecureChannel& c, hop::SecureChannel& s, bool expected) {
    for (int i=0;i<20000 && (!c.authenticated() || !s.authenticated());++i) {
        auto a=c.handshake(), b=s.handshake();
        if (a==hop::TlsProgress::failed || b==hop::TlsProgress::failed) { check(!expected, "pinned handshake unexpectedly failed"); return; }
    }
    check(expected && c.authenticated() && s.authenticated(), "both peers authenticate only after handshake");
}
}
int main(int argc, char** argv) {
    check(argc == 7, "six test fixture paths");
    auto aCert=file(argv[1]), aKey=file(argv[2]), bCert=file(argv[3]), bKey=file(argv[4]), badCert=file(argv[5]), badKey=file(argv[6]);
    Endpoint a,b; a.other=&b; b.other=&a;
    hop::SecureChannel client(hop::TlsRole::client,{aCert,aKey,bCert},&a,send,receive);
    hop::SecureChannel server(hop::TlsRole::server,{bCert,bKey,aCert},&b,send,receive);
    const std::string message="private clipboard test payload";
    check(client.write(reinterpret_cast<const std::uint8_t*>(message.data()),message.size())<0, "no data before authentication");
    exchange(client,server,true);
    a.sent.clear();
    int written=client.write(reinterpret_cast<const std::uint8_t*>(message.data()),message.size());
    check(written==int(message.size()), "TLS application write succeeds");
    check(std::search(a.sent.begin(),a.sent.end(),message.begin(),message.end())==a.sent.end(), "wire bytes do not contain plaintext");
    std::vector<std::uint8_t> output(128);
    int n=server.read(output.data(),output.size());
    check(n==int(message.size()) && std::equal(message.begin(),message.end(),output.begin()), "TLS peer receives exact payload");
    Endpoint c,d; c.other=&d; d.other=&c;
    hop::SecureChannel wrongPin(hop::TlsRole::client,{aCert,aKey,badCert},&c,send,receive);
    hop::SecureChannel legitimate(hop::TlsRole::server,{bCert,bKey,aCert},&d,send,receive);
    exchange(wrongPin,legitimate,false);
    check(!wrongPin.authenticated(), "wrong server certificate never authenticates");
    check(wrongPin.write(reinterpret_cast<const std::uint8_t*>(message.data()),message.size())<0, "failed peer cannot send content");
    Endpoint e,f; e.other=&f; f.other=&e;
    hop::SecureChannel attacker(hop::TlsRole::client,{badCert,badKey,bCert},&e,send,receive);
    hop::SecureChannel pinnedServer(hop::TlsRole::server,{bCert,bKey,aCert},&f,send,receive);
    exchange(attacker,pinnedServer,false);
    check(!pinnedServer.authenticated(), "untrusted client never authenticates");
    hop::SecureChannel missing(hop::TlsRole::client,{aCert,aKey,""},&a,send,receive);
    check(missing.handshake()==hop::TlsProgress::failed && !missing.authenticated(), "missing peer pin fails closed");
    hop::SecureChannel chain(hop::TlsRole::client,{aCert,aKey,bCert+badCert},&a,send,receive);
    check(chain.handshake()==hop::TlsProgress::failed, "ambiguous trust bundle rejected");
    check(!hop_tls_create(2,aCert.c_str(),aKey.c_str(),bCert.c_str(),&a,send,receive), "C API rejects unknown role");
    check(hop_tls_want_read_error()==MBEDTLS_ERR_SSL_WANT_READ && hop_tls_want_write_error()==MBEDTLS_ERR_SSL_WANT_WRITE, "C ABI exposes nonblocking status values");
    check(!hop_tls_create(1,aCert.c_str(),aKey.c_str(),"",&a,send,receive), "C API rejects missing peer pin");
    check(hop_tls_handshake(nullptr)==-1 && hop_tls_error(nullptr)==-1, "C API handles null safely");
    Endpoint g,h; g.other=&h; h.other=&g;
    auto* cClient=hop_tls_create(1,aCert.c_str(),aKey.c_str(),bCert.c_str(),&g,send,receive);
    auto* cServer=hop_tls_create(0,bCert.c_str(),bKey.c_str(),aCert.c_str(),&h,send,receive);
    check(cClient && cServer, "C API constructs both peers");
    int cs=0,ss=0;
    for(int i=0;i<20000 && (cs!=1||ss!=1);++i) {
        if(cs!=1)cs=hop_tls_handshake(cClient);
        if(ss!=1)ss=hop_tls_handshake(cServer);
        check(cs>=0 && ss>=0,"C ABI handshake remains valid");
    }
    check(cs==1 && ss==1, "C ABI authenticates both directions");
    check(hop_tls_write(cClient,reinterpret_cast<const std::uint8_t*>(message.data()),message.size())==int(message.size()), "C ABI encrypted write");
    std::fill(output.begin(),output.end(),0);
    check(hop_tls_read(cServer,output.data(),output.size())==int(message.size()) && std::equal(message.begin(),message.end(),output.begin()), "C ABI decrypts exact data");
    check(hop_tls_write(cClient,reinterpret_cast<const std::uint8_t*>(message.data()),message.size())==int(message.size()), "second encrypted write");
    check(!h.incoming.empty(), "encrypted record available for tamper probe");
    h.incoming.back() ^= 1;
    check(hop_tls_read(cServer,output.data(),output.size())<0, "modified encrypted record rejected");
    check(hop_tls_error(cServer)!=0 && hop_tls_write(cServer,reinterpret_cast<const std::uint8_t*>(message.data()),message.size())<0, "fatal TLS error permanently disables channel");
    hop_tls_free(cClient);hop_tls_free(cServer);hop_tls_free(nullptr);
    hop::GeneratedTlsIdentity generatedA,generatedB;
    check(hop::generate_tls_identity(generatedA) && hop::generate_tls_identity(generatedB), "generate independent device identities");
    check(generatedA.fingerprint_sha256.size()==64 && generatedA.fingerprint_sha256!=generatedB.fingerprint_sha256, "identities have distinct SHA-256 fingerprints");
    std::string checkedFingerprint;
    check(hop::tls_certificate_fingerprint(generatedA.certificate_pem,checkedFingerprint) && checkedFingerprint==generatedA.fingerprint_sha256, "fingerprint identifies exact certificate");
    check(!hop::tls_certificate_fingerprint(generatedA.certificate_pem+generatedB.certificate_pem,checkedFingerprint), "multiple certificates cannot masquerade as one identity");
    check(hop::validate_tls_identity(generatedA.certificate_pem,generatedA.private_key_pem), "generated certificate and private key match");
    check(!hop::validate_tls_identity(generatedA.certificate_pem,generatedB.private_key_pem), "mismatched private key rejected");
    check(hop_tls_validate_identity(generatedA.certificate_pem.data(),generatedA.certificate_pem.size(),generatedA.private_key_pem.data(),generatedA.private_key_pem.size())==0, "C ABI validates matching identity");
    check(hop_tls_validate_identity(generatedA.certificate_pem.data(),generatedA.certificate_pem.size(),generatedB.private_key_pem.data(),generatedB.private_key_pem.size())==-1, "C ABI rejects mismatched key");
    std::array<std::uint8_t,16> nonce{}; check(hop_tls_random_bytes(nonce.data(),nonce.size())==0 && !std::all_of(nonce.begin(),nonce.end(),[](auto b){return b==0;}), "cryptographic pairing nonce generated");
    check(hop_tls_random_bytes(nullptr,16)==-1 && hop_tls_random_bytes(nonce.data(),0)==-1, "invalid random request rejected");
    hop::PairOffer candidate; candidate.role=1; candidate.nonce=nonce; candidate.name="Mac 测试"; candidate.certificate_pem=generatedA.certificate_pem;
    auto offerBytes=hop::encode_pair_offer(candidate); check(!offerBytes.empty(), "encode bounded public pairing offer");
    hop::PairOfferDecoder offerDecoder;
    for (auto byte:offerBytes) check(offerDecoder.feed(&byte,1), "fragmented pairing offer");
    auto decodedOffer=offerDecoder.finish();
    check(decodedOffer && decodedOffer->name==candidate.name && decodedOffer->certificate_pem==candidate.certificate_pem && decodedOffer->nonce==candidate.nonce, "pair offer roundtrip");
    check(!offerDecoder.finish(), "pair offer consumed once");
    auto malformed=offerBytes; malformed[6]=0xff; hop::PairOfferDecoder hugeOffer;
    check(!hugeOffer.feed(malformed.data(),malformed.size()), "oversize pairing name rejected");
    hop::PairOfferDecoder truncatedOffer; check(truncatedOffer.feed(offerBytes.data(),offerBytes.size()-1) && !truncatedOffer.finish(), "truncated pairing offer rejected");
    auto extra=offerBytes; extra.push_back(0); hop::PairOfferDecoder extraOffer;
    check(!extraOffer.feed(extra.data(),extra.size()), "trailing pairing data rejected");
    candidate.name=std::string(1,char(0xff)); check(hop::encode_pair_offer(candidate).empty(), "invalid UTF-8 device name rejected");
    candidate.name="Mac\xE2\x80\xAE"; check(hop::encode_pair_offer(candidate).empty(), "bidi override in untrusted name rejected");
    candidate.name="Mac"; candidate.certificate_pem+=generatedB.certificate_pem; check(hop::encode_pair_offer(candidate).empty(), "multiple candidate certificates rejected");
    auto selector=hop::encode_peer_selector(generatedA.fingerprint_sha256);
    check(selector.size()==68 && hop::decode_peer_selector(selector.data(),selector.size())==generatedA.fingerprint_sha256, "public selector roundtrip");
    selector.back()='z'; check(!hop::decode_peer_selector(selector.data(),selector.size()), "invalid selector rejected");
    check(hop::encode_peer_selector("short").empty(), "short fingerprint rejected");
    Endpoint x,y;x.other=&y;y.other=&x;
    hop::SecureChannel wrongKey(hop::TlsRole::client,{generatedA.certificate_pem,generatedB.private_key_pem,generatedB.certificate_pem},&x,send,receive);
    check(wrongKey.error()!=0, "channel rejects mismatched local identity before handshake");
    hop::SecureChannel generatedClient(hop::TlsRole::client,{generatedA.certificate_pem,generatedA.private_key_pem,generatedB.certificate_pem},&x,send,receive);
    hop::SecureChannel generatedServer(hop::TlsRole::server,{generatedB.certificate_pem,generatedB.private_key_pem,generatedA.certificate_pem},&y,send,receive);
    exchange(generatedClient,generatedServer,true);
    char certBuffer[8192],keyBuffer[8192],fingerprintBuffer[65];
    char computedFingerprint[65];
    check(hop_tls_certificate_fingerprint(generatedA.certificate_pem.data(),generatedA.certificate_pem.size(),computedFingerprint,sizeof computedFingerprint)==0 &&
          generatedA.fingerprint_sha256==computedFingerprint, "C ABI fingerprint matches generated certificate");
    check(hop_tls_certificate_fingerprint(generatedA.certificate_pem.data(),generatedA.certificate_pem.size(),computedFingerprint,64)==-1 && computedFingerprint[0]=='\0', "C ABI rejects small fingerprint output");
    check(hop_tls_certificate_fingerprint("bad",3,computedFingerprint,sizeof computedFingerprint)==-1 && computedFingerprint[0]=='\0', "C ABI rejects invalid certificate");
    check(hop_tls_generate_identity(certBuffer,sizeof certBuffer,keyBuffer,sizeof keyBuffer,fingerprintBuffer,sizeof fingerprintBuffer)==0, "C ABI generates device identity");
    check(hop::tls_certificate_fingerprint(certBuffer,checkedFingerprint) && checkedFingerprint==fingerprintBuffer, "C ABI fingerprint matches certificate");
    char tooSmall[1]={'x'};
    check(hop_tls_generate_identity(tooSmall,sizeof tooSmall,keyBuffer,sizeof keyBuffer,fingerprintBuffer,sizeof fingerprintBuffer)==-1 && keyBuffer[0]=='\0', "too-small buffer fails without returning private key");
    std::cout << "TLS: " << checks << " checks passed; in-memory fake identities only\n";
}
