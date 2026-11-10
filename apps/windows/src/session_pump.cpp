#include "hop_windows/session_pump.h"
#include <algorithm>
#include <array>
#include <windows.h>
namespace hop::windows {
bool SessionPump::abort() {
    session_.disconnect(); transport_.close();
    if (!pending_.empty()) SecureZeroMemory(pending_.data(),pending_.size());
    pending_.clear(); offset_=0;
    return false;
}
bool SessionPump::step(std::uint64_t now) {
    try {
    if ((!session_.peer_fingerprint().empty() && session_.peer_fingerprint()!=transport_.pinned_peer_fingerprint()) || !transport_.authenticated() || !session_.tick(transport_.authenticated(),now)) { return abort(); }
    for (unsigned i=0;i<16;++i) {
        if (pending_.empty()) {
            if (!session_.take(transport_.authenticated(),pending_)) return abort();
            offset_=0; if (pending_.empty()) { if (session_.phase()==SessionPhase::pair_cancel_wait) return abort(); break; }
        }
        const auto count=std::min<std::size_t>(16384,pending_.size()-offset_);
        auto out=transport_.write(pending_.data()+offset_,count);
        if (out.status==TlsIoStatus::again) break;
        if (out.status!=TlsIoStatus::ready || !out.bytes || out.bytes>count) { return abort(); }
        offset_+=out.bytes;
        if (offset_==pending_.size()) { SecureZeroMemory(pending_.data(),pending_.size()); pending_.clear(); offset_=0; }
    }
    std::array<std::uint8_t,16384> buffer{};
    for (unsigned i=0;i<16;++i) {
        auto in=transport_.read(buffer.data(),buffer.size());
        if (in.status==TlsIoStatus::again) break;
        if (in.status!=TlsIoStatus::ready || !in.bytes || in.bytes>buffer.size()) { return abort(); }
        if (!session_.receive(transport_.authenticated(),buffer.data(),in.bytes,now)) return abort();
    }
    return true;
    } catch (...) { return abort(); }
}
}
