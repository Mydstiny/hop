#include "hop_windows/image_codec.h"
#include "hop_windows/clipboard.h"
#include "hop_windows/dib.h"
#include <cstdlib>
#include <iostream>
#include <vector>
using namespace hop::windows;
int checks=0;
void check(bool b,const char* s,int line){++checks;if(!b){std::cerr<<line<<": "<<s<<'\n';std::exit(1);}}
#define CHECK(x) check(bool(x),#x,__LINE__)
int main(){
    BgraImage input{2,2,{0,0,255,255, 0,255,0,255, 255,0,0,128, 4,8,16,0}};
    std::vector<std::uint8_t> dib,png,restored;
    CHECK(encode_dibv5(input,dib));
    CHECK(dib_to_png(dib.data(),dib.size(),png));
    CHECK(valid_png_bytes(png));
    CHECK(png_to_dibv5(png,restored));
    BgraImage output;
    CHECK(decode_dib(restored.data(),restored.size(),output));
    CHECK(output.width==2 && output.height==2 && output.pixels==input.pixels);
    auto bad=dib; bad.resize(20); CHECK(!dib_to_png(bad.data(),bad.size(),png));
    std::vector<std::uint8_t> invalid{1,2,3}; CHECK(!png_to_dibv5(invalid,restored));
    std::cout<<"PASS "<<checks<<" WIC PNG/DIBV5 memory checks; no clipboard operations\n";
}
