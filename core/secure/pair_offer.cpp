#include "hop/pair_offer.h"
#include "hop/tls_identity.h"
#include <algorithm>
#include <cstring>
namespace hop {
namespace {
constexpr std::size_t header_size=28, max_name=128, max_cert=16384, max_offer=header_size+max_name+max_cert;
std::uint16_t get16(const std::uint8_t* p) { return (std::uint16_t(p[0])<<8)|p[1]; }
std::uint32_t get32(const std::uint8_t* p) { return (std::uint32_t(p[0])<<24)|(std::uint32_t(p[1])<<16)|(std::uint32_t(p[2])<<8)|p[3]; }
void put16(std::uint8_t* p,std::uint16_t n) { p[0]=std::uint8_t(n>>8);p[1]=std::uint8_t(n); }
void put32(std::uint8_t* p,std::uint32_t n) { for(int i=3;i>=0;--i) { p[i]=std::uint8_t(n); n>>=8; } }
bool name_ok(const std::string& name) {
    if (name.empty() || name.size()>max_name) return false;
    const auto* p=reinterpret_cast<const unsigned char*>(name.data()); std::size_t i=0;
    while(i<name.size()) {
        const auto c=p[i++];
        if(c<0x80) { if(c<0x20 || c==0x7f) return false; continue; }
        const int more=c>=0xc2&&c<=0xdf?1:c>=0xe0&&c<=0xef?2:c>=0xf0&&c<=0xf4?3:-1;
        if(more<0 || i+std::size_t(more)>name.size()) return false;
        const auto first=p[i];
        if((c==0xe0&&first<0xa0)||(c==0xed&&first>=0xa0)||(c==0xf0&&first<0x90)||(c==0xf4&&first>=0x90)) return false;
        std::uint32_t cp = c & (more==1 ? 0x1f : more==2 ? 0x0f : 0x07);
        for(int j=0;j<more;++j) { const auto next=p[i++]; if((next&0xc0)!=0x80) return false; cp=(cp<<6)|(next&0x3f); }
        // Candidate names are untrusted; reject control and bidi overrides.
        if ((cp>=0x80&&cp<=0x9f) || cp==0x061c || cp==0x200e || cp==0x200f ||
            (cp>=0x2028&&cp<=0x202e) || (cp>=0x2066&&cp<=0x2069)) return false;
    }
    return true;
}
bool nonce_ok(const std::array<std::uint8_t,16>& nonce) { return std::any_of(nonce.begin(),nonce.end(),[](std::uint8_t b){return b!=0;}); }
bool valid(const PairOffer& offer) {
    std::string fingerprint;
    return (offer.role==1||offer.role==2) && nonce_ok(offer.nonce) && name_ok(offer.name) &&
        !offer.certificate_pem.empty() && offer.certificate_pem.size()<=max_cert &&
        tls_certificate_fingerprint(offer.certificate_pem,fingerprint);
}
bool hex64(const std::string& s) {
    return s.size()==64 && std::all_of(s.begin(),s.end(),[](char c){return (c>='0'&&c<='9')||(c>='A'&&c<='F');});
}
}
std::vector<std::uint8_t> encode_pair_offer(const PairOffer& offer) {
    if (!valid(offer)) return {};
    std::vector<std::uint8_t> out(header_size+offer.name.size()+offer.certificate_pem.size());
    out[0]='H';out[1]='P';out[2]='O';out[3]='P';out[4]=1;out[5]=offer.role;
    put16(out.data()+6,static_cast<std::uint16_t>(offer.name.size()));put32(out.data()+8,static_cast<std::uint32_t>(offer.certificate_pem.size()));
    std::copy(offer.nonce.begin(),offer.nonce.end(),out.begin()+12);
    std::copy(offer.name.begin(),offer.name.end(),out.begin()+header_size);
    std::copy(offer.certificate_pem.begin(),offer.certificate_pem.end(),out.begin()+header_size+offer.name.size());
    return out;
}
bool PairOfferDecoder::feed(const std::uint8_t* data,std::size_t count) {
    if(failed_||finished_||(!data&&count)||count>max_offer-bytes_.size()) {failed_=true;return false;}
    try {
        if(count) bytes_.insert(bytes_.end(),data,data+count);
        if(bytes_.size()>=header_size && !expected_) {
            const auto* p=bytes_.data(); const auto name=get16(p+6); const auto cert=get32(p+8);
            if(std::memcmp(p,"HPOP",4)!=0||p[4]!=1||(p[5]!=1&&p[5]!=2)||!name||name>max_name||!cert||cert>max_cert) {failed_=true;return false;}
            expected_=header_size+name+cert;
        }
        if(expected_ && bytes_.size()>expected_) {failed_=true;return false;}
        return true;
    } catch (...) {failed_=true;return false;}
}
std::optional<PairOffer> PairOfferDecoder::finish() {
    if(failed_||finished_||!expected_||bytes_.size()!=expected_) {failed_=true;return std::nullopt;}
    finished_=true;
    try {
        const auto* p=bytes_.data(); const auto name=get16(p+6);
        PairOffer result; result.role=p[5]; std::copy(p+12,p+28,result.nonce.begin());
        result.name.assign(reinterpret_cast<const char*>(p+header_size),name);
        result.certificate_pem.assign(reinterpret_cast<const char*>(p+header_size+name),bytes_.size()-header_size-name);
        if(!valid(result)) {failed_=true;return std::nullopt;}
        return result;
    } catch (...) {failed_=true;return std::nullopt;}
}
std::vector<std::uint8_t> encode_peer_selector(const std::string& fingerprint) {
    if(!hex64(fingerprint)) return {};
    std::vector<std::uint8_t> out={'H','P','S','L'}; out.insert(out.end(),fingerprint.begin(),fingerprint.end());return out;
}
std::optional<std::string> decode_peer_selector(const std::uint8_t* data,std::size_t count) {
    if(!data||count!=68||std::memcmp(data,"HPSL",4)!=0) return std::nullopt;
    std::string value(reinterpret_cast<const char*>(data+4),64);
    return hex64(value)?std::optional<std::string>(value):std::nullopt;
}
}
