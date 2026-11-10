import Foundation
import Darwin

/// Reads the public selector after V2IncomingListener classified HPSL. The
/// fingerprint is only a lookup hint; the caller must resolve an already
/// trusted or explicitly approved candidate and still authenticate pinned TLS.
final class V2SelectorReader {
    struct Selection { let fd: Int32; let fingerprint: String }
    typealias Completion = (Result<Selection, Error>) -> Void
    private let queue = DispatchQueue(label: "local.hop.v2.selector")
    private let queueKey = DispatchSpecificKey<Bool>()
    private let completion: Completion
    private var fd: Int32
    private var reader: DispatchSourceRead?
    private var timeout: DispatchSourceTimer?
    private var bytes = [UInt8]()
    private var deadline: TimeInterval = 0
    private var finished = false
    /// Takes ownership of an already nonblocking, close-on-exec descriptor.
    init?(fd: Int32, prefix: [UInt8], completion: @escaping Completion) {
        let flags = fd >= 0 ? fcntl(fd, F_GETFL) : -1
        let descriptorFlags = fd >= 0 ? fcntl(fd, F_GETFD) : -1
        guard fd >= 0, flags >= 0, flags & O_NONBLOCK != 0, descriptorFlags >= 0, descriptorFlags & FD_CLOEXEC != 0,
              prefix == [0x48,0x50,0x53,0x4c] else {
            if fd >= 0 { Darwin.close(fd) }; return nil
        }
        self.fd = fd; self.completion = completion; bytes = prefix
        queue.setSpecific(key: queueKey, value: true)
    }
    deinit { cancel() }
    private func onQueue<T>(_ work: () -> T) -> T {
        if DispatchQueue.getSpecific(key: queueKey) != nil { return work() }
        return queue.sync(execute: work)
    }
    func start() {
        onQueue {
            guard !finished && reader == nil else { return }
            let r = DispatchSource.makeReadSource(fileDescriptor: fd, queue: queue)
            let t = DispatchSource.makeTimerSource(queue: queue)
            reader = r; timeout = t; deadline = ProcessInfo.processInfo.systemUptime + 5
            r.setEventHandler { [weak self] in self?.readAvailable() }
            t.schedule(deadline: .now() + .seconds(5))
            t.setEventHandler { [weak self] in self?.fail("Selector timed out") }
            r.resume(); t.resume()
        }
    }
    func cancel() { onQueue { if !finished { finished = true; cleanup(closeDescriptor: true) } } }
    private func cleanup(closeDescriptor: Bool) {
        reader?.cancel(); reader = nil; timeout?.cancel(); timeout = nil
        if closeDescriptor && fd >= 0 { Darwin.close(fd) }
        fd = -1; bytes.removeAll()
    }
    private func fail(_ reason: String) {
        guard !finished else { return }
        finished = true; cleanup(closeDescriptor: true); completion(.failure(WireError.invalid(reason)))
    }
    private func readAvailable() {
        guard !finished && fd >= 0 && reader != nil else { return }
        guard ProcessInfo.processInfo.systemUptime < deadline else { fail("Selector timed out"); return }
        let remaining = 68 - bytes.count
        guard remaining > 0 else { fail("Invalid selector"); return }
        var fragment = [UInt8](repeating: 0, count: remaining)
        let n = fragment.withUnsafeMutableBytes { recv(fd, $0.baseAddress, remaining, 0) }
        if n < 0 && (errno == EAGAIN || errno == EINTR) { return }
        guard n > 0 else { fail("Selector disconnected"); return }
        bytes.append(contentsOf: fragment.prefix(n))
        guard bytes.count == 68 else { return }
        guard let fingerprint = V2PairOffer.selectorFingerprint(bytes) else { fail("Invalid selector"); return }
        let handedOff = fd
        finished = true; cleanup(closeDescriptor: false)
        completion(.success(Selection(fd: handedOff, fingerprint: fingerprint)))
    }
}
