#include "hop/secure_channel_c.h"
#include <array>
#include <fstream>
int main(int argc,char** argv) {
    if(argc!=3) return 2;
    std::array<char,8192> cert{},key{}; std::array<char,65> fingerprint{};
    if(hop_tls_generate_identity(cert.data(),cert.size(),key.data(),key.size(),fingerprint.data(),fingerprint.size())!=0) return 1;
    std::ofstream c(argv[1],std::ios::binary), k(argv[2],std::ios::binary);
    c<<cert.data(); k<<key.data(); c.close(); k.close();
    volatile char* p=key.data(); for(std::size_t i=0;i<key.size();++i) p[i]=0;
    return c.fail()||k.fail()?1:0;
}
