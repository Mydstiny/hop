import Darwin
import Foundation
import Network
import dnssd

/// A Bonjour name locates a service. Identity still comes from the pinned
/// certificate and the receiver's explicit pairing flow.
struct V2DiscoveryCandidate: Hashable {
    let name, domain, interface: String
    let interfaceIndex: UInt32

    init?(name: String, type: String, domain: String, interface: String = "", interfaceIndex: UInt32 = 0) {
        guard [V2Discovery.serviceType, V2Discovery.serviceType + "."].contains(type.lowercased()),
              ["local", "local."].contains(domain.lowercased()), Self.validName(name) else { return nil }
        self.name = name; self.domain = "local."; self.interface = interface; self.interfaceIndex = interfaceIndex
    }

    init?(endpoint: NWEndpoint, interface fallback: NWInterface? = nil) {
        guard case let .service(name, type, domain, specifiedInterface) = endpoint else { return nil }
        let interface = specifiedInterface ?? fallback
        guard interface?.type != .loopback else { return nil }
        self.init(name: name, type: type, domain: domain, interface: interface?.name ?? "",
                  interfaceIndex: interface.flatMap { UInt32(exactly: $0.index) } ?? 0)
    }

    fileprivate static func validName(_ name: String) -> Bool {
        !name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty && name.utf8.count <= 63 &&
            name.unicodeScalars.allSatisfy { scalar in
                let value = scalar.value
                return !(value < 0x20 || (0x7f...0x9f).contains(value) || value == 0x061c || value == 0x200e || value == 0x200f ||
                         (0x2028...0x202e).contains(value) || (0x2066...0x2069).contains(value))
            }
    }
}

struct V2DiscoveredHost: Hashable {
    let name, host: String
    let port: UInt16
}

/// Main-queue, explicitly started discovery. A browser result is verified only
/// as a v2 service hint and cannot establish or select a trust record.
final class V2Discovery {
    struct Advertisement {
        let candidate: V2DiscoveryCandidate
        let txt: Data
    }
    struct Resolution {
        let port: Int
        let txt: Data
        let addresses: [Data]
        var interfaceIndex: UInt32 = 0
    }

    typealias Cancel = () -> Void
    typealias Browse = (@escaping ([Advertisement]) -> Void, @escaping (String) -> Void) -> Cancel
    typealias Resolve = (V2DiscoveryCandidate, @escaping (Resolution?) -> Void) -> Cancel

    static let serviceType = "_hop2._tcp"
    private static let txt = Data([3, 0x76, 0x3d, 0x32]) // One v=2 entry, shared by all Hop publishers.
    private static let ownSuffix = "-" + UUID().uuidString.replacingOccurrences(of: "-", with: "").prefix(10).lowercased()
    private static let maxCandidates = 64
    private static let maxResolving = 4

    private struct Job {
        let id: UUID
        let cancel: Cancel
    }
    private let browse: Browse
    private let resolve: Resolve
    private let localIPs: () -> Set<String>
    private var run: UUID?
    private var cancelBrowse: Cancel?
    private var candidates: [V2DiscoveryCandidate] = []
    private var attempted = Set<V2DiscoveryCandidate>()
    private var pending: [V2DiscoveryCandidate: Job] = [:]
    private var resolved: [V2DiscoveryCandidate: V2DiscoveredHost] = [:]
    private var visible: [V2DiscoveredHost] = []

    var onHosts: (([V2DiscoveredHost]) -> Void)?
    var onError: ((String) -> Void)?

    init(browse: Browse? = nil, resolve: Resolve? = nil, localIPs: (() -> Set<String>)? = nil) {
        self.browse = browse ?? Self.browseBonjour
        self.resolve = resolve ?? Self.resolveBonjour
        self.localIPs = localIPs ?? Self.localInterfaceIPs
    }

