#include "hop/posix_tls_socket.h"
#include "hop/posix_tls_socket_c.h"
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <thread>
using namespace hop;
namespace {
int checks = 0;
void check(bool ok, const char* why) { ++checks; if (!ok) { std::cerr << "FAIL " << why << '\n'; std::exit(1); } }
struct Identity {
    std::array<char,8192> cert{}, key{};
    std::array<char,65> fingerprint{};
    Identity() { check(hop_tls_generate_identity(cert.data(), cert.size(), key.data(), key.size(), fingerprint.data(), fingerprint.size()) == 0, "generate temporary identity"); }
    ~Identity() { volatile char* p = key.data(); for (std::size_t i = 0; i < key.size(); ++i) p[i] = 0; }
};
struct Pair { int client, server; };
Pair loopback() {
    int listener = socket(AF_INET, SOCK_STREAM, 0); check(listener >= 0, "listener");
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    check(bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 && listen(listener, 1) == 0, "bind localhost");
    socklen_t len = sizeof(addr); check(getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len) == 0, "port");
    int client = socket(AF_INET, SOCK_STREAM, 0); check(client >= 0 && connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "connect");
    int server = accept(listener, nullptr, nullptr); close(listener); check(server >= 0, "accept");
    return {client, server};
}
PosixTlsCredentials credentials(const Identity& own, const Identity& peer) { return {own.cert.data(), own.key.data(), {peer.cert.data()}}; }
bool handshake(PosixTlsSocket& client, PosixTlsSocket& server, bool expected) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        auto a = client.handshake(), b = server.handshake();
        if (a == SocketProgress::failed || b == SocketProgress::failed) return !expected;
        if (a == SocketProgress::ready && b == SocketProgress::ready) return expected;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}
