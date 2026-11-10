import Foundation
import Darwin

/// One explicitly requested public candidate exchange, never a trust decision.
/// The owner bounds concurrent exchanges, keeps this object alive through completion, and must independently
/// compare both fingerprints and complete pinned mutual TLS before saving trust.
final class V2OfferExchange {
    typealias Completion = (Result<V2PairOffer, Error>) -> Void
    private let queue = DispatchQueue(label: "local.hop.v2.offer")
    private let queueKey = DispatchSpecificKey<Bool>()
    private let completion: Completion
    private let local: V2PairOffer
    private var fd: Int32
    private var reader: DispatchSourceRead?
    private var writer: DispatchSourceWrite?
    private var timeout: DispatchSourceTimer?
    private var decoder = V2PairOfferDecoder()
    private var header = [UInt8]()
    private var received = 0, expected: Int?
    private var output = [UInt8](), sent = 0
    private var candidate: V2PairOffer?
    private var finished = false
    private var deadline: TimeInterval = 0
    /// Takes ownership of an already nonblocking descriptor and the four HPOP
    /// bytes consumed by V2IncomingListener. The local offer is public data.
    init?(fd: Int32, prefix: [UInt8], local: V2PairOffer, completion: @escaping Completion) {
        let flags = fd >= 0 ? fcntl(fd, F_GETFL) : -1
        let descriptorFlags = fd >= 0 ? fcntl(fd, F_GETFD) : -1
        guard fd >= 0, flags >= 0, flags & O_NONBLOCK != 0, descriptorFlags >= 0, descriptorFlags & FD_CLOEXEC != 0,
              prefix == [0x48,0x50,0x4f,0x50], local.valid, let bytes = local.encoded() else {
            if fd >= 0 { Darwin.close(fd) }; return nil
        }
        self.fd = fd; self.local = local; self.completion = completion; output = bytes
        header = prefix; received = prefix.count
        queue.setSpecific(key: queueKey, value: true)
        guard decoder.feed(prefix) else { Darwin.close(fd); self.fd = -1; return nil }
    }
    deinit { cancel() }
    private func onQueue<T>(_ work: () -> T) -> T {
        if DispatchQueue.getSpecific(key: queueKey) != nil { return work() }
        return queue.sync(execute: work)
    }
    func start() {
        onQueue {
            guard !finished && reader == nil && writer == nil else { return }
            var one: Int32 = 1
            guard setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size)) == 0 else {
                finish(.failure(WireError.invalid("Cannot configure candidate exchange"))); return
            }
            let r = DispatchSource.makeReadSource(fileDescriptor: fd, queue: queue)
            let t = DispatchSource.makeTimerSource(queue: queue)
            reader = r; timeout = t
            deadline = ProcessInfo.processInfo.systemUptime + 10
            r.setEventHandler { [weak self] in self?.readAvailable() }
            t.schedule(deadline: .now() + .seconds(10))
            t.setEventHandler { [weak self] in self?.finish(.failure(WireError.invalid("Candidate exchange timed out"))) }
            r.resume(); t.resume()
        }
    }
    func cancel() {
        onQueue {
            guard !finished else { return }
            finished = true; cleanup()
        }
    }
    private func cleanup() {
        reader?.cancel(); reader = nil; writer?.cancel(); writer = nil; timeout?.cancel(); timeout = nil
        if fd >= 0 { Darwin.close(fd); fd = -1 }
        output.removeAll(); header.removeAll(); candidate = nil
    }
    private func finish(_ result: Result<V2PairOffer, Error>) {
        guard !finished else { return }
        finished = true; cleanup(); completion(result)
    }
    private func readAvailable() {
        guard !finished && fd >= 0 && reader != nil else { return }
        guard ProcessInfo.processInfo.systemUptime < deadline else { finish(.failure(WireError.invalid("Candidate exchange timed out"))); return }
        if candidate != nil { waitForPeerEOF(); return }
        // Read at most the remaining header/body so an oversized peer cannot
        // force an unbounded allocation or keep a readable partial prefix busy.
        for _ in 0..<8 {
            let need = expected.map { $0 - received } ?? (V2PairOffer.headerSize - received)
            guard need > 0 else { finish(.failure(WireError.invalid("Invalid candidate size"))); return }
            var bytes = [UInt8](repeating: 0, count: min(need, 4096))
            let count = bytes.count
            let n = bytes.withUnsafeMutableBytes { recv(fd, $0.baseAddress, count, 0) }
            if n < 0 && (errno == EAGAIN || errno == EINTR) { return }
            guard n > 0 else { finish(.failure(WireError.invalid("Candidate disconnected"))); return }
            let fragment = Array(bytes.prefix(n)); received += n
            guard decoder.feed(fragment) else { finish(.failure(WireError.invalid("Invalid candidate"))); return }
            if expected == nil {
                header += fragment
                if header.count == V2PairOffer.headerSize {
                    let name = Int(header[6]) << 8 | Int(header[7])
                    let cert = header[8..<12].reduce(0) { ($0 << 8) | Int($1) }
                    guard (1...V2PairOffer.maxName).contains(name), (1...V2PairOffer.maxCertificate).contains(cert) else {
                        finish(.failure(WireError.invalid("Invalid candidate size"))); return
                    }
                    expected = V2PairOffer.headerSize + name + cert
                }
            }
            if let target = expected, received == target {
                guard let offer = decoder.finish(), offer.role != local.role,
                      let other = V2Certificate.fingerprint(offer.certificatePEM),
                      let own = V2Certificate.fingerprint(local.certificatePEM), other != own else {
                    finish(.failure(WireError.invalid("Invalid candidate identity"))); return
                }
                candidate = offer; waitForPeerEOF()
                return
            }
        }
    }
    private func waitForPeerEOF() {
        var extra: UInt8 = 0
        let n = recv(fd, &extra, 1, 0)
        if n < 0 && (errno == EAGAIN || errno == EINTR) { return }
        guard n == 0 else { finish(.failure(WireError.invalid("Candidate contains trailing data"))); return }
        reader?.cancel(); reader = nil
        let w = DispatchSource.makeWriteSource(fileDescriptor: fd, queue: queue)
        writer = w; w.setEventHandler { [weak self] in self?.writeAvailable() }; w.resume()
    }
    private func writeAvailable() {
        guard !finished && fd >= 0 && writer != nil, let offer = candidate else { return }
        guard ProcessInfo.processInfo.systemUptime < deadline else { finish(.failure(WireError.invalid("Candidate exchange timed out"))); return }
        let n = output.withUnsafeBytes { raw in send(fd, raw.baseAddress!.advanced(by: sent), output.count - sent, 0) }
        if n < 0 && (errno == EAGAIN || errno == EINTR) { return }
        guard n > 0 else { finish(.failure(WireError.invalid("Candidate response failed"))); return }
        sent += n
        if sent == output.count { finish(.success(offer)) }
    }
}
