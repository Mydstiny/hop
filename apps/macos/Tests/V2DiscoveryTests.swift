import Darwin
import Foundation
import Network

@main struct V2DiscoveryTests {
    private final class FakeBonjour {
        struct Browse {
            let update: ([V2Discovery.Advertisement]) -> Void
            let fail: (String) -> Void
        }
        struct Lookup {
            let candidate: V2DiscoveryCandidate
            let finish: (V2Discovery.Resolution?) -> Void
        }
        var scans: [Browse] = []
        var lookups: [Lookup] = []
        var stopped = 0
        var cancelled = Set<Int>()

        func browse(_ update: @escaping ([V2Discovery.Advertisement]) -> Void, _ fail: @escaping (String) -> Void) -> V2Discovery.Cancel {
            scans.append(Browse(update: update, fail: fail))
            return { [weak self] in self?.stopped += 1 }
        }

        func resolve(_ candidate: V2DiscoveryCandidate, _ finish: @escaping (V2Discovery.Resolution?) -> Void) -> V2Discovery.Cancel {
            let index = lookups.count
            lookups.append(Lookup(candidate: candidate, finish: finish))
            return { [weak self] in _ = self?.cancelled.insert(index) }
        }
    }

    static func main() {
        var checks = 0
        func check(_ ok: Bool, _ detail: String) {
            checks += 1
            if !ok { fputs("FAIL v2 discovery: \(detail)\n", stderr); exit(1) }
        }
        let txt = Data([3, 0x76, 0x3d, 0x32])
        let service = V2Discovery.advertisement(deviceName: "测试设备 with a long local label", port: 24891)
        check(service.type == "_hop2._tcp." && service.domain == "local." && service.port == 24891 &&
              service.txtRecordData() == txt && service.name.utf8.count <= 63, "v2 advertisement has only the interoperable TXT")
        check(V2Discovery.announcement(name: "Hop").type == "_hop2._tcp", "v2 service type")
        check(service.name.hasPrefix("测试设备 with a long local label-") && service.name.suffix(10).allSatisfy(\.isHexDigit),
              "advertisement preserves device label with a short random suffix")
        check(V2Discovery.isLocalResolveTarget("Device-2.LoCaL.") && !V2Discovery.isLocalResolveTarget("Device.example.com.") &&
              !V2Discovery.isLocalResolveTarget("device..local.") && !V2Discovery.isLocalResolveTarget("evil\\.local."),
              "only a well-formed local SRV hostname can reach address resolution")
        let endpoint = NWEndpoint.service(name: "测试设备", type: "_hop2._tcp", domain: "local.", interface: nil)
        check(V2DiscoveryCandidate(endpoint: endpoint)?.name == "测试设备", "safe Unicode Bonjour name")
        check(V2DiscoveryCandidate(endpoint: .service(name: "old", type: "_hop._tcp", domain: "local.", interface: nil)) == nil,
              "v1 results are separate")
        check(V2DiscoveryCandidate(endpoint: .service(name: "Internet", type: "_hop2._tcp", domain: "example.com.", interface: nil)) == nil,
              "wide-area domains are rejected")
        check(V2DiscoveryCandidate(endpoint: .hostPort(host: "127.0.0.1", port: 24891)) == nil, "raw endpoint is not Bonjour")
        check(V2DiscoveryCandidate(name: "evil\u{202e}", type: "_hop2._tcp", domain: "local.") == nil &&
              V2DiscoveryCandidate(name: String(repeating: "🟢", count: 16), type: "_hop2._tcp", domain: "local.") == nil,
              "bidi controls and oversized names are rejected")

        let fake = FakeBonjour()
        let discovery = V2Discovery(browse: fake.browse, resolve: fake.resolve, localIPs: { ["192.168.1.3", "fe80::1"] })
        var hosts: [V2DiscoveredHost] = [], errors: [String] = []
        discovery.onHosts = { hosts = $0 }; discovery.onError = { errors.append($0) }
        func advertisement(_ name: String, txt record: Data? = nil, index: UInt32 = 0) -> V2Discovery.Advertisement {
            .init(candidate: V2DiscoveryCandidate(name: name, type: "_hop2._tcp.", domain: "local", interfaceIndex: index)!, txt: record ?? txt)
        }
        func result(_ addresses: [Data], port: Int = 24891, txt record: Data? = nil) -> V2Discovery.Resolution {
            .init(port: port, txt: record ?? txt, addresses: addresses)
        }

        check(fake.scans.isEmpty && fake.lookups.isEmpty, "construction never starts network discovery")
        discovery.start(); discovery.start()
        check(fake.scans.count == 1, "start is explicit and idempotent")
        let own = advertisement(service.name.uppercased()), wrongVersion = advertisement("Old", txt: Data([3, 0x76, 0x3d, 0x31]))
        let malformed = advertisement("Malformed TXT", txt: txt + txt)
        let remote = advertisement("Windows")
        fake.scans[0].update([own, wrongVersion, malformed, remote, remote])
        drain()
        check(fake.lookups.map(\.candidate.name) == ["Windows"], "self, malformed and duplicate ads are filtered before resolution")
        fake.lookups[0].finish(result([v4("192.168.1.3"), v4("127.0.0.1"), v4("224.0.0.1"), v4("192.168.1.4", port: 9),
                                       v6("::ffff:192.168.1.3"), v6("fd00::5"), v4("192.168.1.22")], txt: txt))
        drain()
        check(hosts == [.init(name: "Windows", host: "192.168.1.22", port: 24891)], "validated IPv4 chosen over local, invalid and alternate endpoints")

        let link = advertisement("Scoped IPv6", index: 7)
        let localAlias = advertisement("Local alias")
        let changedTXT = advertisement("Changed TXT")
        let badPort = advertisement("Bad port")
        let lost = advertisement("Gone")
        fake.scans[0].update([link, localAlias, changedTXT, badPort, lost])
        drain()
        check(hosts.isEmpty && fake.lookups.count == 5, "removal clears published hints and caps concurrent resolve jobs")
        fake.lookups[1].finish(result([v6("fe80::1", scope: 7), v6("fe80::88", scope: 8), v6("fe80::a")]))
        fake.lookups[2].finish(result([v4("192.168.1.3"), v6("::ffff:127.0.0.2"), v6("::")]))
        fake.lookups[3].finish(result([v4("192.168.1.23")], txt: txt + Data([0])))
        fake.lookups[4].finish(result([v4("192.168.1.24")], port: 65536))
        drain()
        check(hosts == [.init(name: "Scoped IPv6", host: "fe80::a%7", port: 24891)] && fake.lookups.count == 6,
              "IPv6 scope and local-address checks, malformed SRV/TXT rejection and bounded queue drain")
        check(numericAddress(hosts[0]), "scoped IPv6 is accepted by the receiver's numeric address parser")
        fake.scans[0].update([link])
        drain()
        check(fake.cancelled.contains(5), "removed service cancels its unresolved job")
        fake.lookups[5].finish(result([v4("192.168.1.25")]))
        drain()
        check(hosts.count == 1, "late result from a removed service is ignored")

        discovery.stop()
        check(fake.stopped == 1 && hosts.isEmpty, "stop cancels browse and clears results")
        discovery.start()
        fake.scans[0].update([remote]); fake.scans[0].fail("stale")
        fake.scans[1].update((0..<100).map { advertisement("Host \($0)") })
        drain()
        check(fake.lookups.count == 10 && errors.isEmpty, "old browse generation is ignored and at most four resolves run")
        fake.scans[1].fail("manual fallback")
        drain()
        check(fake.stopped == 2 && errors == ["manual fallback"] && hosts.isEmpty &&
              (6..<10).allSatisfy(fake.cancelled.contains), "failure cancels all work and reports manual fallback")
        print("PASS \(checks) Mac v2 discovery checks; fake Bonjour and numeric address parsing only")
    }

