#include "hop/posix_session_pump.h"
#include <algorithm>
#include <array>
#include <utility>
namespace hop {
PosixSessionPump::PosixSessionPump(PosixTlsSocket& transport,v2::Session& session,std::function<bool()> begin,std::function<void()> end)
 :transport_(transport),session_(session),begin_batch_(std::move(begin)),end_batch_(std::move(end)) {}
PosixSessionPump::~PosixSessionPump(){stop();}
void PosixSessionPump::wipe(){volatile std::uint8_t* p=pending_.data();for(std::size_t i=0;i<pending_.size();++i)p[i]=0;pending_.clear();offset_=0;}
bool PosixSessionPump::abort(){
    if(!stopped_){stopped_=true;try{session_.disconnect();}catch(...){} transport_.close();wipe();}
    return false;
}
void PosixSessionPump::stop(){abort();}
bool PosixSessionPump::step(std::uint64_t now){
    if(stopped_)return false;
    if(bool(begin_batch_) != bool(end_batch_))return abort();
    try {
        if(!transport_.authenticated() || session_.peer_fingerprint().empty() ||
           session_.peer_fingerprint()!=transport_.pinned_peer_fingerprint() || !session_.tick(true,now)) return abort();
        for(unsigned i=0;i<16;++i){
            if(pending_.empty()){
                if(!session_.take(true,pending_))return abort();offset_=0;
                if(pending_.empty()){if(session_.phase()==v2::SessionPhase::pair_cancel_wait)return abort();break;}
            }
            const auto n=std::min<std::size_t>(16384,pending_.size()-offset_);
            const auto out=transport_.write(pending_.data()+offset_,n);
            if(out.status==SocketProgress::again)break;
            if(out.status!=SocketProgress::ready||!out.bytes||out.bytes>n)return abort();
            offset_+=out.bytes;if(offset_==pending_.size())wipe();
        }
        std::array<std::uint8_t,16384> bytes{};
        struct Wiper{std::array<std::uint8_t,16384>& a;~Wiper(){volatile std::uint8_t* p=a.data();for(std::size_t i=0;i<a.size();++i)p[i]=0;}} wiper{bytes};
        for(unsigned i=0;i<16;++i){
            const auto in=transport_.read(bytes.data(),bytes.size());
            if(in.status==SocketProgress::again)break;
            if(in.status!=SocketProgress::ready||!in.bytes||in.bytes>bytes.size())return abort();
            if(begin_batch_ && !begin_batch_())return abort();
            bool ok=false;
            try {ok=session_.receive(true,bytes.data(),in.bytes,now);} catch(...) {if(end_batch_)end_batch_();return abort();}
            if(end_batch_)end_batch_();
            if(!ok)return abort();
        }
        return true;
    }catch(...){return abort();}
}
}