    /// Use this publisher with `publish(options: .noAutoRename)`. Keep the
    /// TXT record compatible with Linux and use the per-process name suffix
    /// to exclude our own service before starting a resolve.
    static func advertisement(deviceName: String, port: UInt16) -> NetService {
        let service = NetService(domain: "local.", type: serviceType + ".", name: advertisedName(deviceName), port: Int32(port))
        service.setTXTRecord(txt)
        return service
    }

    static func announcement(name: String) -> NWListener.Service {
        NWListener.Service(name: advertisedName(name), type: serviceType, txtRecord: txt)
    }

    private static func advertisedName(_ deviceName: String) -> String {
        let base = V2DiscoveryCandidate.validName(deviceName) ? deviceName : "Hop Mac"
        let byteLimit = 63 - ownSuffix.utf8.count
        var label = ""
        for scalar in base.unicodeScalars {
            guard label.utf8.count + String(scalar).utf8.count <= byteLimit else { break }
            label.unicodeScalars.append(scalar)
        }
        return label + ownSuffix
    }

    func start() {
        dispatchPrecondition(condition: .onQueue(.main))
        guard run == nil else { return }
        let id = UUID()
        run = id
        cancelBrowse = browse({ [weak self] advertisements in
            DispatchQueue.main.async { [weak self] in
                guard let self, self.run == id else { return }
                self.update(advertisements, run: id)
            }
        }, { [weak self] message in
            DispatchQueue.main.async { [weak self] in
                guard let self, self.run == id else { return }
                self.stop()
                self.onError?(message)
            }
        })
    }

    func stop() {
        dispatchPrecondition(condition: .onQueue(.main))
        cancel()
        publish()
    }

    deinit {
        let cancellations = [cancelBrowse].compactMap { $0 } + pending.values.map(\.cancel)
        if Thread.isMainThread { cancellations.forEach { $0() } }
        else { DispatchQueue.main.async { cancellations.forEach { $0() } } }
    }

    private func cancel() {
        run = nil
        cancelBrowse?(); cancelBrowse = nil
        let jobs = pending.values
        pending.removeAll(); resolved.removeAll(); candidates.removeAll(); attempted.removeAll()
        for job in jobs { job.cancel() }
    }

    private func update(_ advertisements: [Advertisement], run: UUID) {
        var seen = Set<V2DiscoveryCandidate>()
        candidates = advertisements.prefix(Self.maxCandidates * 2).compactMap { advertisement in
            let candidate = advertisement.candidate
            guard advertisement.txt == Self.txt, !candidate.name.lowercased().hasSuffix(Self.ownSuffix),
                  seen.count < Self.maxCandidates, seen.insert(candidate).inserted else { return nil }
            return candidate
        }
        for candidate in Array(pending.keys) where !seen.contains(candidate) {
            pending.removeValue(forKey: candidate)?.cancel()
        }
        resolved = resolved.filter { seen.contains($0.key) }
        attempted.formIntersection(seen)
        publish()
        resolveAvailable(run: run)
    }

    private func resolveAvailable(run: UUID) {
        guard self.run == run else { return }
        for candidate in candidates where pending.count < Self.maxResolving && attempted.insert(candidate).inserted {
            let ticket = UUID()
            let cancel = resolve(candidate) { [weak self] result in
                DispatchQueue.main.async { [weak self] in
                    guard let self, self.run == run, self.pending[candidate]?.id == ticket else { return }
                    self.pending.removeValue(forKey: candidate)?.cancel()
                    if let result, let host = self.validated(result, for: candidate) { self.resolved[candidate] = host }
                    self.publish()
                    self.resolveAvailable(run: run)
                }
            }
            pending[candidate] = Job(id: ticket, cancel: cancel)
        }
    }

    private func publish() {
        let hosts = Set(resolved.values).sorted { ($0.name, $0.host, $0.port) < ($1.name, $1.host, $1.port) }
        guard hosts != visible else { return }
        visible = hosts
        onHosts?(hosts)
    }