    private static func drain() {
        var complete = false
        DispatchQueue.main.async { complete = true }
        while !complete { RunLoop.main.run(until: Date(timeIntervalSinceNow: 0.001)) }
    }

    private static func v4(_ text: String, port: UInt16 = 24891) -> Data {
        var address = sockaddr_in()
        address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        address.sin_family = sa_family_t(AF_INET)
        address.sin_port = port.bigEndian
        precondition(inet_pton(AF_INET, text, &address.sin_addr) == 1)
        return Data(bytes: &address, count: MemoryLayout<sockaddr_in>.size)
    }

    private static func v6(_ text: String, scope: UInt32 = 0) -> Data {
        var address = sockaddr_in6()
        address.sin6_len = UInt8(MemoryLayout<sockaddr_in6>.size)
        address.sin6_family = sa_family_t(AF_INET6)
        address.sin6_port = UInt16(24891).bigEndian
        address.sin6_scope_id = scope
        precondition(inet_pton(AF_INET6, text, &address.sin6_addr) == 1)
        return Data(bytes: &address, count: MemoryLayout<sockaddr_in6>.size)
    }

    private static func numericAddress(_ host: V2DiscoveredHost) -> Bool {
        var hints = addrinfo()
        hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV
        hints.ai_family = AF_UNSPEC
        hints.ai_socktype = SOCK_STREAM
        hints.ai_protocol = IPPROTO_TCP
        var result: UnsafeMutablePointer<addrinfo>?
        guard getaddrinfo(host.host, String(host.port), &hints, &result) == 0, let result else { return false }
        freeaddrinfo(result)
        return true
    }
}
