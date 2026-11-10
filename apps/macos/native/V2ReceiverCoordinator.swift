import Foundation
import Security
import Darwin

/// A destination entered by the user or resolved from a local service record.
struct V2ReceiverAddress: Equatable {
    let host: String
    let port: UInt16

    init(host: String, port: UInt16) throws {
        let host = host.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !host.isEmpty, host.utf8.count <= 255, !host.contains("\0"), port != 0 else {
            throw WireError.invalid("Invalid host address")
        }
        self.host = host
        self.port = port
    }
}

/// Holds a single receiving session. The worker alone owns descriptors, the
/// pinned TLS state and injected key state. Main-queue commands revoke its
/// input lease immediately and never wait for network I/O.
final class V2ReceiverCoordinator {
    struct Input {
        let screenIDs: [UInt64]
        let screenAdvertisement: [UInt8]
        let enter: ([UInt8], UInt64) -> Bool
        let input: ([UInt8], UInt64) -> Bool
        let restore: () -> Bool
        let takeLeave: () -> (UInt16, UInt16)?
    }
    struct Comparison {
        let peerName, peerFingerprint, localFingerprint: String
    }
    enum Event {
        case connecting
        case compare(Comparison)
        case ready(V2VerifiedPeer)
        case receiving
        case closed
        case failed(String)
    }

    private final class Job {
        private let condition = NSCondition()
        private var stopped = false
        private var approved = false
        var cancelled: Bool { condition.withLock { stopped } }
        func cancel() { condition.withLock { stopped = true; condition.broadcast() } }
        func confirm() { condition.withLock { approved = true; condition.broadcast() } }
        func takeConfirmation() -> Bool {
            condition.withLock {
                if stopped || !approved { return false }
                approved = false
                return true
            }
        }
        func waitForConfirmation(until deadline: UInt64) -> Bool {
            condition.lock()
            defer { condition.unlock() }
            while !stopped && !approved && V2ReceiverCoordinator.now() < deadline {
                _ = condition.wait(until: Date(timeIntervalSinceNow: 0.1))
            }
            if stopped || !approved || V2ReceiverCoordinator.now() >= deadline { return false }
            approved = false
            return true
        }
    }
    private final class Descriptor {
        private(set) var fd: Int32
        init(_ fd: Int32) { self.fd = fd }
        func take() -> Int32 { defer { fd = -1 }; return fd }
        deinit { if fd >= 0 { Darwin.close(fd) } }
    }

    private final class ClipboardReply<Value> {
        private let gate = NSLock()
        private var result: Value?
        var value: Value? { gate.withLock { result } }
        func finish(_ value: Value) { gate.withLock { result = value } }
        func finishOnce(_ value: Value) -> Bool {
            gate.withLock {
                guard result == nil else { return false }
                result = value
                return true
            }
        }
    }

    /// Session-worker state for one pinned receiving connection. Actor
    /// completions use bounded mailboxes because the worker owns a poll loop.
    private final class ReceiverClipboard {
        private final class Focus {
            let epoch: UInt64
            let prepared = ClipboardReply<Bool>()
            let writeFailed = ClipboardReply<Bool>()
            var ready = false
            var initialDrained = false
            var initialBarrier: ClipboardReply<Bool>?
            var poll: ClipboardReply<HopClipboardPayload?>?
            var lastPoll: UInt64?
            var outgoing: Data?
            var offset = 0
            var leased = false

            init(epoch: UInt64) { self.epoch = epoch }
            func discardOutgoing() {
                if let count = outgoing?.count { outgoing?.resetBytes(in: 0..<count) }
                outgoing = nil; offset = 0
            }
            deinit { discardOutgoing() }
        }

        private let actor: V2ClipboardActor
        private let peer: String
        private let allowed: () -> Bool
        private var focus: Focus?
        private var awaitingEnter = false
        private var baseline: Int?
        private var revocation: ClipboardReply<Bool>?
        private var transferID: UInt64 = 0

        init(actor: V2ClipboardActor, peer: String, allowed: @escaping () -> Bool) {
            self.actor = actor; self.peer = peer; self.allowed = allowed
        }

