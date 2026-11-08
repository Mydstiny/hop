#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace hop {
constexpr size_t kBodySize = 128, kFrameSize = 132;
enum class Type : uint8_t { Hello=1, Enter=2, Leave=3, RequestLeave=4, Move=5, Button=6, Wheel=7, Key=8, Keepalive=9, Bye=10 };
enum class Phase : uint8_t { Resume=0, Pair=1, Challenge=2, Confirm=3, Ready=4, Commit=5, Reject=6 };
inline uint16_t U16(const uint8_t* p) { return uint16_t(p[0])<<8 | p[1]; }
inline uint32_t U32(const uint8_t* p) { return uint32_t(p[0])<<24 | uint32_t(p[1])<<16 | uint32_t(p[2])<<8 | p[3]; }
inline uint64_t U64(const uint8_t* p) { return uint64_t(U32(p))<<32 | U32(p+4); }
inline void Put16(uint8_t* p, uint16_t n) { p[0]=uint8_t(n>>8); p[1]=uint8_t(n); }
inline void Put32(uint8_t* p, uint32_t n) { for(int i=3;i>=0;--i) { p[i]=uint8_t(n); n>>=8; } }
inline void Put64(uint8_t* p, uint64_t n) { Put32(p,uint32_t(n>>32)); Put32(p+4,uint32_t(n)); }
inline bool Zero(const uint8_t* p, size_t n) { uint8_t v=0; for(size_t i=0;i<n;++i) v|=p[i]; return v==0; }
inline bool ConstantEqual(const uint8_t* a,const uint8_t* b,size_t n) { uint8_t v=0; for(size_t i=0;i<n;++i) v|=a[i]^b[i]; return v==0; }
inline void Require(bool ok,const char* reason) { if(!ok) throw std::runtime_error(reason); }
inline bool Utf8Name(const uint8_t* p,size_t n) {
    size_t i=0; while(i<n && p[i]) {
        uint8_t c=p[i++]; if(c<0x80) continue;
        int more= c>=0xC2&&c<=0xDF ? 1 : c>=0xE0&&c<=0xEF ? 2 : c>=0xF0&&c<=0xF4 ? 3 : -1;
        if(more<0||i+more>n) return false;
        uint8_t first=p[i]; if((c==0xE0&&first<0xA0)||(c==0xED&&first>=0xA0)||(c==0xF0&&first<0x90)||(c==0xF4&&first>=0x90)) return false;
        while(more--) if((p[i++]&0xC0)!=0x80) return false;
    }
    return i<n && Zero(p+i,n-i);
}
struct Message {
    std::array<uint8_t,kBodySize> bytes{};
    explicit Message(Type t,uint32_t epoch=0) { bytes[0]=1; bytes[1]=uint8_t(t); Put32(bytes.data()+4,epoch); }
    Type type() const { return Type(bytes[1]); }
    uint32_t epoch() const { return U32(bytes.data()+4); }
    uint8_t* data() { return bytes.data()+8; }
    const uint8_t* data() const { return bytes.data()+8; }
    std::array<uint8_t,kFrameSize> frame() const { std::array<uint8_t,kFrameSize> f{}; Put32(f.data(),kBodySize); std::copy(bytes.begin(),bytes.end(),f.begin()+4); return f; }
    void validate() const {
        Require(bytes[0]==1&&bytes[1]>=1&&bytes[1]<=10,"version/type");
        Require(bytes[3]==0&&(type()==Type::Hello ? bytes[2]<=1 : bytes[2]==0),"flags/reserved");
        bool zeroEpoch=type()==Type::Hello||type()==Type::Keepalive||type()==Type::Bye;
        Require(zeroEpoch ? epoch()==0 : epoch()!=0,"epoch");
        const auto* p=data(); size_t used=0;
        switch(type()) {
        case Type::Hello: {
            Require(p[0]<=6&&(p[1]==1||p[1]==2)&&p[2]<=1&&p[3]==0,"hello enum");
            uint32_t code=U32(p+4), w=U32(p+8), h=U32(p+12);
            Require((code==0||(code>=100000&&code<=999999))&&w>=1&&w<=65535&&h>=1&&h<=65535,"hello range");
            Require(!Zero(p+16,16)&&!Zero(p+32,16)&&Utf8Name(p+80,32),"identity/nonce/name");
            bool needsCode=p[0]==2||p[0]==3||p[0]==5;
            Require(needsCode ? code>=100000 : ((p[0]==0||p[0]==1) ? code==0 : true),"phase code");
            Require(bytes[2]==(p[1]==1 ? 1 : 0),"HELLO flags/role");
            Require(p[1]==1 ? (p[0]==2||p[0]==4||p[0]==5||p[0]==6) : (p[0]==0||p[0]==1||p[0]==3||p[0]==5),"HELLO phase/role");
            bool token=p[0]==0||p[0]==4||p[0]==5;
            Require(token ? !Zero(p+48,32) : Zero(p+48,32),"phase token"); used=112; break;
        }
        case Type::Enter: Require((U16(p+4)&~0x1FF)==0&&(U16(p+6)&~31)==0,"enter masks"); used=8; break;
        case Type::Leave: Require(p[4]<=2,"leave reason"); used=5; break;
        case Type::RequestLeave: case Type::Move: used=4; break;
        case Type::Button: Require(p[0]>=1&&p[0]<=5&&p[1]<=1&&(U16(p+6)&~31)==0,"button fields"); Require(bool(U16(p+6)&(1<<(p[0]-1)))==bool(p[1]),"button state"); used=8; break;
        case Type::Wheel: { int64_t x=int32_t(U32(p)),y=int32_t(U32(p+4)); Require(x>=-120000&&x<=120000&&y>=-120000&&y<=120000,"wheel range"); used=8; break; }
        case Type::Key: Require(U16(p)<128&&(U16(p+2)&~0x1FF)==0&&p[4]<=1&&p[5]<=1&&(p[4]||p[5]==0),"key fields"); used=6; break;
        case Type::Keepalive: used=8; break;
        case Type::Bye: Require(p[0]<=4,"bye reason"); used=1; break;
        }
        Require(Zero(p+used,120-used),"reserved payload");
    }
};
class Decoder {
    std::array<uint8_t,kFrameSize> pending_{}; size_t count_=0;
public:
    template<class F> void feed(const uint8_t* p,size_t n,F accept) {
        while(n) {
            size_t take=std::min(n,kFrameSize-count_); std::copy(p,p+take,pending_.begin()+count_); count_+=take;p+=take;n-=take;
            if(count_>=4) Require(U32(pending_.data())==kBodySize,"frame length");
            if(count_==kFrameSize) { Message m(Type::Hello); std::copy(pending_.begin()+4,pending_.end(),m.bytes.begin());count_=0;m.validate();accept(m); }
        }
    }
    bool partial() const { return count_!=0; }
};
inline std::string Hex(const uint8_t* p,size_t n) { static const char* c="0123456789abcdef"; std::string s; s.reserve(n*2);for(size_t i=0;i<n;++i) {s+=c[p[i]>>4];s+=c[p[i]&15];}return s; }
inline std::vector<uint8_t> Unhex(const std::string& s,size_t n) { Require(s.size()==2*n,"hex length"); std::vector<uint8_t> b(n);for(size_t i=0;i<n;++i) {auto digit=[](char c)->int { return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;};int a=digit(s[i*2]),d=digit(s[i*2+1]);Require(a>=0&&d>=0,"hex format");b[i]=uint8_t(a*16+d);}return b; }
} // namespace hop
