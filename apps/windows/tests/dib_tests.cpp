#include "hop_windows/dib.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>
using namespace hop::windows;
int n=0;
void check(bool b, const char* text, int line) { ++n; if(!b){std::cerr<<line<<": "<<text<<'\n';std::exit(1);} }
#define CHECK(x) check(bool(x),#x,__LINE__)
void put32(std::vector<std::uint8_t>& p,std::size_t i,std::uint32_t n){for(int k=0;k<4;++k)p[i+k]=(n>>(8*k))&255;}
int main(){
    BgraImage input{2,2,{1,2,3,4, 5,6,7,8, 9,10,11,12, 13,14,15,16}}, decoded;
    std::vector<std::uint8_t> v5;
    CHECK(encode_dibv5(input,v5)); CHECK(v5.size()==140);
    CHECK(decode_dib(v5.data(),v5.size(),decoded));
    CHECK(decoded.width==2 && decoded.height==2 && decoded.pixels==input.pixels);
    auto bad=v5; bad[42]=0; CHECK(!decode_dib(bad.data(),bad.size(),decoded));
    bad=v5; put32(bad,112,124); put32(bad,116,8); CHECK(!decode_dib(bad.data(),bad.size(),decoded));
    bad=v5; put32(bad,56,0); CHECK(!decode_dib(bad.data(),bad.size(),decoded));
    bad=v5; bad.resize(139); CHECK(!decode_dib(bad.data(),bad.size(),decoded));
    bad=v5; put32(bad,4,0); CHECK(!decode_dib(bad.data(),bad.size(),decoded));
    bad=v5; put32(bad,8,0x80000000); CHECK(!decode_dib(bad.data(),bad.size(),decoded));
    bad=v5; put32(bad,16,4); CHECK(!decode_dib(bad.data(),bad.size(),decoded));
    bad=v5; put32(bad,32,1); CHECK(!decode_dib(bad.data(),bad.size(),decoded));
    CHECK(!decode_dib(nullptr,0,decoded));
    BgraImage invalid=input; invalid.pixels.pop_back(); CHECK(!encode_dibv5(invalid,bad));
    invalid=input; invalid.width=0; CHECK(!encode_dibv5(invalid,bad));
    // 1x2, 24-bit bottom-up, each row padded to 4 bytes.
    std::vector<std::uint8_t> rgb(48); put32(rgb,0,40); put32(rgb,4,1); put32(rgb,8,2);
    rgb[12]=1; rgb[14]=24;
    rgb[40]=4;rgb[41]=5;rgb[42]=6; rgb[44]=1;rgb[45]=2;rgb[46]=3;
    CHECK(decode_dib(rgb.data(),rgb.size(),decoded));
    CHECK(decoded.pixels==std::vector<std::uint8_t>({1,2,3,255,4,5,6,255}));
    put32(rgb,8,static_cast<std::uint32_t>(-2)); CHECK(decode_dib(rgb.data(),rgb.size(),decoded));
    CHECK(decoded.pixels==std::vector<std::uint8_t>({4,5,6,255,1,2,3,255}));
    std::cout<<"PASS "<<n<<" DIB layout checks; in-memory fixtures only\n";
}
