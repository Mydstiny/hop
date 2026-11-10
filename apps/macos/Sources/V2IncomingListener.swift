import Foundation
import Darwin

/// Owns only the untrusted TCP preface. A selector is a lookup hint, not
/// authentication; an offer is accepted only during an explicit pairing window.
/// The recipient owns the handed-off nonblocking descriptor and must perform
/// bounded parsing and a pinned TLS handshake before trusting or injecting input.
final class V2IncomingListener {
    enum Kind { case offer, selector }
    typealias Handler = (Int32, Kind, [UInt8]) -> Void
    private final class Pending {
        let source: DispatchSourceRead
        let timeout: DispatchSourceTimer
        var prefix: [UInt8] = []
        init(source: DispatchSourceRead, timeout: DispatchSourceTimer) { self.source = source; self.timeout = timeout }
    }
    private let queue = DispatchQueue(label: "local.hop.v2.incoming")
    private let queueKey = DispatchSpecificKey<Bool>()
    private let handler: Handler
    private var server: Int32 = -1
    private var acceptSource: DispatchSourceRead?
    private var pending: [Int32: Pending] = [:]
    private var pairingUntil: TimeInterval = 0
    init(handler: @escaping Handler) {
        self.handler = handler
        queue.setSpecific(key: queueKey, value: true)
    }
    deinit { stop() }
    private func onQueue<T>(_ body: () throws -> T) rethrows -> T {
        if DispatchQueue.getSpecific(key: queueKey) != nil { return try body() }
        return try queue.sync(execute: body)
    }
    private static func prepare(_ fd: Int32) -> Bool {
        let status = fcntl(fd, F_GETFL), descriptor = fcntl(fd, F_GETFD)
        return status >= 0 && descriptor >= 0 && fcntl(fd, F_SETFL, status | O_NONBLOCK) == 0 &&
            fcntl(fd, F_SETFD, descriptor | FD_CLOEXEC) == 0
    }
    /// Port zero is for isolated tests only; production callers specify 24891.
    @discardableResult func start(port: UInt16) throws -> UInt16 {
        try onQueue {
            guard server < 0 else { throw WireError.invalid("v2 listener already running") }
            let fd = socket(AF_INET6, SOCK_STREAM, 0)
            guard fd >= 0 else { throw WireError.invalid("Cannot create v2 listener") }
            var transferred = false
            defer { if !transferred { Darwin.close(fd) } }
            var one: Int32 = 1, zero: Int32 = 0
            guard Self.prepare(fd), setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, socklen_t(MemoryLayout<Int32>.size)) == 0,
                  setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, socklen_t(MemoryLayout<Int32>.size)) == 0 else {
                throw WireError.invalid("Cannot configure v2 listener")
            }
            var addr = sockaddr_in6(); addr.sin6_len = UInt8(MemoryLayout<sockaddr_in6>.size)
            addr.sin6_family = sa_family_t(AF_INET6); addr.sin6_port = port.bigEndian; addr.sin6_addr = in6addr_any
            let bound = withUnsafePointer(to: &addr) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in6>.size)) } }
            guard bound == 0, listen(fd, 8) == 0 else { throw WireError.invalid("Cannot bind v2 listener") }
            var actual = sockaddr_in6(), length = socklen_t(MemoryLayout<sockaddr_in6>.size)
            let got = withUnsafeMutablePointer(to: &actual) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { getsockname(fd, $0, &length) } }
            guard got == 0 else { throw WireError.invalid("Cannot inspect v2 listener") }
            let source = DispatchSource.makeReadSource(fileDescriptor: fd, queue: queue)
            source.setEventHandler { [weak self] in self?.acceptPending() }
            server = fd; acceptSource = source; transferred = true; source.resume()
            return UInt16(bigEndian: actual.sin6_port)
        }
    }
    func openPairingWindow(seconds: TimeInterval = 120) {
        onQueue { pairingUntil = seconds > 0 && seconds.isFinite ? ProcessInfo.processInfo.systemUptime + min(seconds, 120) : 0 }
    }
    func closePairingWindow() { onQueue { pairingUntil = 0 } }
    func stop() {
        onQueue {
            pairingUntil = 0; acceptSource?.cancel(); acceptSource = nil
            if server >= 0 { Darwin.close(server); server = -1 }
            for fd in Array(pending.keys) { closePending(fd) }
        }
    }
    private func acceptPending() {
        guard server >= 0 else { return }
        for _ in 0..<16 {
            let fd = accept(server, nil, nil)
            if fd < 0 { return }
            guard pending.count < 8 && Self.prepare(fd) else { Darwin.close(fd); continue }
            let source = DispatchSource.makeReadSource(fileDescriptor: fd, queue: queue)
            let timeout = DispatchSource.makeTimerSource(queue: queue)
            let entry = Pending(source: source, timeout: timeout)
            pending[fd] = entry
            source.setEventHandler { [weak self, weak entry] in
                if let entry { self?.classify(fd, expected: entry) }
            }
            timeout.schedule(deadline: .now() + .seconds(10))
            timeout.setEventHandler { [weak self, weak entry] in
                if let entry { self?.closePending(fd, expected: entry) }
            }
            source.resume(); timeout.resume()
        }
    }
    private func closePending(_ fd: Int32, expected: Pending? = nil) {
        guard let entry = pending[fd], expected == nil || expected === entry else { return }
        pending.removeValue(forKey: fd)
        entry.source.cancel(); entry.timeout.cancel(); Darwin.close(fd)
    }
    private func classify(_ fd: Int32, expected: Pending) {
        guard let entry = pending[fd], entry === expected else { return }
        let remaining = 4 - entry.prefix.count
        var fragment = [UInt8](repeating: 0, count: remaining)
        let n = fragment.withUnsafeMutableBytes { recv(fd, $0.baseAddress, remaining, 0) }
        if n < 0 && (errno == EAGAIN || errno == EINTR) { return }
        if n == 0 || n < 0 { closePending(fd); return }
        entry.prefix.append(contentsOf: fragment.prefix(n))
        if entry.prefix.count < 4 { return }
        let kind: Kind
        if entry.prefix == [0x48,0x50,0x4f,0x50] && ProcessInfo.processInfo.systemUptime < pairingUntil { kind = .offer }
        else if entry.prefix == [0x48,0x50,0x53,0x4c] { kind = .selector }
        else { closePending(fd); return }
        pending.removeValue(forKey: fd); entry.source.cancel(); entry.timeout.cancel()
        handler(fd, kind, entry.prefix)
    }
}