        var hooks: V2SessionBridge.ClipboardHooks {
            .init(revoke: { [weak self] in self?.revoke() }, sample: { [weak self] in
                guard let self else { return nil }
                if self.focus == nil {
                    self.awaitingEnter = true
                    self.baseline = self.actor.takeReceiverBaseline()
                    // Generation 0 can never match a real prepared actor.
                    return self.baseline == nil ? 0 : 1
                }
                return self.actor.sample() ?? 0
            }, apply: { [weak self] format, epoch, bytes, version in
                guard let self, let focus = self.focus, focus.epoch == epoch else { return true }
                return self.actor.accept(format: format, peer: self.peer, epoch: epoch, bytes: bytes, baseline: version) {
                    focus.writeFailed.finish(true)
                }
            })
        }

        func enter(epoch: UInt64) {
            guard awaitingEnter else { return }
            awaitingEnter = false
            let current = Focus(epoch: epoch)
            focus = current
            current.leased = actor.prepare(peer: peer, epoch: epoch, initialSnapshot: false, baseline: baseline, sessionPermits: allowed) {
                current.prepared.finish($0)
            }
            baseline = nil
        }

        func revoke() {
            guard let old = focus else { return }
            focus = nil
            old.discardOutgoing()
            if old.leased {
                let ticket = ClipboardReply<Bool>()
                revocation = ticket
                actor.revoke { ticket.finish(true) }
            }
        }

        @discardableResult
        func pauseIfRevoked(_ session: V2SessionBridge) throws -> Bool {
            guard session.phase == 6, let focus,
                  !focus.leased || !actor.permitted(focus.epoch) || focus.writeFailed.value == true else { return false }
            guard session.pauseClipboard() else { throw WireError.invalid("Could not safely pause v2 clipboard") }
            return true
        }

        func advance(_ session: V2SessionBridge, now: UInt64, advertised: Bool) throws {
            guard session.clipboardFormats != 0 else { return }
            if revocation?.value == true { revocation = nil }
            if session.phase == 9 { // release_ack_wait
                if revocation == nil, !session.completeRelease(now: now) { throw WireError.invalid("Could not finish v2 clipboard release") }
                return
            }
            if session.phase == 4 && advertised && revocation == nil {
                actor.primeReceiverBaseline(allowed: allowed)
                return
            }
            guard session.phase == 6, let focus, try !pauseIfRevoked(session) else { return }
            if !focus.ready {
                guard let prepared = focus.prepared.value else { return }
                guard prepared, session.readyClipboard() else { throw WireError.invalid("Could not activate v2 clipboard") }
                focus.ready = true
            }
            guard session.clipboardInitialReceived else { return }
            if !focus.initialDrained {
                if focus.initialBarrier == nil {
                    let ticket = ClipboardReply<Bool>()
                    focus.initialBarrier = ticket
                    actor.barrier { ticket.finish(true) }
                }
                guard focus.initialBarrier?.value == true else { return }
                guard session.drainInitialClipboard() else { throw WireError.invalid("Could not drain initial v2 clipboard") }
                focus.initialDrained = true
            }
            try sendNext(session, focus: focus, now: now)
        }

        private func sendNext(_ session: V2SessionBridge, focus: Focus, now: UInt64) throws {
            if let data = focus.outgoing {
                guard session.drainedWrites else { return }
                let sent = session.sendClipboardSlice(id: transferID, data: data, offset: focus.offset)
                guard sent > 0 else { throw WireError.invalid("Could not send v2 clipboard chunk") }
                focus.offset += sent
                if focus.offset == data.count { focus.discardOutgoing() }
                return
            }
            if let ticket = focus.poll {
                guard let payload = ticket.value else { return }
                focus.poll = nil
                guard let payload else { return }
                let format: UInt8
                let data: Data
                switch payload {
                case .text(let bytes): format = 1; data = bytes
                case .png(let bytes): format = 2; data = bytes
                }
                guard session.clipboardFormats & (1 << format) != 0 else { return }
                guard transferID < UInt64.max, let size = UInt32(exactly: data.count) else { throw WireError.invalid("Invalid v2 clipboard transfer") }
                transferID += 1
                guard session.startClipboard(id: transferID, format: format, total: size) else { throw WireError.invalid("Could not start v2 clipboard transfer") }
                if !data.isEmpty { focus.outgoing = data }
                return
            }
            if let last = focus.lastPoll, now - last < 100 { return }
            focus.lastPoll = now
            let ticket = ClipboardReply<HopClipboardPayload?>()
            focus.poll = ticket
            actor.poll(epoch: focus.epoch) { payload, _ in ticket.finish(payload) }
        }
    }

