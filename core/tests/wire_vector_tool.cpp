#include "hop/frame.h"
#include "hop/wire_validation.h"
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
namespace {
int digit(char c) { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; return -1; }
}
int main(int argc, char** argv) {
    if (argc != 5) return 2;
    hop::Frame f{static_cast<hop::FrameKind>(std::strtoul(argv[1],nullptr,10)),std::strtoull(argv[2],nullptr,10),std::strtoull(argv[3],nullptr,10),{}};
    std::string hex=argv[4]; if (hex.size()%2) return 2;
    for (std::size_t i=0;i<hex.size();i+=2) { int a=digit(hex[i]), b=digit(hex[i+1]); if (a<0 || b<0) return 2; f.payload.push_back(static_cast<std::uint8_t>(a*16+b)); }
    auto encoded=hop::encode_frame(f); if (encoded.empty()) return 2;
    std::cout << (hop::valid_wire_payload(f) ? "valid " : "invalid ");
    for (auto b:encoded) std::cout << std::hex << std::setfill('0') << std::setw(2) << int(b);
    std::cout << '\n';
}
