#include "hop_windows/pairing_ui.h"
namespace hop::windows {
namespace {
std::wstring fingerprint(const std::string& value) {
    if(value.size()!=64)return L"";
    std::wstring out;out.reserve(72);
    for(std::size_t i=0;i<value.size();++i) {
        const char c=value[i];if(!((c>='0'&&c<='9')||(c>='A'&&c<='F')))return L"";
        out.push_back(static_cast<wchar_t>(c));if(i%16==15 && i!=63)out.push_back(L'\n');
    }
    return out;
}
}
std::wstring pairing_prompt_text(UiLanguage language,const std::string& local,const std::string& remote,bool after_tls) {
    auto a=fingerprint(local),b=fingerprint(remote);if(a.empty()||b.empty())return {};
    if(language==UiLanguage::chinese) {
        return std::wstring(after_tls ? L"加密连接已建立。请再次确认已通过独立可信渠道核对以下完整指纹。\n\n" : L"请通过独立可信渠道与对方逐字核对以下完整指纹。设备名称和网络地址不能证明身份。\n\n")+
               L"本机 SHA-256：\n"+a+L"\n\n对方 SHA-256：\n"+b+L"\n\n确认完全一致吗？";
    }
    return std::wstring(after_tls ? L"Encrypted connection established. Confirm you independently compared both complete fingerprints.\n\n" : L"Compare both complete fingerprints through an independent trusted channel. Device names and network addresses do not prove identity.\n\n")+
           L"This device SHA-256:\n"+a+L"\n\nPeer SHA-256:\n"+b+L"\n\nDo they match exactly?";
}
bool show_candidate_fingerprint_prompt(HWND owner,UiLanguage language,PairingReview& review) {
    const auto text=pairing_prompt_text(language,review.local_fingerprint(),review.remote_fingerprint(),false);if(text.empty())return false;
    if(MessageBoxW(owner,text.c_str(),language==UiLanguage::chinese?L"核对设备指纹":L"Compare fingerprints",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)!=IDYES){review.cancel();return false;}
    return review.approve_after_independent_comparison(GetTickCount64());
}
bool show_tls_pair_confirmation(HWND owner,UiLanguage language,const PairingReview& review) {
    VerifiedPeerCertificate peer; std::uint64_t expires=0;
    if(!review.approved_peer(GetTickCount64(),peer,expires))return false;
    const auto text=pairing_prompt_text(language,review.local_fingerprint(),review.remote_fingerprint(),true);if(text.empty())return false;
    if(MessageBoxW(owner,text.c_str(),language==UiLanguage::chinese?L"确认配对":L"Confirm pairing",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)!=IDYES)return false;
    return review.approved_peer(GetTickCount64(),peer,expires);
}
}
