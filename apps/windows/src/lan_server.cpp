#include "hop_windows/lan_server.h"
#include <limits>
#include <new>
namespace hop::windows {
std::unique_ptr<LanServer> LanServer::start(const TcpAddress& address,bool enabled) {
    auto listener=TcpListener::open(address,enabled);if(!listener)return nullptr;
    return std::unique_ptr<LanServer>(new(std::nothrow) LanServer(std::move(listener)));
}
bool LanServer::open_pairing_window(std::uint64_t now,std::uint64_t duration) {
    if(!duration || duration>120000 || now>std::numeric_limits<std::uint64_t>::max()-duration || now<last_now_)return false;
    last_now_=now;pair_until_=now+duration;return true;
}
bool LanServer::poll(std::uint64_t now,std::unique_ptr<ClassifiedSocket>& result) {
    result.reset();if(now<last_now_)return false;last_now_=now;
    try {
        for(unsigned i=0;i<4 && pending_.size()<8;++i) {
            SOCKET socket=INVALID_SOCKET;const auto accepted=listener_->accept(socket);
            if(accepted==TcpProgress::failed)return false;if(accepted==TcpProgress::pending)break;
            auto classifier=ConnectionClassifier::accept(socket,pair_until_ && now<pair_until_,now);
            if(classifier)pending_.push_back(std::move(classifier));
        }
        for(auto it=pending_.begin();it!=pending_.end();) {
            const auto progress=(*it)->step(now);
            if(progress==PairIo::pending){++it;continue;}
            if(progress==PairIo::ready) {
                const auto kind=(*it)->kind();const auto socket=(*it)->release();it=pending_.erase(it);
                if(kind==ConnectionKind::candidate_offer && (!pair_until_ || now>=pair_until_)){if(socket!=INVALID_SOCKET)closesocket(socket);continue;}
                if(socket==INVALID_SOCKET)continue;
                result=std::unique_ptr<ClassifiedSocket>(new(std::nothrow) ClassifiedSocket(socket,kind));
                if(!result){closesocket(socket);return false;}return true;
            }
            it=pending_.erase(it);
        }
        return true;
    } catch(...) {return false;}
}
}
