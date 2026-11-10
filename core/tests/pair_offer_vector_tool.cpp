#include "hop/pair_offer.h"
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
int main() {
    std::string line;
    while(std::getline(std::cin,line)) {
        std::istringstream in(line); std::string type,hex;
        if(!(in>>type>>hex) || hex.size()%2) { std::cout<<"error\n"; continue; }
        std::vector<std::uint8_t> bytes; bool ok=true;
        for(std::size_t i=0;i<hex.size();i+=2) {
            auto digit=[](char c) { return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1; };
            int a=digit(hex[i]),b=digit(hex[i+1]); if(a<0||b<0){ok=false;break;} bytes.push_back(std::uint8_t(a*16+b));
        }
        bool valid=false;
        if(ok&&type=="offer") { hop::PairOfferDecoder d; valid=d.feed(bytes.data(),bytes.size())&&bool(d.finish()); }
        else if(ok&&type=="selector") valid=bool(hop::decode_peer_selector(bytes.data(),bytes.size()));
        std::cout << (valid?"valid":"invalid") << '\n';
    }
}
