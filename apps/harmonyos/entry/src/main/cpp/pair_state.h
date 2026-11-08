#pragma once
#include "protocol.h"
namespace hop {
// Pure protocol state: no sockets, SDK or storage. A COMMIT acknowledgment is
// impossible until ArkTS confirms durable Preferences.flush() completion.
class PairState {
public:
    bool resuming=false, confirmed=false, saved=false, commitReceived=false, ready=false;
    uint32_t code=0;
    uint8_t direction=1;
    std::array<uint8_t,16> nonce{},peer{};
    std::array<uint8_t,32> token{};
    enum class Action { None, Challenge, Persist, CommitAck, Ready, Reject };
    void Confirm() { Require(code&&!resuming&&!commitReceived,"no challenge to confirm");confirmed=true; }
    bool Saved() { if(saved)return false;Require(commitReceived&&confirmed,"no confirmed binding to persist");saved=true;return true; }
    Action Receive(const Message& m) {
        m.validate(); const auto* p=m.data();
        Require(!ready&&m.type()==Type::Hello&&m.bytes[2]==1&&p[1]==1,"unexpected HELLO");
        Require(ConstantEqual(p+32,nonce.data(),16),"nonce mismatch");
        auto phase=Phase(p[0]);
        if(phase==Phase::Reject)return Action::Reject;
        if(phase==Phase::Challenge){
            Require(!resuming&&!commitReceived,"unexpected challenge");
            if(code)Require(code==U32(p+4)&&direction==p[2]&&ConstantEqual(peer.data(),p+16,16),"changed challenge");
            else{code=U32(p+4);direction=p[2];std::copy_n(p+16,16,peer.begin());}
            return Action::Challenge;
        }
        Require(ConstantEqual(peer.data(),p+16,16),"peer identity mismatch");
        if(phase==Phase::Commit){
            Require(!resuming&&confirmed&&code==U32(p+4)&&direction==p[2],"unconfirmed commit");
            if(commitReceived){Require(ConstantEqual(token.data(),p+48,32),"changed token");return saved?Action::CommitAck:Action::None;}
            std::copy_n(p+48,32,token.begin());commitReceived=true;return Action::Persist;
        }
        Require(phase==Phase::Ready&&(resuming||saved)&&ConstantEqual(token.data(),p+48,32),"READY binding mismatch");
        if(!resuming)Require(direction==p[2]&&(U32(p+4)==0||U32(p+4)==code),"READY pairing mismatch");
        direction=p[2];ready=true;return Action::Ready;
    }
};
}
