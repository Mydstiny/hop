#include "hop_windows/pointer_settings.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

namespace hop::windows {
namespace {
constexpr std::size_t legacy_record_size=29, record_size=31; // HPT2 adds u16 motion rate
bool valid_peer(const DeviceFingerprint& id) { return std::any_of(id.begin(),id.end(),[](std::uint8_t b){return b!=0;}); }
void put_double(std::vector<std::uint8_t>& bytes, std::size_t offset, double value) {
    static_assert(sizeof(double)==8 && std::numeric_limits<double>::is_iec559, "IEEE754 binary64 required");
    std::uint64_t bits; std::memcpy(&bits,&value,sizeof(bits));
    for (int i=0;i<8;++i) bytes[offset+i]=static_cast<std::uint8_t>((bits >> (8*i)) & 0xff);
}
double get_double(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint64_t bits=0; for(int i=0;i<8;++i) bits|=std::uint64_t(bytes[offset+i]) << (8*i);
    double value; std::memcpy(&value,&bits,sizeof(value)); return value;
}
std::vector<std::uint8_t> encode(const hop::PointerSettings& value,std::uint16_t rate) {
    std::vector<std::uint8_t> bytes(record_size);
    bytes[0]='H';bytes[1]='P';bytes[2]='T';bytes[3]='2';
    put_double(bytes,4,value.movement_scale); put_double(bytes,12,value.horizontal_scroll_scale); put_double(bytes,20,value.vertical_scroll_scale);
    bytes[28]=(value.reverse_horizontal ? 1 : 0) | (value.reverse_vertical ? 2 : 0);
    bytes[29]=static_cast<std::uint8_t>(rate>>8);bytes[30]=static_cast<std::uint8_t>(rate);
    return bytes;
}
bool decode(const std::vector<std::uint8_t>& bytes, hop::PointerSettings& value,std::uint16_t& rate) {
    const bool legacy=bytes.size()==legacy_record_size && bytes[3]=='1';
    const bool current=bytes.size()==record_size && bytes[3]=='2';
    if ((!legacy && !current) || bytes[0]!='H' || bytes[1]!='P' || bytes[2]!='T' || (bytes[28] & ~3)) return false;
    hop::PointerSettings candidate;
    candidate.movement_scale=get_double(bytes,4); candidate.horizontal_scroll_scale=get_double(bytes,12); candidate.vertical_scroll_scale=get_double(bytes,20);
    candidate.reverse_horizontal=(bytes[28]&1)!=0; candidate.reverse_vertical=(bytes[28]&2)!=0;
    if (!hop::valid_pointer_settings(candidate)) return false;
    const auto candidate_rate=legacy ? std::uint16_t(240) : static_cast<std::uint16_t>((std::uint16_t(bytes[29])<<8)|bytes[30]);
    if(candidate_rate<20 || candidate_rate>1000)return false;
    value=candidate;rate=candidate_rate; return true;
}
std::wstring registry_path(const DeviceFingerprint& peer) {
    const wchar_t hex[]=L"0123456789ABCDEF";
    std::wstring out=L"Software\\Hop\\PointerSettings\\";
    for (auto byte:peer) { out.push_back(hex[byte>>4]); out.push_back(hex[byte&15]); }
    return out;
}
struct Key { HKEY value=nullptr; ~Key(){if(value)RegCloseKey(value);} };
}
bool parse_device_fingerprint(const std::string& hex, DeviceFingerprint& out) {
    if (hex.size()!=64) return false;
    const auto nibble=[](char c)->int {
        if(c>='0' && c<='9') return c-'0';
        if(c>='A' && c<='F') return c-'A'+10;
        if(c>='a' && c<='f') return c-'a'+10;
        return -1;
    };
    DeviceFingerprint candidate{};
    for(std::size_t i=0;i<candidate.size();++i) {
        const auto hi=nibble(hex[i*2]), lo=nibble(hex[i*2+1]);
        if(hi<0 || lo<0) return false;
        candidate[i]=static_cast<std::uint8_t>((hi<<4)|lo);
    }
    if(!valid_peer(candidate)) return false;
    out=candidate; return true;
}
bool RegistryPointerSettingsStore::load(const DeviceFingerprint& peer, std::vector<std::uint8_t>& bytes, bool& found) {
    found=false; if(!valid_peer(peer)) return false;
    Key key; const auto path=registry_path(peer);
    const auto rc=RegOpenKeyExW(HKEY_CURRENT_USER,path.c_str(),0,KEY_QUERY_VALUE,&key.value);
    if(rc==ERROR_FILE_NOT_FOUND) return true;
    if(rc!=ERROR_SUCCESS) return false;
    DWORD type=0, size=0;
    auto status=RegQueryValueExW(key.value,L"Settings",nullptr,&type,nullptr,&size);
    if(status==ERROR_FILE_NOT_FOUND) return true;
    if(status!=ERROR_SUCCESS || type!=REG_BINARY || (size!=record_size && size!=legacy_record_size)) return false;
    std::vector<std::uint8_t> data(size);
    status=RegQueryValueExW(key.value,L"Settings",nullptr,&type,data.data(),&size);
    if(status!=ERROR_SUCCESS || type!=REG_BINARY || size!=data.size()) return false;
    bytes=std::move(data); found=true; return true;
}
bool RegistryPointerSettingsStore::save(const DeviceFingerprint& peer, const std::vector<std::uint8_t>& bytes) {
    if(!valid_peer(peer) || bytes.size()!=record_size) return false;
    Key key; const auto path=registry_path(peer);
    if(RegCreateKeyExW(HKEY_CURRENT_USER,path.c_str(),0,nullptr,0,KEY_SET_VALUE,nullptr,&key.value,nullptr)!=ERROR_SUCCESS) return false;
    return RegSetValueExW(key.value,L"Settings",0,REG_BINARY,bytes.data(),static_cast<DWORD>(bytes.size()))==ERROR_SUCCESS;
}
bool TargetPointerSettings::select(const DeviceFingerprint& peer) {
    clear();
    if (!valid_peer(peer)) return false;
    std::vector<std::uint8_t> bytes; bool found=false;
    try { if (!store_.load(peer,bytes,found)) return false; } catch (...) { return false; }
    hop::PointerSettings value;std::uint16_t rate=240;
    if (found && !decode(bytes,value,rate)) return false;
    peer_=peer; current_=value; rate_=rate; selected_=true; publish(true,current_,rate_); return true;
}
bool TargetPointerSettings::update(const DeviceFingerprint& peer, const hop::PointerSettings& value) {
    if(!valid_peer(peer) || !hop::valid_pointer_settings(value)) return false;
    try {
        std::vector<std::uint8_t> old;bool found=false;hop::PointerSettings prior;std::uint16_t rate=240;
        if(!store_.load(peer,old,found) || (found && !decode(old,prior,rate)) || !store_.save(peer,encode(value,rate)))return false;
        if(selected_ && peer_==peer) {current_=value;rate_=rate;publish(true,current_,rate_);}return true;
    } catch (...) {return false;}
}
bool TargetPointerSettings::update_motion_rate(const DeviceFingerprint& peer,std::uint16_t rate) {
    if(!valid_peer(peer) || rate<20 || rate>1000)return false;
    try {
        std::vector<std::uint8_t> old;bool found=false;hop::PointerSettings value;std::uint16_t prior=240;
        if(!store_.load(peer,old,found) || (found && !decode(old,value,prior)) || !store_.save(peer,encode(value,rate)))return false;
        if(selected_ && peer_==peer) {current_=value;rate_=rate;publish(true,current_,rate_);}return true;
    } catch (...) {return false;}
}
void TargetPointerSettings::publish(bool selected, const hop::PointerSettings& value,std::uint16_t rate) {
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "lock-free 64-bit snapshot required");
    const auto scale=[](double d) { return static_cast<std::uint64_t>(std::llround(d*10000)); };
    const auto packed=scale(value.movement_scale) | (scale(value.horizontal_scroll_scale)<<17) |
                      (scale(value.vertical_scroll_scale)<<34) | (std::uint64_t(value.reverse_horizontal)<<51) |
                      (std::uint64_t(value.reverse_vertical)<<52) | (std::uint64_t(selected)<<53) | (std::uint64_t(rate)<<54);
    snapshot_bits_.store(packed,std::memory_order_release);
}
void TargetPointerSettings::clear() { selected_=false; publish(false,hop::PointerSettings{},240); }
std::optional<hop::PointerSettings> TargetPointerSettings::snapshot() const {
    const auto packed=snapshot_bits_.load(std::memory_order_acquire);
    if(!(packed & (std::uint64_t(1)<<53))) return std::nullopt;
    constexpr std::uint64_t mask=(std::uint64_t(1)<<17)-1;
    hop::PointerSettings result;
    result.movement_scale=double(packed&mask)/10000;
    result.horizontal_scroll_scale=double((packed>>17)&mask)/10000;
    result.vertical_scroll_scale=double((packed>>34)&mask)/10000;
    result.reverse_horizontal=(packed & (std::uint64_t(1)<<51))!=0;
    result.reverse_vertical=(packed & (std::uint64_t(1)<<52))!=0;
    return hop::valid_pointer_settings(result) ? std::optional<hop::PointerSettings>{result} : std::nullopt;
}
std::optional<hop::PointerSettings> TargetPointerSettings::current() const { return snapshot(); }
std::optional<std::uint16_t> TargetPointerSettings::motion_rate() const {
    const auto packed=snapshot_bits_.load(std::memory_order_acquire);if(!(packed & (std::uint64_t(1)<<53)))return std::nullopt;
    const auto rate=static_cast<std::uint16_t>((packed>>54)&1023);return rate>=20 && rate<=1000 ? std::optional<std::uint16_t>{rate} : std::nullopt;
}
std::optional<hop::Delta> TargetPointerSettings::motion(hop::Delta input) const { const auto s=snapshot(); return s ? hop::map_motion(*s,input) : std::nullopt; }
std::optional<hop::Delta> TargetPointerSettings::scroll(hop::Delta input) const { const auto s=snapshot(); return s ? hop::map_scroll(*s,input) : std::nullopt; }
} // namespace hop::windows
