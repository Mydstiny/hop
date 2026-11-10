import Foundation

/// Separates pasteboard I/O and decoding from the authenticated session
/// worker. The lock protects only an epoch lease and a cached version.
final class V2ClipboardActor {
    private let queue = DispatchQueue(label: "local.hop.v2.clipboard", qos: .utility)
    private let backend: () -> HopClipboardBackend
    private var board: HopClipboardBackend?
    private var sync: HopClipboardSync?
    private let gate = NSLock()
    private var enabled = false
    private var permittedEpoch: UInt64?
    private var sessionPermits: () -> Bool = { true }
    private var cachedVersion: UInt64?
    private var preflight: Int?
    private var priming = false
    private var pendingRevocations = 0
    private var pendingApplies = 0
    private var pendingBytes = 0

    init(backend: @escaping () -> HopClipboardBackend) { self.backend = backend }

    func setEnabled(_ value: Bool) {
        gate.withLock {
            enabled = value
            if !value { permittedEpoch = nil; preflight = nil }
        }
    }

    func prepare(peer: String, epoch: UInt64, initialSnapshot: Bool = true, baseline: Int? = nil,
                 sessionPermits: @escaping () -> Bool = { true }, completion: @escaping (Bool) -> Void) -> Bool {
        let accepted = gate.withLock {
            guard enabled, pendingRevocations == 0 else { return false }
            permittedEpoch = epoch
            self.sessionPermits = sessionPermits
            cachedVersion = nil
            return true
        }
        guard accepted else { return false }
        queue.async {
            guard self.permitted(epoch) else { return }
            let sync = self.sync ?? HopClipboardSync(backend: self.getBoard())
            self.sync = sync
            guard self.permitted(epoch) else { return }
            sync.configure(enabled: true, verified: true, peer: peer, epoch: epoch, initialSnapshot: initialSnapshot, baseline: baseline)
            self.gate.withLock {
                if self.permittedEpoch == epoch, sync.sampledChangeCount != nil { self.cachedVersion = 1 }
            }
            completion(self.sample() != nil && self.permitted(epoch))
        }
        return true
    }

    /// Receiver-only, after pinned trust and before focus. Samples a version
    /// without reading contents so ENTER can protect edits without blocking.
    func primeReceiverBaseline(allowed: @escaping () -> Bool) {
        let accepted = gate.withLock {
            guard enabled, permittedEpoch == nil, preflight == nil, !priming, pendingRevocations == 0 else { return false }
            priming = true
            return true
        }
        guard accepted else { return }
        queue.async {
            defer { self.gate.withLock { self.priming = false } }
            guard allowed(), self.gate.withLock({ self.enabled && self.permittedEpoch == nil }) else { return }
            let version = self.getBoard().changeCount
            guard allowed() else { return }
            self.gate.withLock {
                if self.enabled && self.permittedEpoch == nil { self.preflight = version }
            }
        }
    }

    func takeReceiverBaseline() -> Int? {
        gate.withLock {
            defer { preflight = nil }
            return preflight
        }
    }

    func barrier(_ completion: @escaping () -> Void) { queue.async { completion() } }

    func poll(epoch: UInt64, completion: @escaping (HopClipboardPayload?, Bool) -> Void) {
        queue.async {
            guard self.permitted(epoch), let sync = self.sync else { completion(nil, false); return }
            let previous = sync.sampledChangeCount
            let payload = sync.poll(epoch: epoch, permitted: { self.permitted(epoch) })
            if sync.sampledChangeCount != previous {
                self.gate.withLock {
                    if self.permittedEpoch == epoch, let version = self.cachedVersion, version < UInt64.max {
                        self.cachedVersion = version + 1
                    }
                }
            }
            guard self.permitted(epoch) else { completion(nil, false); return }
            completion(payload, !sync.initialSnapshotPending)
        }
    }

    /// Opaque local-edit generation from the last observation on the actor.
    /// A remote write updates the OS change count without becoming a local
    /// edit or invalidating a second transfer already received from that peer.
    func sample() -> UInt64? { gate.withLock { cachedVersion } }

    func accept(format: UInt8, peer: String, epoch: UInt64, bytes: UnsafeBufferPointer<UInt8>, baseline: UInt64,
                writeFailed: @escaping () -> Void) -> Bool {
        guard (format == 1 || format == 2) else { return false }
        gate.lock()
        guard permittedEpoch == epoch else { gate.unlock(); return true }
        guard pendingApplies < 2, bytes.count <= HopClipboardValidation.maxImage - pendingBytes else { gate.unlock(); return false }
        pendingApplies += 1
        pendingBytes += bytes.count
        gate.unlock()

        // The C callback borrows the assembled buffer. Own one bounded copy
        // before returning; validation and pasteboard access stay on the actor.
        let data = Data(buffer: bytes)
        queue.async {
            defer { self.gate.withLock { self.pendingApplies -= 1; self.pendingBytes -= data.count } }
            guard self.permitted(epoch), self.sample() == baseline, let sync = self.sync else { return }
            let payload: HopClipboardPayload = format == 1 ? .text(data) : .png(data)
            sync.apply(payload, from: peer, epoch: epoch, permitted: { self.permitted(epoch) })
            if !sync.active && self.permitted(epoch) { writeFailed() }
        }
        return true
    }

    func invalidate() { gate.withLock { permittedEpoch = nil; preflight = nil } }

    /// The lease is invalid immediately. Completion follows all prior work,
    /// including any write that had already passed its final commit check.
    func revoke(completion: @escaping () -> Void) {
        gate.withLock { permittedEpoch = nil; preflight = nil; pendingRevocations += 1 }
        queue.async {
            self.sync?.configure(enabled: false, verified: false, peer: "", epoch: 0)
            self.gate.withLock {
                self.cachedVersion = nil
                self.sessionPermits = { true }
                self.pendingRevocations -= 1
            }
            completion()
        }
    }

    func permitted(_ epoch: UInt64) -> Bool {
        let check = gate.withLock { permittedEpoch == epoch ? sessionPermits : nil }
        return check?() == true
    }

    private func getBoard() -> HopClipboardBackend {
        if let board { return board }
        let created = backend()
        board = created
        return created
    }
}
