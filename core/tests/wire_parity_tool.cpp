#include "hop/wire_validation.h"
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
int main() {
    std::string line;
    while (std::getline(std::cin,line)) {
        std::istringstream in(line); unsigned kind; std::uint64_t seq,epoch; std::string hex;
        if (!(in>>kind>>seq>>epoch>>hex) || hex.size()%2 || kind > 255) { std::cout<<"error\n"; continue; }
        hop::Frame f{static_cast<hop::FrameKind>(kind),seq,epoch,{}}; bool ok=true;
        for(std::size_t i=0;i<hex.size();i+=2) {
            auto digit=[](char c) { return c>='0' && c<='9' ? c-'0' : c>='a' && c<='f' ? c-'a'+10 : -1; };
            auto a=digit(hex[i]), b=digit(hex[i+1]); if(a<0 || b<0) { ok=false; break; } f.payload.push_back(static_cast<std::uint8_t>(a*16+b));
        }
        std::cout << (!ok ? "error" : hop::valid_wire_payload(f) ? "valid" : "invalid") << '\n';
    }
}