    private let worker = DispatchQueue(label: "local.hop.v2.receive", qos: .userInitiated)
    private let callbackQueue: DispatchQueue
    private let identity: V2LocalIdentity
    private let trust: V2TrustStore
    private let name: String
    private let onEvent: (Event) -> Void
    private let clipboardActor: V2ClipboardActor?
    private let gate = NSLock()
    private var job: Job?

    init(identity: V2LocalIdentity, name: String, trust: V2TrustStore, callbackQueue: DispatchQueue = .main,
         clipboardBackend: (() -> HopClipboardBackend)? = nil,
         onEvent: @escaping (Event) -> Void) {
        self.identity = identity; self.name = name; self.trust = trust
        self.callbackQueue = callbackQueue; self.onEvent = onEvent
        clipboardActor = clipboardBackend.map { V2ClipboardActor(backend: $0) }
    }

    @discardableResult
    func connect(address: V2ReceiverAddress, fingerprint: String?, input: Input) -> Bool {
        let next = Job()
        guard !input.screenIDs.isEmpty, !input.screenAdvertisement.isEmpty,
              gate.withLock({ () -> Bool in
                  guard job == nil else { return false }
                  job = next; return true
              }) else { return false }
        worker.async { [weak self] in self?.run(job: next, address: address, fingerprint: fingerprint, input: input) }
        return true
    }

    func approveComparison() { gate.withLock { job?.confirm() } }

    func setClipboardEnabled(_ enabled: Bool) { clipboardActor?.setEnabled(enabled) }

    func stop() {
        gate.withLock {
            job?.cancel()
            job = nil
            clipboardActor?.invalidate()
        }
    }

    func stop(after restore: @escaping () -> Bool, completion: @escaping (Bool) -> Void) {
        stop()
        worker.async { [callbackQueue, clipboardActor] in
            let neutral = restore()
            guard neutral, let clipboardActor else {
                callbackQueue.async { completion(neutral) }
                return
            }
            let result = ClipboardReply<Bool>()
            clipboardActor.barrier {
                callbackQueue.async { if result.finishOnce(true) { completion(true) } }
            }
            callbackQueue.asyncAfter(deadline: .now() + .seconds(2)) {
                if result.finishOnce(false) { completion(false) }
            }
        }
    }

    /// Called only on quit, when no further jobs will be started. Hooks must
    /// not wait for main: ownership must be neutral before the process exits.
    func drainOnQuit() {
        stop()
        worker.sync {}
    }

    private static func now() -> UInt64 { DispatchTime.now().uptimeNanoseconds / 1_000_000 }

    private func emit(_ event: Event, job: Job, terminal: Bool = false) {
        callbackQueue.async { [weak self] in
            guard let self, self.gate.withLock({ () -> Bool in
                guard self.job === job else { return false }
                if terminal { self.job = nil }
                return true
            }) else { return }
            self.onEvent(event)
        }
    }

    private func run(job: Job, address: V2ReceiverAddress, fingerprint: String?, input: Input) {
        do {
            try receive(job: job, address: address, fingerprint: fingerprint, input: input)
            emit(.closed, job: job, terminal: true)
        } catch {
            emit(.failed(String(describing: error)), job: job, terminal: true)
        }
    }