    private func validated(_ result: Resolution, for candidate: V2DiscoveryCandidate) -> V2DiscoveredHost? {
        guard result.txt == Self.txt,
              let port = UInt16(exactly: result.port), port != 0, (1...32).contains(result.addresses.count) else { return nil }
        let local = localIPs()
        let scope = candidate.interfaceIndex == 0 ? result.interfaceIndex : candidate.interfaceIndex
        let addresses = result.addresses.compactMap { IP.decode($0, fallbackScope: scope) }
            .filter { $0.port == port && !local.contains($0.key) }
        guard let ip = addresses.min(by: { ($0.rank, $0.host) < ($1.rank, $1.host) }) else { return nil }
        return V2DiscoveredHost(name: candidate.name, host: ip.host, port: port)
    }

    private static func browseBonjour(_ update: @escaping ([Advertisement]) -> Void, _ fail: @escaping (String) -> Void) -> Cancel {
        let browser = NWBrowser(for: .bonjourWithTXTRecord(type: serviceType, domain: "local."), using: .tcp)
        browser.browseResultsChangedHandler = { results, _ in
            let found = results.prefix(maxCandidates * 2).compactMap { result -> Advertisement? in
                let interface = result.interfaces.first { $0.type != .loopback }
                guard case .bonjour(let record) = result.metadata, record.data == txt,
                      result.interfaces.isEmpty || interface != nil,
                      let candidate = V2DiscoveryCandidate(endpoint: result.endpoint, interface: interface) else { return nil }
                return Advertisement(candidate: candidate, txt: record.data)
            }
            update(found.sorted { ($0.candidate.name, $0.candidate.interface) < ($1.candidate.name, $1.candidate.interface) })
        }
        browser.stateUpdateHandler = { state in
            switch state {
            case .failed(let error), .waiting(let error): fail(error.localizedDescription)
            default: break
            }
        }
        browser.start(queue: .main)
        return { browser.cancel() }
    }

    private static func resolveBonjour(_ candidate: V2DiscoveryCandidate, _ completion: @escaping (Resolution?) -> Void) -> Cancel {
        let job = BonjourResolution(candidate, completion: completion)
        job.start()
        return { job.cancel() }
    }

    private final class BonjourResolution {
        private let candidate: V2DiscoveryCandidate
        private var completion: ((Resolution?) -> Void)?
        private var ref: DNSServiceRef?
        private var deadline: DispatchWorkItem?
        private var port: UInt16 = 0
        private var interfaceIndex: UInt32 = 0
        private var addresses: [Data] = []

        init(_ candidate: V2DiscoveryCandidate, completion: @escaping (Resolution?) -> Void) {
            self.candidate = candidate
            self.completion = completion
        }

        func start() {
            let deadline = DispatchWorkItem { [weak self] in self?.finish(nil) }
            self.deadline = deadline
            DispatchQueue.main.asyncAfter(deadline: .now() + .seconds(3), execute: deadline)
            var next: DNSServiceRef?
            let status = DNSServiceResolve(&next, DNSServiceFlags(kDNSServiceFlagsForceMulticast), candidate.interfaceIndex,
                                           candidate.name, serviceType, candidate.domain,
                                           { _, _, interfaceIndex, error, _, hostname, port, txtLength, txtBytes, context in
                guard let context else { return }
                let job = Unmanaged<BonjourResolution>.fromOpaque(context).takeUnretainedValue()
                job.resolved(error: error, hostname: hostname, wirePort: port, txtLength: txtLength, txtBytes: txtBytes, interface: interfaceIndex)
            }, Unmanaged.passUnretained(self).toOpaque())
            install(next, status: status)
        }

        func cancel() {
            completion = nil
            deadline?.cancel(); deadline = nil
            stopQuery()
        }

