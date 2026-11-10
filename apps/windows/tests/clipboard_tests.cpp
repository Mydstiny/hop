#include "hop_windows/clipboard.h"
#include <cstdlib>
#include <iostream>
using namespace hop::windows;
namespace {
int n=0;
void check(bool b, const char* text, int line) { ++n; if(!b) { std::cerr<<line<<": "<<text<<'\n';std::exit(1); } }
#define CHECK(x) check(bool(x),#x,__LINE__)
struct Fake final : ClipboardBackend {
    int reads=0, writes=0;
    bool read_ok=true, write_ok=true;
    std::uint32_t written=0, expected=0, current=10;
    ClipboardPayload payload{ClipboardFormat::utf8_text, {'h','i'}};
    bool read(std::uint32_t sequence, ClipboardPayload& out) override {
        ++reads; expected=sequence; if(!read_ok) return false; out=payload; return true;
    }
    bool write(const ClipboardPayload& value, std::uint32_t expected_sequence, std::uint32_t& seq) override {
        ++writes; if (!write_ok || current!=expected_sequence) return false; payload=value; seq=written; current=written; return true;
    }
};
ClipboardPayload image() {
    return {ClipboardFormat::png, {0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,
      0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x06,0x00,0x00,0x00,
      0x1f,0x15,0xc4,0x89,0x00,0x00,0x00,0x0b,0x49,0x44,0x41,0x54,0x78,0x9c,0x63,0x60,0x00,
      0x02,0x00,0x00,0x05,0x00,0x01,0x7a,0x5e,0xab,0x3f,0x00,0x00,0x00,0x00,0x49,0x45,0x4e,
      0x44,0xae,0x42,0x60,0x82}};
}
}
int main() {
    CHECK(select_clipboard_format(true,true,true,true,true)==ClipboardSelection::none);
    CHECK(select_clipboard_format(false,true,true,true,true)==ClipboardSelection::png);
    CHECK(select_clipboard_format(false,false,true,true,true)==ClipboardSelection::dibv5);
    CHECK(select_clipboard_format(false,false,false,true,true)==ClipboardSelection::dib);
    CHECK(select_clipboard_format(false,false,false,false,true)==ClipboardSelection::text);
    CHECK(select_clipboard_format(false,false,false,false,false)==ClipboardSelection::none);
    ClipboardPayload plain{ClipboardFormat::utf8_text, {'h','i'}};
    ClipboardPayload empty{ClipboardFormat::utf8_text, {}};
    ClipboardPayload multibyte{ClipboardFormat::utf8_text, {0xe4,0xbd,0xa0,0xf0,0x9f,0x98,0x80}};
    CHECK(valid_clipboard_payload(plain) && valid_clipboard_payload(empty) && valid_clipboard_payload(multibyte));
    CHECK(!valid_clipboard_payload({ClipboardFormat::utf8_text, {0xc0,0x80}}));
    CHECK(!valid_clipboard_payload({ClipboardFormat::utf8_text, {0xed,0xa0,0x80}}));
    CHECK(!valid_clipboard_payload({ClipboardFormat::utf8_text, {0xf4,0x90,0x80,0x80}}));
    CHECK(!valid_clipboard_payload({ClipboardFormat::utf8_text, {'a',0,'b'}}));
    CHECK(valid_clipboard_payload({ClipboardFormat::utf8_text, {'C',':','\\','f'}}));
    CHECK(valid_clipboard_payload({ClipboardFormat::utf8_text, {'x','\n','/','a'}}));
    CHECK(valid_clipboard_payload({ClipboardFormat::utf8_text, {'F','I','L','E',':','/','/','x'}}));
    CHECK(valid_clipboard_payload({ClipboardFormat::utf8_text, {'h','t','t','p',':','/','/','x'}}));
    CHECK(!valid_clipboard_payload({ClipboardFormat::utf8_text, std::vector<std::uint8_t>(max_text_bytes+1,'a')}));
    auto png = image(); CHECK(valid_clipboard_payload(png));
    auto bad = png; bad.bytes[0]=0; CHECK(!valid_clipboard_payload(bad));
    bad=png; bad.bytes[19]=0; CHECK(!valid_clipboard_payload(bad)); // zero width
    bad=png; bad.bytes[18]=0xff; bad.bytes[19]=0xff; CHECK(!valid_clipboard_payload(bad));
    bad=png; bad.bytes.resize(32); CHECK(!valid_clipboard_payload(bad));
    bad=png; bad.bytes[45] ^= 1; CHECK(!valid_clipboard_payload(bad)); // bad IDAT CRC
    bad=png; bad.bytes.push_back(0); CHECK(!valid_clipboard_payload(bad)); // trailing data
    bad=png;
    bad.bytes.insert(bad.bytes.begin()+33, {0,0,0,1,'i','C','C','P','x',77,24,130,49});
    CHECK(!valid_clipboard_payload(bad)); // valid CRC, compressed metadata excluded
    CHECK(!valid_clipboard_payload({static_cast<ClipboardFormat>(99), {}}));
    Fake f; ClipboardSync s(f);
    CHECK(!s.active() && !s.on_change(10,1) && f.reads==0);
    CHECK(!s.apply_remote("pc",1,plain,10) && f.writes==0);
    s.configure(true,false,"pc",1,9); CHECK(!s.active() && !s.on_change(10,1) && f.reads==0);
    s.configure(true,true,"pc",1,0); CHECK(!s.active() && !s.on_change(10,1) && f.reads==0);
    s.configure(true,true,"pc",1,9); CHECK(s.active());
    CHECK(!s.on_change(9,1) && !s.on_change(10,2) && f.reads==0); // old baseline/generation
    auto read=s.on_change(10,1); CHECK(read && read->bytes==plain.bytes && f.reads==1 && f.expected==10);
    CHECK(!s.on_change(10,1) && f.reads==1);
    CHECK(!s.apply_remote("other",1,png,10) && !s.apply_remote("pc",2,png,10) && f.writes==0);
    f.written=11; CHECK(s.apply_remote("pc",1,png,10) && f.writes==1);
    CHECK(!s.on_change(11,1) && f.reads==1); // echo
    f.payload=plain; read=s.on_change(12,1); CHECK(read && f.reads==2);
    f.current=13; s.configure(true,true,"pc2",2,12);
    CHECK(!s.on_change(12,2) && !s.on_change(13,1) && f.reads==2); // no old-target replay
    f.read_ok=false; CHECK(!s.on_change(13,2) && f.reads==3);
    f.read_ok=true; CHECK(s.on_change(13,2) && f.reads==4); // retry is possible
    CHECK(!s.apply_remote("pc2",2,bad,13) && f.writes==1);
    f.write_ok=false; f.written=14; CHECK(!s.apply_remote("pc2",2,plain,13) && s.faulted());
    CHECK(!s.on_change(14,2) && !s.apply_remote("pc2",2,plain,13));
    s.configure(true,true,"pc2",2,14); CHECK(s.faulted()); // no silent fault reset
    s.configure(false,true,"pc2",2,14); s.configure(true,true,"pc2",2,14); CHECK(s.active());
    f.write_ok=true; f.written=0; CHECK(!s.apply_remote("pc2",2,plain,13) && s.faulted());
    Fake conflict; ClipboardSync guarded(conflict); guarded.configure(true,true,"pc",3,10);
    conflict.current=12; conflict.written=13;
    CHECK(!guarded.apply_remote("pc",3,png,10));
    CHECK(conflict.payload.format==ClipboardFormat::utf8_text && conflict.current==12);
    std::cout << "PASS " << n << " clipboard policy checks; fake backend only\n";
}
