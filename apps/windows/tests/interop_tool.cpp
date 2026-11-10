#include "hop_windows/pairing_worker.h"
#include "hop_windows/host_input_bridge.h"
#include "hop_windows/receiver_session_adapter.h"
#include "hop/secure_channel_c.h"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace hop::windows;
namespace {
class MemoryCredentials final : public CredentialBackend {
public:
    CredentialRead read(const std::wstring& name, std::vector<std::uint8_t>& value) override {
        const auto found = records_.find(name);
        if (found == records_.end()) return CredentialRead::missing;
        value = found->second;
        return CredentialRead::found;
    }
    bool write(const std::wstring& name, const std::vector<std::uint8_t>& value) override {
        records_[name] = value;
        return true;
    }
    bool erase(const std::wstring& name) override { return records_.erase(name) != 0; }
private:
    std::map<std::wstring, std::vector<std::uint8_t>> records_;
};

std::string read_file(const char* path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::string fingerprint(const std::string& certificate) {
    char bytes[65]{};
    if (hop_tls_certificate_fingerprint(certificate.data(), certificate.size(), bytes, sizeof(bytes)) != 0) return {};
    return bytes;
}

int fail(const char* stage) {
    std::cerr << "FAIL Windows adapter " << stage << '\n';
    return 1;
}
}

int main(int argc, char** argv) {
    if (argc != 7) return fail("usage: host|receiver listen|dial port own-cert own-key expected-peer-cert");
    const std::string role = argv[1], direction = argv[2];
    if ((role != "host" && role != "receiver") || (direction != "listen" && direction != "dial")) return fail("arguments");
    const bool host = role == "host", listen = direction == "listen";
    char* end = nullptr;
    const auto port = std::strtoul(argv[3], &end, 10);
    if (!end || end == argv[3] || *end || port > 65535 || (!listen && !port)) return fail("port");

    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) return fail("winsock");
    struct WinsockGuard { ~WinsockGuard() { WSACleanup(); } } winsock_guard;
    DeviceIdentity own;
    own.certificate = read_file(argv[4]);
    own.private_key = read_file(argv[5]);
    own.fingerprint = fingerprint(own.certificate);
    const auto expected = fingerprint(read_file(argv[6]));
    if (own.fingerprint.empty() || own.private_key.empty() || expected.empty() || own.fingerprint == expected)
        return fail("temporary identity files");

    MemoryCredentials credentials;
    TrustStore trust(credentials);
    std::vector<INPUT> injected;
    InputSink sink([&](UINT count, LPINPUT values, int) {
        injected.insert(injected.end(), values, values + count);
        return count;
    });
    ReceiverSessionAdapter receiver(sink, {{7, {0, 0, 1001, 501}}}, {0, 0, 1001, 501});
    auto callbacks = [&] {
        if (!host) return receiver.callbacks();
        return SessionCallbacks{{}, {}, [] { return true; }};
    };
    PairingWorker worker(own, trust, host ? SessionRole::host : SessionRole::receiver,
                         "Windows interop", host ? std::vector<std::uint64_t>{} : receiver.screen_ids(), callbacks);
    TcpAddress address{};
    if (!numeric_tcp_address("127.0.0.1", static_cast<std::uint16_t>(port), address)) return fail("loopback address");
    const auto started = GetTickCount64();
    if (!(listen ? worker.begin_listening(address, started) : worker.begin_connecting(address, started)))
        return fail("start HPOP endpoint");
    if (listen) {
        if (!worker.local_address(address)) return fail("query listener");
        std::cout << "PORT " << ntohs(reinterpret_cast<const sockaddr_in*>(&address.value)->sin_port) << std::endl;
    }