    private func receive(job: Job, address: V2ReceiverAddress, fingerprint: String?, input: Input) throws {
        guard !job.cancelled, identity.valid, let own = identity.fingerprint,
              let selector = V2PairOffer.selector(own) else { throw WireError.invalid("Invalid v2 identity") }

        var confirmation: V2PairingConfirmation?
        let peer: V2VerifiedPeer
        if let fingerprint {
            guard let saved = try trust.peers().first(where: { $0.fingerprint == fingerprint }), saved.fingerprint != own else {
                throw WireError.invalid("Unknown receiving host")
            }
            peer = saved
        } else {
            emit(.connecting, job: job)
            let candidate = try offer(address: address, job: job)
            let check = try V2PairingConfirmation(localPEM: identity.certificatePEM, peerPEM: candidate.certificatePEM,
                                                   peerName: candidate.name)
            emit(.compare(Comparison(peerName: candidate.name, peerFingerprint: check.peerFingerprint,
                                     localFingerprint: check.localFingerprint)), job: job)
            guard job.waitForConfirmation(until: Self.now() + 115_000) else { return }
            try check.confirmComparison(peerOnLocal: check.peerFingerprint, localOnPeer: check.localFingerprint)
            peer = try .verified(name: candidate.name, certificatePEM: candidate.certificatePEM, comparedFingerprint: check.peerFingerprint)
            confirmation = check
        }
        guard !job.cancelled else { return }
        emit(.connecting, job: job)
        let fd = try Self.open(address, job: job)
        try Self.write(selector, fd: fd.fd, job: job, until: Self.now() + 5_000)
        let transport = try V2TLSSocket.adopt(fd: fd.take(), client: true, local: identity,
                                              verifiedPeerPEM: peer.certificatePEM, verifiedFingerprint: peer.fingerprint)
        defer { transport.close() }
        let tlsDeadline = Self.now() + 6_000
        while !job.cancelled && Self.now() < tlsDeadline {
            switch try transport.handshake() {
            case .ready: break
            case .closed: throw WireError.invalid("Host closed secure connection")
            case .again:
                guard let descriptor = transport.pollDescriptor,
                      Self.wait(descriptor, transport.wantsWrite ? Int16(POLLOUT) : Int16(POLLIN), job: job, until: tlsDeadline) else { break }
                continue
            }
            break
        }
        guard !job.cancelled, transport.authenticatedPeerFingerprint == peer.fingerprint else {
            throw WireError.invalid("Host certificate verification failed")
        }
        try confirmation?.pinnedTLSReady(authenticatedPeerFingerprint: peer.fingerprint)

        let requestedPairing = confirmation != nil
        let clipboard = clipboardActor.map { ReceiverClipboard(actor: $0, peer: peer.fingerprint, allowed: { !job.cancelled }) }
        var advertised = false
        let session = try V2SessionBridge.start(transport: transport, role: .receiver, platform: 1, sessionEpoch: 0,
                                                 localScreens: input.screenIDs, now: Self.now(), requirePairing: requestedPairing,
                                                 enter: { payload, epoch in
                                                     guard advertised, !job.cancelled, input.enter(payload, epoch) else { return false }
                                                     clipboard?.enter(epoch: epoch)
                                                     return true
                                                 }, input: { !job.cancelled && input.input($0, $1) }, restore: input.restore,
                                                 clipboard: clipboard?.hooks)
        defer {
            job.cancel()
            clipboardActor?.invalidate()
            session.close()
            clipboard?.revoke()
        }
        var sentConfirmation = false, receiving = false
        var comparisonDeadline: UInt64?
        var nextLeaveAt: UInt64 = 0
        while !job.cancelled {
            let now = Self.now()
            try clipboard?.pauseIfRevoked(session)
            try session.step(now: now)
            switch session.phase {
            case 2: // pair_wait
                if confirmation == nil {
                    let check = try V2PairingConfirmation(localPEM: identity.certificatePEM, peerPEM: peer.certificatePEM, peerName: peer.name)
                    confirmation = check; comparisonDeadline = now + 115_000
                    emit(.compare(Comparison(peerName: peer.name, peerFingerprint: check.peerFingerprint,
                                             localFingerprint: check.localFingerprint)), job: job)
                }
                if let comparisonDeadline, now >= comparisonDeadline { return }
                if !sentConfirmation, let check = confirmation, requestedPairing || job.takeConfirmation() {
                    if !requestedPairing {
                        try check.confirmComparison(peerOnLocal: check.peerFingerprint, localOnPeer: check.localFingerprint)
                        try check.pinnedTLSReady(authenticatedPeerFingerprint: peer.fingerprint)
                    }
                    guard session.confirmPairing() else { throw WireError.invalid("Host pairing confirmation unavailable") }
                    sentConfirmation = true
                }
            case 4: // ready
                if !advertised && session.drainedWrites {
                    if let check = confirmation {
                        guard session.pairingConfirmed, !job.cancelled else { throw WireError.invalid("Host pairing not confirmed") }
                        try check.peerConfirmedOnPinnedTLS()
                        try check.commit(to: trust)
                    }
                    guard session.advertise(input.screenAdvertisement) else { throw WireError.invalid("Cannot send display geometry") }
                    advertised = true
                    emit(.ready(peer), job: job)
                } else if receiving {
                    receiving = false
                    emit(.ready(peer), job: job)
                }
            case 6: // active
                if !receiving {
                    receiving = true
                    nextLeaveAt = 0
                    emit(.receiving, job: job)
                }
                if now >= nextLeaveAt, let (x, y) = input.takeLeave() {
                    guard session.requestLeave(x: x, y: y) else { throw WireError.invalid("Cannot return input to the host") }
                    nextLeaveAt = now + 120
                }
            default: break
            }
            try clipboard?.advance(session, now: now, advertised: advertised)
            guard let descriptor = transport.pollDescriptor else { break }
            let interest = transport.wantsWrite || session.needsWriteReadiness ? Int16(POLLOUT) : Int16(POLLIN)
            _ = Self.wait(descriptor, interest, job: job, until: Self.now() + 20)
        }
    }