void transfer(PosixTlsSocket& sender, PosixTlsSocket& receiver, const char* data) {
    std::array<std::uint8_t, 128> output{}; auto length = std::strlen(data);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5); bool sent = false, got = false;
    while (std::chrono::steady_clock::now() < deadline && !got) {
        if (!sent) { auto r = sender.write(reinterpret_cast<const std::uint8_t*>(data), length);
            check(r.status == SocketProgress::ready || r.status == SocketProgress::again, "write progress");
            if (r.status == SocketProgress::ready) { check(r.bytes == length, "write length"); sent = true; } }
        auto r = receiver.read(output.data(), output.size());
        check(r.status == SocketProgress::ready || r.status == SocketProgress::again, "read progress");
        if (r.status == SocketProgress::ready) { check(r.bytes == length && std::memcmp(output.data(), data, length) == 0, "payload"); got = true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(sent && got, "transfer deadline");
}
}
int main() {
    Identity a, b, wrong;
    {
        auto sockets = loopback(); auto c = PosixTlsSocket::adopt(sockets.client, true, credentials(a,b)); auto s = PosixTlsSocket::adopt(sockets.server, false, credentials(b,a));
        check(c && s, "adopt connected sockets");
        int noDelay = 0; socklen_t optionSize = sizeof(noDelay);
        const int optionResult = getsockopt(sockets.client,IPPROTO_TCP,TCP_NODELAY,&noDelay,&optionSize);
        if (optionResult != 0 || noDelay == 0) std::cerr << "TCP_NODELAY diagnostic: rc=" << optionResult << " value=" << noDelay << " length=" << optionSize << '\n';
        check(optionResult == 0 && noDelay != 0, "small input frames disable Nagle");
        std::uint8_t x = 0; check(c->write(&x, 1).status == SocketProgress::failed && s->read(&x, 1).status == SocketProgress::failed, "pre-auth denied");
        check(handshake(*c, *s, true), "mutual authentication");
        transfer(*c,*s,"client to server"); transfer(*s,*c,"server to client");
        s.reset();
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3); SocketResult r{SocketProgress::again};
        while (std::chrono::steady_clock::now() < deadline && r.status == SocketProgress::again) { r = c->read(&x,1); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        check(r.status == SocketProgress::closed || r.status == SocketProgress::failed, "closed peer terminates stream");
    }
    {
        auto sockets = loopback(); auto c = PosixTlsSocket::adopt(sockets.client, true, credentials(a,wrong)); auto s = PosixTlsSocket::adopt(sockets.server, false, credentials(b,a));
        check(c && s && handshake(*c,*s,false) && !c->authenticated(), "wrong server pin rejected");
    }
    {
        auto sockets = loopback(); auto c = PosixTlsSocket::adopt(sockets.client, true, credentials(wrong,b)); auto s = PosixTlsSocket::adopt(sockets.server, false, credentials(b,a));
        check(c && s && handshake(*c,*s,false) && !s->authenticated(), "wrong client pin rejected");
    }
    {
        auto sockets = loopback(); auto invalid = credentials(a,b); invalid.peer.certificate_pem.clear();
        check(!PosixTlsSocket::adopt(sockets.client,true,invalid), "missing pin rejected");
        int option = 0; socklen_t len = sizeof(option); check(getsockopt(sockets.client,SOL_SOCKET,SO_TYPE,&option,&len) != 0, "failed adopt closes fd"); close(sockets.server);
    }
    {
        auto sockets = loopback(); int small = 4096;
        check(setsockopt(sockets.client,SOL_SOCKET,SO_SNDBUF,&small,sizeof(small)) == 0, "limit send buffer");
        auto c = PosixTlsSocket::adopt(sockets.client,true,credentials(a,b));
        auto s = PosixTlsSocket::adopt(sockets.server,false,credentials(b,a));
        check(c && s && handshake(*c,*s,true), "backpressure test handshake");
        std::array<std::uint8_t,16384> payload{}; payload.fill(0x41);
        bool blocked = false; const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (int i = 0; i < 2048 && std::chrono::steady_clock::now() < deadline; ++i) {
            auto r = c->write(payload.data(),payload.size());
            if (r.status == SocketProgress::again) { blocked = true; break; }
            check(r.status == SocketProgress::ready && r.bytes > 0, "write during backpressure test");
        }
        check(blocked, "backpressure is nonblocking");
        payload[0] = 0x42;
        check(c->write(payload.data(),payload.size()).status == SocketProgress::failed && !c->authenticated(), "changed retry buffer fails closed");
    }
    {
        auto sockets = loopback();
        auto* c = hop_posix_tls_adopt(sockets.client,1,a.cert.data(),a.key.data(),b.cert.data());
        auto* s = hop_posix_tls_adopt(sockets.server,0,b.cert.data(),b.key.data(),a.cert.data());
        check(c && s, "C ABI adopts sockets");
        check(hop_posix_tls_authenticated_peer(c)==nullptr && hop_posix_tls_authenticated_peer(s)==nullptr, "C ABI never exposes unauthenticated peer");
        check(hop_posix_tls_wants_write(nullptr)==-1, "C ABI rejects invalid poll interest");
        std::uint8_t data[] = {7,8,9}, output[3]{}; size_t count = 42;
        check(hop_posix_tls_write(c,data,sizeof(data),&count) == -1 && count == 0, "C ABI blocks pre-auth writes");
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        int clientStatus = 0, serverStatus = 0;
        while (std::chrono::steady_clock::now() < deadline && !(clientStatus == 1 && serverStatus == 1)) {
            clientStatus = hop_posix_tls_handshake(c); serverStatus = hop_posix_tls_handshake(s);
            if (clientStatus < 0 || serverStatus < 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        check(clientStatus == 1 && serverStatus == 1, "C ABI handshake");
        check(hop_posix_tls_authenticated_peer(c) && std::string(hop_posix_tls_authenticated_peer(c))==b.fingerprint.data(), "C ABI reports pinned authenticated peer");
        check(hop_posix_tls_authenticated_peer(s) && std::string(hop_posix_tls_authenticated_peer(s))==a.fingerprint.data(), "C ABI reports mutual authenticated peer");
        bool sent = false, received = false;
        while (std::chrono::steady_clock::now() < deadline && !received) {
            if (!sent) { int r = hop_posix_tls_write(c,data,sizeof(data),&count); check(r >= 0, "C ABI write"); if (r == 1) sent = count == sizeof(data); }
            int r = hop_posix_tls_read(s,output,sizeof(output),&count); check(r >= 0, "C ABI read");
            if (r == 1) received = count == sizeof(data) && std::memcmp(data,output,sizeof(data)) == 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        check(sent && received, "C ABI transfers exact data");
        hop_posix_tls_free(c); hop_posix_tls_free(s);
        auto invalid = loopback();
        check(!hop_posix_tls_adopt(invalid.client, 2, a.cert.data(), a.key.data(), b.cert.data()), "C ABI rejects invalid role");
        int type = 0; socklen_t len = sizeof(type);
        check(getsockopt(invalid.client,SOL_SOCKET,SO_TYPE,&type,&len) != 0, "C ABI failure consumes socket"); close(invalid.server);
    }
    std::cout << "PASS " << checks << " POSIX pinned TLS loopback checks with temporary identities\n";
}
