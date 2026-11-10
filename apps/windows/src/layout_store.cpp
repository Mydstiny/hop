#include "hop_windows/layout_store.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <algorithm>
#include <limits>
#include <string>
#include <utility>
namespace hop::windows {
namespace {
constexpr std::size_t max_record=256*1024;
void put16(std::vector<std::uint8_t>& b,std::uint16_t v){b.push_back(v>>8);b.push_back(v&255);}
void put32(std::vector<std::uint8_t>& b,std::uint32_t v){for(int i=3;i>=0;--i)b.push_back((v>>(8*i))&255);}
bool id_ok(const std::string& id){return !id.empty()&&id.size()<=128&&std::all_of(id.begin(),id.end(),[](unsigned char c){return c>=33&&c<=126;});}
bool text_utf8(const std::wstring& text,std::string& out){
 if(text.empty()||text.size()>128||text.find(L'\0')!=std::wstring::npos)return false;
 const int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
 if(n<=0||n>512)return false;out.resize(n);
 return WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),out.data(),n,nullptr,nullptr)==n;
}
bool wide_utf8(const std::string& text,std::wstring& out){
 if(text.empty()||text.size()>512||text.find('\0')!=std::string::npos)return false;
 const int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
 if(n<=0||n>128)return false;out.resize(n);
 return MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),out.data(),n)==n;
}
struct Reader {
 const std::vector<std::uint8_t>& b;std::size_t i=0;
 bool take8(std::uint8_t& n){if(i>=b.size())return false;n=b[i++];return true;}
 bool take16(std::uint16_t& n){if(b.size()-i<2)return false;n=(std::uint16_t(b[i])<<8)|b[i+1];i+=2;return true;}
 bool take32(std::uint32_t& n){if(b.size()-i<4)return false;n=0;for(int k=0;k<4;++k)n=(n<<8)|b[i++];return true;}
 bool text(std::string& out,std::size_t max){std::uint16_t n=0;if(!take16(n)||!n||n>max||b.size()-i<n)return false;out.assign(reinterpret_cast<const char*>(b.data()+i),n);i+=n;return true;}
};
struct Key {HKEY value=nullptr;~Key(){if(value)RegCloseKey(value);}};
}
bool encode_layout(const LayoutEditor& editor,std::vector<std::uint8_t>& bytes){
 try {
  std::vector<std::uint8_t> out={'H','L','Y','1'};
  const auto& devices=editor.devices();if(devices.size()>64)return false;put16(out,static_cast<std::uint16_t>(devices.size()));
  for(const auto& d:devices){
   std::string name;if(!id_ok(d.id)||!text_utf8(d.name,name)||d.screens.empty()||d.screens.size()>16)return false;
   put16(out,static_cast<std::uint16_t>(d.id.size()));out.insert(out.end(),d.id.begin(),d.id.end());
   put16(out,static_cast<std::uint16_t>(name.size()));out.insert(out.end(),name.begin(),name.end());
   out.push_back(d.expanded?1:0);out.push_back(static_cast<std::uint8_t>(d.screens.size()));
   for(const auto& s:d.screens){if(!id_ok(s.id)||s.device_id!=d.id)return false;
    put16(out,static_cast<std::uint16_t>(s.id.size()));out.insert(out.end(),s.id.begin(),s.id.end());
    put32(out,static_cast<std::uint32_t>(s.x));put32(out,static_cast<std::uint32_t>(s.y));put32(out,static_cast<std::uint32_t>(s.width));put32(out,static_cast<std::uint32_t>(s.height));
   }
   if(out.size()>max_record)return false;
  }
  bytes=std::move(out);return true;
 }catch(...){return false;}
}
bool decode_layout(const std::vector<std::uint8_t>& bytes,LayoutEditor& editor){
 if(bytes.size()<6||bytes.size()>max_record||bytes[0]!='H'||bytes[1]!='L'||bytes[2]!='Y'||bytes[3]!='1')return false;
 try {
  Reader r{bytes,4};std::uint16_t count=0;if(!r.take16(count)||count>64)return false;
  std::vector<LayoutDevice> devices;devices.reserve(count);
  for(unsigned i=0;i<count;++i){
   LayoutDevice d;std::string name;std::uint8_t expanded=0,number=0;
   if(!r.text(d.id,128)||!id_ok(d.id)||!r.text(name,512)||!wide_utf8(name,d.name)||!r.take8(expanded)||expanded>1||!r.take8(number)||!number||number>16)return false;
   d.expanded=expanded!=0;d.online=false;d.trusted=false;
   for(unsigned j=0;j<number;++j){
    hop::Screen screen{};std::uint32_t x=0,y=0,w=0,h=0;
    if(!r.text(screen.id,128)||!id_ok(screen.id)||!r.take32(x)||!r.take32(y)||!r.take32(w)||!r.take32(h))return false;
    // Decode two's-complement coordinates without implementation-defined cast.
    auto signed32=[](std::uint32_t v)->std::int32_t {return v<=0x7fffffff ? static_cast<std::int32_t>(v) : static_cast<std::int32_t>(std::int64_t(v)-0x100000000LL);};
    screen.device_id=d.id;screen.x=signed32(x);screen.y=signed32(y);screen.width=signed32(w);screen.height=signed32(h);d.screens.push_back(std::move(screen));
   }
   devices.push_back(std::move(d));
  }
  if(r.i!=bytes.size())return false;
  return editor.replace(std::move(devices));
 }catch(...){return false;}
}
bool RegistryLayoutStorage::load(std::vector<std::uint8_t>& bytes,bool& found){
 found=false;Key key;auto rc=RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\Hop\\Layout",0,KEY_QUERY_VALUE,&key.value);
 if(rc==ERROR_FILE_NOT_FOUND)return true;if(rc!=ERROR_SUCCESS)return false;
 DWORD type=0,size=0;auto status=RegQueryValueExW(key.value,value_name_.c_str(),nullptr,&type,nullptr,&size);
 if(status==ERROR_FILE_NOT_FOUND)return true;if(status!=ERROR_SUCCESS||type!=REG_BINARY||size<6||size>max_record)return false;
 try {std::vector<std::uint8_t> data(size);status=RegQueryValueExW(key.value,value_name_.c_str(),nullptr,&type,data.data(),&size);
  if(status!=ERROR_SUCCESS||type!=REG_BINARY||size!=data.size())return false;bytes=std::move(data);found=true;return true;
 }catch(...){return false;}
}
bool RegistryLayoutStorage::save(const std::vector<std::uint8_t>& bytes){
 if(bytes.size()<6||bytes.size()>max_record)return false;Key key;
 if(RegCreateKeyExW(HKEY_CURRENT_USER,L"Software\\Hop\\Layout",0,nullptr,0,KEY_SET_VALUE,nullptr,&key.value,nullptr)!=ERROR_SUCCESS)return false;
 return RegSetValueExW(key.value,value_name_.c_str(),0,REG_BINARY,bytes.data(),static_cast<DWORD>(bytes.size()))==ERROR_SUCCESS;
}
bool save_layout(LayoutStorage& storage,const LayoutEditor& editor){try{std::vector<std::uint8_t> data;return encode_layout(editor,data)&&storage.save(data);}catch(...){return false;}}
bool load_layout(LayoutStorage& storage,LayoutEditor& editor,bool& found){try{std::vector<std::uint8_t> data;found=false;if(!storage.load(data,found))return false;return !found||decode_layout(data,editor);}catch(...){found=false;return false;}}
} // namespace hop::windows