    private func offer(address: V2ReceiverAddress, job: Job) throws -> V2PairOffer {
        var nonce = [UInt8](repeating: 0, count: 16)
        guard nonce.withUnsafeMutableBytes({ SecRandomCopyBytes(kSecRandomDefault, $0.count, $0.baseAddress!) }) == errSecSuccess,
              let bytes = V2PairOffer(role: 2, nonce: nonce, name: name, certificatePEM: identity.certificatePEM).encoded() else {
            throw WireError.invalid("Could not create public pairing request")
        }
        let fd = try Self.open(address, job: job)
        let deadline = Self.now() + 6_000
        try Self.write(bytes, fd: fd.fd, job: job, until: deadline)
        guard shutdown(fd.fd, SHUT_WR) == 0 else { throw WireError.invalid("Could not finish public pairing request") }
        let decoder = V2PairOfferDecoder()
        var buffer = [UInt8](repeating: 0, count: 4096)
        while Self.wait(fd.fd, Int16(POLLIN), job: job, until: deadline) {
            let count = buffer.withUnsafeMutableBytes { recv(fd.fd, $0.baseAddress, $0.count, 0) }
            if count == 0, let offer = decoder.finish(), offer.role == 1 { return offer }
            if count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) { continue }
            if count <= 0 || !decoder.feed(Array(buffer.prefix(count))) { break }
        }
        throw WireError.invalid("Host did not supply a valid pairing candidate")
    }

    private static func open(_ address: V2ReceiverAddress, job: Job) throws -> Descriptor {
        var hints = addrinfo()
        hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV
        hints.ai_family = AF_UNSPEC
        hints.ai_socktype = SOCK_STREAM
        hints.ai_protocol = IPPROTO_TCP
        var result: UnsafeMutablePointer<addrinfo>?
        guard getaddrinfo(address.host, String(address.port), &hints, &result) == 0, let result else {
            throw WireError.invalid("Enter a numeric IPv4 or IPv6 address")
        }
        defer { freeaddrinfo(result) }
        let info = result.pointee
        let socket = Darwin.socket(info.ai_family, info.ai_socktype, info.ai_protocol)
        guard socket >= 0 else { throw WireError.invalid("Cannot create connection") }
        let fd = Descriptor(socket)
        var one: Int32 = 1
        let flags = fcntl(socket, F_GETFL)
        guard flags >= 0, fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0, fcntl(socket, F_SETFD, FD_CLOEXEC) == 0,
              setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size)) == 0,
              setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &one, socklen_t(MemoryLayout<Int32>.size)) == 0 else {
            throw WireError.invalid("Cannot configure connection")
        }
        if Darwin.connect(socket, info.ai_addr, info.ai_addrlen) != 0 {
            guard errno == EINPROGRESS, wait(socket, Int16(POLLOUT), job: job, until: now() + 5_000) else {
                throw WireError.invalid("Host could not be reached")
            }
            var error: Int32 = 0, count = socklen_t(MemoryLayout<Int32>.size)
            guard getsockopt(socket, SOL_SOCKET, SO_ERROR, &error, &count) == 0, error == 0 else {
                throw WireError.invalid("Host could not be reached")
            }
        }
        return fd
    }

    private static func wait(_ fd: Int32, _ events: Int16, job: Job, until deadline: UInt64) -> Bool {
        while !job.cancelled && now() < deadline {
            var item = pollfd(fd: fd, events: events, revents: 0)
            let count = poll(&item, 1, 20)
            if count > 0 { return true }
            if count < 0 && errno != EINTR { return false }
        }
        return false
    }

    private static func write(_ bytes: [UInt8], fd: Int32, job: Job, until deadline: UInt64) throws {
        var offset = 0
        while offset < bytes.count && wait(fd, Int16(POLLOUT), job: job, until: deadline) {
            let count = bytes.withUnsafeBytes { send(fd, $0.baseAddress!.advanced(by: offset), $0.count - offset, 0) }
            if count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) { continue }
            if count <= 0 { break }
            offset += count
        }
        guard offset == bytes.count else { throw WireError.invalid("Could not send secure connection preface") }
    }
}
