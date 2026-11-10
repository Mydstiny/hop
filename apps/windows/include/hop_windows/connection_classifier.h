#pragma once
#include "hop_windows/pair_transport.h"
#include <memory>
namespace hop::windows {
enum class ConnectionKind { unknown, candidate_offer, trusted_selector };
// Peeks only the four-byte protocol identifier on an accepted connection; it
// never consumes HPOP/HPSL bytes. Candidate mode must be explicitly enabled
// by the user and always uses a separate connection from the later TLS path.
class ConnectionClassifier {
public:
    static std::unique_ptr<ConnectionClassifier> accept(SOCKET socket,bool candidate_window_enabled,std::uint64_t now_ms);
    ~ConnectionClassifier();
    PairIo step(std::uint64_t now_ms);
    ConnectionKind kind() const {return kind_;}
    SOCKET release();
private:
    ConnectionClassifier(SOCKET socket,bool candidate,std::uint64_t now) : socket_(socket),candidate_(candidate),started_(now),last_now_(now) {}
    PairIo fail();
    SOCKET socket_=INVALID_SOCKET;bool candidate_=false,failed_=false;std::uint64_t started_,last_now_;ConnectionKind kind_=ConnectionKind::unknown;
};
}