    bool reviewed = false, confirmed = false, connected = false, entered = false, advertised = false;
    bool sent = false, released = false;
    std::uint64_t send_time = 0, ready_time = 0;
    while (GetTickCount64() - started < 20000) {
        const auto now = GetTickCount64();
        if (!worker.step(now)) {
            if (ready_time) break; // other endpoint received RELEASE_ACK and closed
            return fail(confirmed ? "authenticated session" : reviewed ? "HPSL or TLS" : "HPOP");
        }
        if (!reviewed && worker.state() == PairingState::reviewing) {
            auto review = worker.review();
            if (!review || review->local_fingerprint() != own.fingerprint || review->remote_fingerprint() != expected ||
                !review->approve_after_independent_comparison(now)) return fail("candidate pin comparison");
            reviewed = true;
            std::cout << "HPOP compared" << std::endl;
        }
        if (!confirmed && worker.state() == PairingState::confirming) {
            if (!worker.transport() || !worker.transport()->authenticated() ||
                worker.transport()->pinned_peer_fingerprint() != expected || !worker.confirm_authenticated_pair(true))
                return fail("pinned TLS and explicit confirmation");
            confirmed = true;
            std::cout << "TLS confirmed locally" << std::endl;
        }
        if (worker.state() != PairingState::connected) { Sleep(1); continue; }
        auto* session = worker.session();
        if (!session || !session->pairing_confirmed() || session->peer_fingerprint() != expected)
            return fail("paired session pin");
        if (!connected) {
            connected = true;
            std::cout << "PAIR both confirmed" << std::endl;
        }
        if (!host && !advertised) {
            if (!session->advertise_screens(receiver.screen_payload())) return fail("screen advertisement");
            advertised = true;
        }
        if (host && !entered) {
            const auto screens = worker.peer_displays();
            if (!screens.empty()) {
                if (!worker.request_focus(screens.front().id, 32768, 32768)) return fail("request focus");
                entered = true;
            }
        }
        if (session->phase() == SessionPhase::active) {
            if (!host) entered = true;
            if (host && !sent) {
                const auto displays = worker.peer_displays();
                const auto& screen = displays.front();
                HostInputBridge input(*session, screen.id, screen.width, screen.height, 32768, 32768, false, {});
                if (!input.move_relative(2, 2) ||
                    !input.captured({CaptureKind::key, 0x1d, true}) ||
                    !input.captured({CaptureKind::key, 0x2e, true}) ||
                    !input.captured({CaptureKind::key, 0x2e, false}) ||
                    !input.captured({CaptureKind::key, 0x1d, false}) ||
                    !input.captured({CaptureKind::wheel, 0, false, false, 0, 120}))
                    return fail("host shortcut and wheel");
                sent = true;
                send_time = now;
            }
            if (host && sent && !released && now - send_time >= 200) {
                if (!session->release(0)) return fail("send release");
                released = true;
            }
        }
        if (entered && session->phase() == SessionPhase::ready) {
            if (!ready_time) ready_time = now;
            if (now - ready_time >= 350) break;
        }
        Sleep(1);
    }
    if (!reviewed || !confirmed || !connected || !entered || !ready_time || (host && (!sent || !released)))
        return fail("timed out before focus and release");
    if (!host) {
        bool control = false, copy_down = false, copy_up = false, wheel = false;
        for (const auto& event : injected) {
            if (event.type == INPUT_KEYBOARD) {
                const bool down = (event.ki.dwFlags & KEYEVENTF_KEYUP) == 0;
                if (event.ki.wScan == 0x1d) control = down;
                if (event.ki.wScan == 0x2e) { copy_down |= down && control; copy_up |= !down; }
            }
            if (event.type == INPUT_MOUSE && (event.mi.dwFlags & MOUSEEVENTF_WHEEL)) wheel |= event.mi.mouseData != 0;
        }
        if (!copy_down || !copy_up || !wheel || sink.held_keys() || sink.held_buttons())
            return fail("fake Windows input did not observe Ctrl+C, wheel and release");
    }
    std::cout << "PASS Windows " << role << " HPOP/HPSL, pinned TLS, screens, focus, key, wheel and release" << std::endl;
}