        private func resolved(error: DNSServiceErrorType, hostname: UnsafePointer<CChar>?, wirePort: UInt16,
                              txtLength: UInt16, txtBytes: UnsafePointer<UInt8>?, interface: UInt32) {
            guard error == kDNSServiceErr_NoError, wirePort != 0, let hostname,
                  let target = String(validatingCString: hostname), isLocalResolveTarget(target),
                  Int(txtLength) == txt.count, let txtBytes, Data(bytes: txtBytes, count: Int(txtLength)) == txt,
                  candidate.interfaceIndex == 0 || candidate.interfaceIndex == interface else { finish(nil); return }
            port = UInt16(bigEndian: wirePort)
            interfaceIndex = interface
            stopQuery()
            var next: DNSServiceRef?
            let families = DNSServiceProtocol(kDNSServiceProtocol_IPv4 | kDNSServiceProtocol_IPv6)
            let status = DNSServiceGetAddrInfo(&next, DNSServiceFlags(kDNSServiceFlagsForceMulticast), interface, families, target,
                                               { _, flags, index, error, _, address, _, context in
                guard let context else { return }
                let job = Unmanaged<BonjourResolution>.fromOpaque(context).takeUnretainedValue()
                job.address(error: error, flags: flags, interface: index, address: address)
            }, Unmanaged.passUnretained(self).toOpaque())
            install(next, status: status)
        }

        private func address(error: DNSServiceErrorType, flags: DNSServiceFlags, interface: UInt32, address: UnsafePointer<sockaddr>?) {
            guard error == kDNSServiceErr_NoError else { finish(nil); return }
            if flags & DNSServiceFlags(kDNSServiceFlagsAdd) != 0, let address,
               interfaceIndex == 0 || interface == interfaceIndex, addresses.count < 32,
               let encoded = encodeAddress(address, interface: interface), IP.decode(encoded, fallbackScope: interface) != nil {
                addresses.append(encoded)
            }
            if flags & DNSServiceFlags(kDNSServiceFlagsMoreComing) == 0 && !addresses.isEmpty {
                finish(Resolution(port: Int(port), txt: txt, addresses: addresses, interfaceIndex: interfaceIndex))
            }
        }

        private func encodeAddress(_ address: UnsafePointer<sockaddr>, interface: UInt32) -> Data? {
            switch Int32(address.pointee.sa_family) {
            case AF_INET:
                guard Int(address.pointee.sa_len) == MemoryLayout<sockaddr_in>.size else { return nil }
                var ipv4 = UnsafeRawPointer(address).loadUnaligned(as: sockaddr_in.self)
                ipv4.sin_port = port.bigEndian
                return withUnsafeBytes(of: &ipv4) { Data($0) }
            case AF_INET6:
                guard Int(address.pointee.sa_len) == MemoryLayout<sockaddr_in6>.size else { return nil }
                var ipv6 = UnsafeRawPointer(address).loadUnaligned(as: sockaddr_in6.self)
                ipv6.sin6_port = port.bigEndian
                let linkLocal = withUnsafeBytes(of: ipv6.sin6_addr) { $0[0] == 0xfe && $0[1] & 0xc0 == 0x80 }
                if linkLocal && ipv6.sin6_scope_id == 0 { ipv6.sin6_scope_id = interface }
                return withUnsafeBytes(of: &ipv6) { Data($0) }
            default:
                return nil
            }
        }

        private func install(_ next: DNSServiceRef?, status: DNSServiceErrorType) {
            guard status == kDNSServiceErr_NoError, let next else { finish(nil); return }
            ref = next
            if DNSServiceSetDispatchQueue(next, .main) != kDNSServiceErr_NoError { finish(nil) }
        }

        private func stopQuery() {
            if let ref { DNSServiceRefDeallocate(ref); self.ref = nil }
        }

        private func finish(_ result: Resolution?) {
            let callback = completion
            cancel()
            callback?(result)
        }
    }

    static func isLocalResolveTarget(_ target: String) -> Bool {
        guard target.utf8.count <= 255, target.lowercased().hasSuffix(".local.") else { return false }
        return target.dropLast().split(separator: ".", omittingEmptySubsequences: false).allSatisfy { label in
            (1...63).contains(label.utf8.count) && label.utf8.allSatisfy {
                (48...57).contains($0) || (65...90).contains($0) || (97...122).contains($0) || $0 == 45 || $0 == 95
            }
        }
    }

