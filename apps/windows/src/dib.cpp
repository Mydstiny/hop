#include "hop_windows/dib.h"
#include "hop_windows/clipboard.h"
#include <algorithm>
#include <limits>
#include <utility>

namespace hop::windows {
namespace {
std::uint16_t le16(const std::uint8_t* p) { return std::uint16_t(p[0]) | (std::uint16_t(p[1]) << 8); }
std::uint32_t le32(const std::uint8_t* p) { return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24); }
void put16(std::vector<std::uint8_t>& p, std::size_t i, std::uint16_t n) { p[i]=n & 255; p[i+1]=(n >> 8) & 255; }
void put32(std::vector<std::uint8_t>& p, std::size_t i, std::uint32_t n) { for (int k=0;k<4;++k) p[i+k]=(n >> (8*k)) & 255; }
bool dimensions(std::uint32_t w, std::uint32_t h) {
    return w && h && w<=16384 && h<=16384 && std::uint64_t(w)*h<=max_png_pixels;
}
}
bool decode_dib(const std::uint8_t* data, std::size_t size, BgraImage& image) {
    if (!data || size < 40) return false;
    const auto header = le32(data);
    if ((header!=40 && header!=108 && header!=124) || size < header) return false;
    const auto w_signed = static_cast<std::int32_t>(le32(data+4));
    const auto h_signed = static_cast<std::int32_t>(le32(data+8));
    if (w_signed <= 0 || h_signed == 0 || h_signed == std::numeric_limits<std::int32_t>::min()) return false;
    const auto w=static_cast<std::uint32_t>(w_signed), h=static_cast<std::uint32_t>(h_signed<0 ? -h_signed : h_signed);
    if (!dimensions(w,h) || le16(data+12)!=1 || le32(data+32)!=0) return false;
    const auto bits=le16(data+14); const auto compression=le32(data+16);
    if ((bits!=24 && bits!=32) || (compression!=0 && compression!=3) || (compression==3 && bits!=32)) return false;
    std::size_t pixels_offset=header; bool alpha=false;
    if (compression==3) {
        if (header==40) { if (size<52) return false; pixels_offset=52; }
        const auto red=le32(data+40), green=le32(data+44), blue=le32(data+48);
        const auto amask=header>=108 ? le32(data+52) : 0;
        if (red!=0x00ff0000 || green!=0x0000ff00 || blue!=0x000000ff || (amask!=0 && amask!=0xff000000)) return false;
        alpha=amask==0xff000000;
    }
    // Do not dereference arbitrary profile paths or misinterpret profile bytes
    // as image pixels; color-managed DIBs need a separate verified conversion.
    if (header==124 && (le32(data+112)!=0 || le32(data+116)!=0)) return false;
    if (header>=108 && le32(data+56)!=0x73524742 && le32(data+56)!=0x57696e20) return false;
    const auto stride=((std::uint64_t(w)*bits+31)/32)*4;
    const auto bytes=stride*h;
    if (pixels_offset>size || bytes>size-pixels_offset) return false;
    BgraImage result; result.width=w; result.height=h;
    try { result.pixels.resize(static_cast<std::size_t>(std::uint64_t(w)*h*4)); }
    catch (...) { return false; }
    for (std::uint32_t y=0;y<h;++y) {
        const auto src_y=h_signed>0 ? h-1-y : y;
        const auto* src=data+pixels_offset+src_y*stride;
        auto* dst=result.pixels.data()+std::size_t(y)*w*4;
        for (std::uint32_t x=0;x<w;++x) {
            const auto* pixel=src+std::size_t(x)*(bits/8);
            dst[x*4]=pixel[0]; dst[x*4+1]=pixel[1]; dst[x*4+2]=pixel[2]; dst[x*4+3]=alpha ? pixel[3] : 255;
        }
    }
    image=std::move(result); return true;
}
bool encode_dibv5(const BgraImage& image, std::vector<std::uint8_t>& dib) {
    if (!dimensions(image.width,image.height) || image.pixels.size()!=std::uint64_t(image.width)*image.height*4) return false;
    std::vector<std::uint8_t> result;
    try { result.resize(124+image.pixels.size()); } catch (...) { return false; }
    put32(result,0,124); put32(result,4,image.width); put32(result,8,std::uint32_t(-static_cast<std::int32_t>(image.height)));
    put16(result,12,1); put16(result,14,32); put32(result,16,3); put32(result,20,static_cast<std::uint32_t>(image.pixels.size()));
    put32(result,40,0x00ff0000); put32(result,44,0x0000ff00); put32(result,48,0x000000ff); put32(result,52,0xff000000);
    put32(result,56,0x73524742); // LCS_sRGB
    std::copy(image.pixels.begin(),image.pixels.end(),result.begin()+124);
    dib=std::move(result); return true;
}
} // namespace hop::windows