    private struct IP {
        let key, host: String
        let port: UInt16
        let rank: Int

        static func decode(_ data: Data, fallbackScope: UInt32) -> IP? {
            data.withUnsafeBytes { bytes in
                guard bytes.count >= 2 else { return nil }
                switch Int32(bytes[1]) {
                case AF_INET:
                    guard Int(bytes[0]) == MemoryLayout<sockaddr_in>.size, bytes.count >= Int(bytes[0]) else { return nil }
                    let address = bytes.loadUnaligned(as: sockaddr_in.self)
                    return v4(address.sin_addr, port: UInt16(bigEndian: address.sin_port))
                case AF_INET6:
                    guard Int(bytes[0]) == MemoryLayout<sockaddr_in6>.size, bytes.count >= Int(bytes[0]) else { return nil }
                    let address = bytes.loadUnaligned(as: sockaddr_in6.self)
                    return v6(address.sin6_addr, port: UInt16(bigEndian: address.sin6_port), scope: address.sin6_scope_id, fallback: fallbackScope)
                default: return nil
                }
            }
        }

        private static func v4(_ address: in_addr, port: UInt16) -> IP? {
            var address = address
            let first = UInt32(bigEndian: address.s_addr) >> 24
            guard first > 0, first != 127, first < 224,
                  let text = numeric(AF_INET, &address) else { return nil }
            return IP(key: text, host: text, port: port, rank: 0)
        }

        private static func v6(_ address: in6_addr, port: UInt16, scope: UInt32, fallback: UInt32) -> IP? {
            var address = address
            let bytes = withUnsafeBytes(of: &address) { Array($0) }
            if bytes.prefix(10).allSatisfy({ $0 == 0 }) && bytes[10] == 255 && bytes[11] == 255 {
                let value = bytes.suffix(4).reduce(UInt32(0)) { ($0 << 8) | UInt32($1) }
                return v4(in_addr(s_addr: value.bigEndian), port: port)
            }
            guard bytes[0] != 255, !bytes.prefix(15).allSatisfy({ $0 == 0 }) || bytes[15] > 1,
                  let text = numeric(AF_INET6, &address) else { return nil }
            let linkLocal = bytes[0] == 0xfe && bytes[1] & 0xc0 == 0x80
            if linkLocal {
                let index = scope == 0 ? fallback : scope
                guard index != 0, index <= UInt32(Int32.max), fallback == 0 || fallback == index else { return nil }
                return IP(key: text, host: "\(text)%\(index)", port: port, rank: 2)
            }
            guard scope == 0 else { return nil }
            return IP(key: text, host: text, port: port, rank: 1)
        }

        private static func numeric<T>(_ family: Int32, _ address: inout T) -> String? {
            var text = [CChar](repeating: 0, count: Int(INET6_ADDRSTRLEN))
            let size = socklen_t(text.count)
            guard withUnsafeBytes(of: &address, { inet_ntop(family, $0.baseAddress!, &text, size) }) != nil else { return nil }
            return String(cString: text)
        }
    }

    private static func localInterfaceIPs() -> Set<String> {
        var first: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&first) == 0, let first else { return [] }
        defer { freeifaddrs(first) }
        var local = Set<String>()
        var cursor: UnsafeMutablePointer<ifaddrs>? = first
        while let current = cursor {
            let entry = current.pointee
            cursor = entry.ifa_next
            guard let address = entry.ifa_addr else { continue }
            let family = Int32(address.pointee.sa_family)
            guard family == AF_INET || family == AF_INET6 else { continue }
            let data = Data(bytes: address, count: Int(address.pointee.sa_len))
            if let ip = IP.decode(data, fallbackScope: if_nametoindex(entry.ifa_name)) { local.insert(ip.key) }
        }
        return local
    }
}
