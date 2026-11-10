import Foundation
import Darwin
import Security

/// A v2 host with one physical focus and independently pinned peers. Network, persistence and session methods are owned
/// by a private serial queue. Events arrive on the supplied serial callback
/// queue. The restore hook must restore local input synchronously and must not
/// wait on this coordinator. Public commands do not wait on the worker.
final class V2HostCoordinator {
    struct Comparison: Equatable {
        let id: UUID
        let localName, peerName, localFingerprint, peerFingerprint: String
    }

    struct Focus: Equatable {
        let epoch, screen: UInt64
        let x, y: UInt16
        let captureEpoch: UInt32?
    }

    struct Position: Equatable {
        let x, y: UInt16
    }

    enum Event {
        case listening(port: UInt16)
        case stopped
        case pairingWindowOpened
        case comparison(Comparison)
        case pairingWindowClosed
        case connected(peer: V2VerifiedPeer, screens: [V2SessionBridge.Screen])
        case focused(Focus)
        case localFocus(Position?)
        case focusReleased(epoch: UInt64)
        case disconnected(fingerprint: String)
        case forgotten(fingerprint: String)
        case failed(String)
    }

    private enum Phase: Int32 {
        case pairing = 2, ready = 4, enterWait = 5, active = 6
    }

    private struct Pairing {
        let comparison: Comparison
        let confirmation: V2PairingConfirmation
        let certificatePEM: String
    }

    private final class Readiness {
        private let reader: DispatchSourceRead
        private let writer: DispatchSourceWrite
        private var readEnabled = true
        private var writeEnabled = false

        init?(fd: Int32, queue: DispatchQueue, pump: @escaping () -> Void) {
            // Keep dispatch-owned descriptor numbers alive until their
            // cancellation handlers run, independently of TLS teardown.
            let readFD = fcntl(fd, F_DUPFD_CLOEXEC, 0)
            guard readFD >= 0 else { return nil }
            let writeFD = fcntl(fd, F_DUPFD_CLOEXEC, 0)
            guard writeFD >= 0 else { Darwin.close(readFD); return nil }
            reader = DispatchSource.makeReadSource(fileDescriptor: readFD, queue: queue)
            writer = DispatchSource.makeWriteSource(fileDescriptor: writeFD, queue: queue)
            reader.setEventHandler(handler: pump)
            writer.setEventHandler(handler: pump)
            reader.setCancelHandler { Darwin.close(readFD) }
            writer.setCancelHandler { Darwin.close(writeFD) }
            reader.resume()
        }

        func arm(read: Bool, write: Bool) {
            if read != readEnabled {
                readEnabled = read
                if read { reader.resume() } else { reader.suspend() }
            }
            if write != writeEnabled {
                writeEnabled = write
                if write { writer.resume() } else { writer.suspend() }
            }
        }

        deinit {
            reader.cancel()
            writer.cancel()
            if !readEnabled { reader.resume() }
            if !writeEnabled { writer.resume() }
        }
    }

    private final class Connection {
        let tls: V2TLSSocket
        let peer: V2VerifiedPeer
        let handshakeDeadline: UInt64
        let requiresPairing: Bool
        let readiness: Readiness
        var session: V2SessionBridge?
        var nextEpoch: UInt64 = 0
        var sentConfirmation = false
        var announcedScreens: [V2SessionBridge.Screen]?
        var clipboardEpoch: UInt64?
        var lastClipboardID: UInt64 = 0
        var awaitingClipboardDrain = false
        var releasedEpoch: UInt64?

        init(tls: V2TLSSocket, peer: V2VerifiedPeer, handshakeDeadline: UInt64, requiresPairing: Bool, readiness: Readiness) {
            self.tls = tls
            self.peer = peer
            self.handshakeDeadline = handshakeDeadline
            self.requiresPairing = requiresPairing
            self.readiness = readiness
        }
    }

    private final class ClipboardFocus {
        struct Transfer {
            let id: UInt64
            var data: Data
            var offset = 0
        }
        let epoch: UInt64
        var initialSent = false
        var pollPending = false
        var lastPoll: UInt64?
        var transfer: Transfer?
        init(epoch: UInt64) { self.epoch = epoch }
        deinit {
            if let count = transfer?.data.count { transfer?.data.resetBytes(in: 0..<count) }
        }
    }

    private let queue = DispatchQueue(label: "local.hop.v2.host", qos: .userInitiated)
    private let callbackQueue: DispatchQueue
    private let onEvent: (Event) -> Void
    private let restore: (Position?) -> Bool
    private let now: () -> UInt64
    private let deviceName, localFingerprint: String
    private let identity: V2LocalIdentity
    private let trustStore: V2TrustStore
    private let clipboardActor: V2ClipboardActor?

    private var generation: UUID?
    private var listener: V2IncomingListener?
    private var timer: DispatchSourceTimer?
    private var trusted: [String: V2VerifiedPeer] = [:]
    private var pairingDeadline: UInt64?
    private var pairing: Pairing?
    private var offer: (id: UUID, job: V2OfferExchange)?
    private var selector: (id: UUID, job: V2SelectorReader)?
    private var connections: [String: Connection] = [:]
    private let connectionLimit = 8
    /// Reserved by one focus until both input and clipboard release barriers drain.
    private var connection: Connection?
    private var pairingConnection: Connection?
    private var lastFocusEpoch: UInt64 = 0
    private var pumpArmed = false
    private let shouldRelease: (String, UInt64, UInt64, Position) -> Bool
    private var focus: Focus?
    private var focusPublished = false
    private var releaseAfterEnter = false
    private var leaveRequested: Position?
    private var clipboardFocus: ClipboardFocus?
    private var clipboardRevocation: UUID?

    // The listener and event tap only reserve bounded work under this lock.
    // Never do I/O, invoke callbacks, or wait on another queue while holding it.
    private let gate = NSLock()
    private var admission: UUID?
    private var handoffPending = false
    private var admittedEpoch: UInt64?
    private var pendingInput = 0
    private var clipboardEnabled = false

    init(deviceName: String, identity: V2LocalIdentity, trustStore: V2TrustStore,
         callbackQueue: DispatchQueue = .main, now: @escaping () -> UInt64 = { DispatchTime.now().uptimeNanoseconds / 1_000_000 },
         onEvent: @escaping (Event) -> Void, restore: @escaping (Position?) -> Bool,
         shouldRelease: @escaping (String, UInt64, UInt64, Position) -> Bool = { _,_,_,_ in true },
         clipboardBackend: (() -> HopClipboardBackend)? = nil) throws {
        let offer = V2PairOffer(role: 1, nonce: Array(repeating: 1, count: 16), name: deviceName, certificatePEM: identity.certificatePEM)
        guard identity.valid, offer.valid, let fingerprint = identity.fingerprint else {
            throw WireError.invalid("Invalid v2 host identity or device name")
        }
        self.deviceName = deviceName
        self.identity = identity
        self.localFingerprint = fingerprint
        self.trustStore = trustStore
        self.shouldRelease = shouldRelease
        clipboardActor = clipboardBackend.map { V2ClipboardActor(backend: $0) }
        self.callbackQueue = callbackQueue
        self.now = now
        self.onEvent = onEvent
        self.restore = restore
    }

    deinit {
        // Worker blocks retain self while running. No work can still mutate
        // these resources when the last reference is released.
        timer?.cancel()
        listener?.stop()
        offer?.job.cancel()
        selector?.job.cancel()
        clipboardActor?.invalidate()
        _ = restoreFocus(notify: false)
        for c in connections.values { c.session?.close(); c.tls.close() }
    }

    func start(port: UInt16 = 24891) {
        queue.async { [weak self] in self?.startOnQueue(port: port) }
    }

    func stop() {
        sealInput()
        // Retain self through cleanup so callers can drop their reference.
        queue.async {
            self.generation = nil
            self.updateAdmission()
            self.closeAllConnections()
            self.listener?.stop()
            self.listener = nil
            self.timer?.cancel()
            self.timer = nil
            self.pumpArmed = false
            self.trusted.removeAll()
            if let actor = self.clipboardActor { actor.revoke { self.emit(.stopped) } }
            else { self.emit(.stopped) }
        }
    }

    func disconnect() {
        sealInput()
        queue.async { [weak self] in self?.closeAllConnections() }
    }

    func abortFocus() {
        sealInput()
        queue.async { [weak self] in self?.closeConnection() }
    }

    func forgetPeer(fingerprint: String) {
        queue.async { [weak self] in
            guard let self, V2PairOffer.selector(fingerprint) != nil else { return }
            guard self.connection == nil || self.connection?.peer.fingerprint == fingerprint else {
                self.emit(.failed("Restore local focus before removing v2 trust")); return
            }
            if let peer = self.connections[fingerprint] { self.closeConnection(peer) }
            if self.pairing?.comparison.peerFingerprint == fingerprint { self.closePairing() }
            // Persistence can block. Do not run it while an unrelated peer
            // owns input and the worker must service its recovery deadlines.
            guard self.focus == nil else {
                self.emit(.failed("Restore local focus before removing v2 trust"))
                return
            }
            do {
                try self.trustStore.revoke(fingerprint: fingerprint)
                self.trusted.removeValue(forKey: fingerprint)
                self.emit(.forgotten(fingerprint: fingerprint))
            } catch { self.emit(.failed("Could not remove v2 peer trust")) }
        }
    }

    func openPairingWindow(seconds: TimeInterval = 120) {
        queue.async { [weak self] in
            guard let self, let listener = self.listener, self.connection == nil, self.selector == nil, self.connections.count < self.connectionLimit,
                  self.pairingDeadline == nil, seconds.isFinite, seconds > 0 else { return }
            let duration = min(seconds, 120)
            self.pairingDeadline = self.deadline(after: max(1, UInt64(duration * 1000)))
            listener.openPairingWindow(seconds: duration)
            self.emit(.pairingWindowOpened)
            self.schedulePump()
        }
    }

    func approve(_ comparison: Comparison, peerOnLocal: String, localOnPeer: String) {
        queue.async { [weak self] in
            guard let self, let pairing = self.pairing, pairing.comparison == comparison,
                  pairing.confirmation.state == .waitingForComparison else { return }
            guard self.pairingIsOpen else {
                self.closePairing(failure: "v2 pairing expired")
                return
            }
            do {
                try pairing.confirmation.confirmComparison(peerOnLocal: peerOnLocal, localOnPeer: localOnPeer)
                if let connection = self.pairingConnection {
                    guard let fingerprint = connection.tls.authenticatedPeerFingerprint else {
                        throw WireError.invalid("v2 pairing has no authenticated peer")
                    }
                    try pairing.confirmation.pinnedTLSReady(authenticatedPeerFingerprint: fingerprint)
                    try self.sendPairConfirmationIfReady(connection)
                    self.pump(connection)
                }
            } catch { self.closePairing(failure: "v2 fingerprint confirmation failed") }
        }
    }

    func cancelPairing() {
        queue.async { [weak self] in
            guard let self, self.pairingDeadline != nil else { return }
            if let session = self.pairingConnection?.session, session.cancelPairing() {
                // Best effort: socket closure still cancels if the alert
                // could not be written without blocking.
                try? session.step(now: self.now())
            }
            self.closePairing()
        }
    }

    /// The platform caller must check physical keys/buttons and permissions
    /// before requesting entry and again before suppressing local input.
    func enter(peerFingerprint: String? = nil, screen: UInt64, x: UInt16, y: UInt16, captureEpoch: UInt32? = nil) {
        queue.async { [weak self] in
            guard let self, self.focus == nil else { return }
            let candidates = self.connections.values.filter { candidate in
                (peerFingerprint == nil || peerFingerprint == candidate.peer.fingerprint) &&
                    candidate.announcedScreens?.contains(where: { $0.id == screen }) == true
            }
            guard self.connection == nil, self.pairingDeadline == nil, self.clipboardRevocation == nil, candidates.count == 1,
                  let connection = candidates.first, let session = connection.session,
                  session.phase == Phase.ready.rawValue,
                  connection.announcedScreens?.contains(where: { $0.id == screen }) == true else {
                // The platform may have marked capture pending already. If
                // release is still awaiting ACK, let it retry from local.
                _ = self.restore(nil)
                self.emit(.localFocus(nil))
                return
            }
            let previous = max(self.lastFocusEpoch, connection.nextEpoch)
            guard previous < UInt64.max else {
                self.closeConnection(connection, failure: "v2 focus epochs exhausted")
                return
            }
            connection.nextEpoch = previous + 1
            self.lastFocusEpoch = connection.nextEpoch
            self.connection = connection
            self.focus = Focus(epoch: connection.nextEpoch, screen: screen, x: x, y: y, captureEpoch: captureEpoch)
            guard session.enter(epoch: connection.nextEpoch, screen: screen, x: x, y: y) else {
                self.closeConnection(failure: "v2 focus entry failed")
                return
            }
            self.pump(connection)
        }
    }

    func release() {
        sealInput()
        queue.async { [weak self] in self?.releaseOnQueue() }
    }

    func setClipboardEnabled(_ enabled: Bool) {
        gate.withLock { clipboardEnabled = enabled }
        clipboardActor?.setEnabled(enabled)
        if !enabled {
            queue.async { [weak self] in
                guard let self, self.clipboardFocus != nil else { return }
                self.pauseClipboard()
            }
        }
    }

    /// Call after the platform successfully hands physical capture to this
    /// acknowledged focus. Until then no local clipboard may be sampled.
    func activateClipboard(focusEpoch: UInt64) {
        queue.async { [weak self] in self?.prepareClipboard(epoch: focusEpoch) }
    }

    /// Intended for an event tap. False means fail open locally. At most one
    /// balanced key tap or one regular event is accepted per submission.
    @discardableResult
    func submitInput(_ payloads: [[UInt8]], focusEpoch: UInt64) -> Bool {
        gate.lock()
        defer { gate.unlock() }
        guard admittedEpoch == focusEpoch else { return false }
        guard pendingInput < 64, payloads.count <= 2, payloads.allSatisfy({ (1...20).contains($0.count) }) else {
            admittedEpoch = nil
            queue.async { [weak self] in
                guard let self, self.focus?.epoch == focusEpoch else { return }
                self.closeConnection(failure: "v2 input queue exhausted or invalid")
            }
            return false
        }
        if payloads.isEmpty { return true }
        pendingInput += 1
        queue.async { [weak self] in self?.sendInputOnQueue(payloads, epoch: focusEpoch) }
        return true
    }

    private func startOnQueue(port: UInt16) {
        guard listener == nil else { return }
        do {
            let peers = try trustStore.peers()
            let id = UUID()
            let incoming = V2IncomingListener { [weak self] fd, kind, prefix in
                guard let self else { Darwin.close(fd); return }
                self.enqueueIncoming(fd, kind: kind, prefix: prefix, generation: id)
            }
            let actual = try incoming.start(port: port)
            trusted = Dictionary(uniqueKeysWithValues: peers.map { ($0.fingerprint, $0) })
            listener = incoming
            generation = id
            let timer = DispatchSource.makeTimerSource(queue: queue)
            timer.setEventHandler { [weak self] in self?.pump() }
            timer.schedule(deadline: .distantFuture)
            self.timer = timer
            schedulePump()
            timer.resume()
            updateAdmission()
            emit(.listening(port: actual))
        } catch { emit(.failed("Could not start the v2 host or load peer trust")) }
    }

    private func enqueueIncoming(_ fd: Int32, kind: V2IncomingListener.Kind, prefix: [UInt8], generation: UUID) {
        gate.lock()
        guard admission == generation, !handoffPending else {
            gate.unlock()
            Darwin.close(fd)
            return
        }
        handoffPending = true
        gate.unlock()
        queue.async { [weak self] in
            guard let self else { Darwin.close(fd); return }
            self.gate.lock()
            self.handoffPending = false
            self.admission = nil
            self.gate.unlock()
            guard self.generation == generation else {
                Darwin.close(fd)
                self.updateAdmission()
                return
            }
            self.accept(fd, kind: kind, prefix: prefix)
            self.updateAdmission()
        }
    }

    private func updateAdmission() {
        gate.lock()
        let handshakes = connections.values.filter { $0.session == nil }.count
        admission = offer == nil && selector == nil && connections.count < connectionLimit && handshakes < 2 ? generation : nil
        gate.unlock()
    }

    private func accept(_ fd: Int32, kind: V2IncomingListener.Kind, prefix: [UInt8]) {
        guard offer == nil, selector == nil, connections.count < connectionLimit else {
            Darwin.close(fd)
            return
        }
        let id = UUID()
        switch kind {
        case .offer:
            guard pairingIsOpen, pairing == nil else { Darwin.close(fd); return }
            guard let nonce = Self.randomBytes(16), nonce.contains(where: { $0 != 0 }) else {
                Darwin.close(fd)
                closePairing(failure: "Could not generate a v2 pairing nonce")
                return
            }
            let local = V2PairOffer(role: 1, nonce: nonce, name: deviceName, certificatePEM: identity.certificatePEM)
            guard let exchange = V2OfferExchange(fd: fd, prefix: prefix, local: local, completion: { [weak self] result in
                self?.queue.async { [weak self] in self?.finishOffer(result, id: id) }
            }) else { return }
            offer = (id, exchange)
            exchange.start()
        case .selector:
            guard let reader = V2SelectorReader(fd: fd, prefix: prefix, completion: { [weak self] result in
                guard let self else { Self.closeSelection(result); return }
                self.queue.async { [weak self] in
                    guard let self else { Self.closeSelection(result); return }
                    self.finishSelector(result, id: id)
                }
            }) else { return }
            selector = (id, reader)
            reader.start()
        }
    }

    private func finishOffer(_ result: Result<V2PairOffer, Error>, id: UUID) {
        guard offer?.id == id else { return }
        offer = nil
        defer { updateAdmission() }
        guard pairingIsOpen, case .success(let candidate) = result else { return }
        do {
            if let fingerprint = V2Certificate.fingerprint(candidate.certificatePEM), connections[fingerprint] != nil { return }
            pairing = try makePairing(peerName: candidate.name, certificatePEM: candidate.certificatePEM)
            listener?.closePairingWindow()
            if let pairing { emit(.comparison(pairing.comparison)) }
        } catch { closePairing(failure: "Invalid v2 pairing candidate") }
    }

    private static func closeSelection(_ result: Result<V2SelectorReader.Selection, Error>) {
        if case .success(let selection) = result { Darwin.close(selection.fd) }
    }

    private func finishSelector(_ result: Result<V2SelectorReader.Selection, Error>, id: UUID) {
        guard selector?.id == id else { Self.closeSelection(result); return }
        selector = nil
        defer { updateAdmission() }
        guard case .success(let selected) = result else { return }

        let peer: V2VerifiedPeer
        let requiresPairing = pairing?.comparison.peerFingerprint == selected.fingerprint
        guard connections[selected.fingerprint] == nil else { Darwin.close(selected.fd); return }
        if requiresPairing {
            guard pairingIsOpen, let pairing, pairingConnection == nil,
                  pairing.confirmation.state == .waitingForTLS,
                  let compared = try? V2VerifiedPeer.verified(name: pairing.comparison.peerName, certificatePEM: pairing.certificatePEM,
                                                              comparedFingerprint: selected.fingerprint) else {
                Darwin.close(selected.fd)
                return
            }
            peer = compared
        } else {
            guard let known = trusted[selected.fingerprint] else { Darwin.close(selected.fd); return }
            peer = known
        }

        do {
            let tls = try V2TLSSocket.adopt(fd: selected.fd, client: false, local: identity,
                                           verifiedPeerPEM: peer.certificatePEM, verifiedFingerprint: peer.fingerprint)
            guard let descriptor = tls.pollDescriptor,
                  let readiness = Readiness(fd: descriptor, queue: queue, pump: { [weak self, weak tls] in
                      guard let self, let tls, let connection = self.connections[peer.fingerprint], connection.tls === tls else { return }
                      self.pump(connection)
                  }) else { throw WireError.invalid("Could not watch v2 transport") }
            let connection = Connection(tls: tls, peer: peer, handshakeDeadline: deadline(after: 5_000),
                                    requiresPairing: requiresPairing, readiness: readiness)
            connections[peer.fingerprint] = connection
            if requiresPairing { pairingConnection = connection }
            schedulePump()
            pump(connection)
        } catch {
            if requiresPairing { closePairing(failure: "Could not establish pinned v2 TLS") }
            else { emit(.failed("Could not establish pinned v2 TLS")) }
        }
    }

    private func makePairing(peerName: String, certificatePEM: String) throws -> Pairing {
        let confirmation = try V2PairingConfirmation(localPEM: identity.certificatePEM, peerPEM: certificatePEM,
                                                     peerName: peerName, now: { [now] in Double(now()) / 1000 })
        let comparison = Comparison(id: UUID(), localName: deviceName, peerName: peerName,
                                    localFingerprint: localFingerprint, peerFingerprint: confirmation.peerFingerprint)
        return Pairing(comparison: comparison, confirmation: confirmation, certificatePEM: certificatePEM)
    }

    private var pairingIsOpen: Bool {
        pairingDeadline.map { now() < $0 } ?? false
    }

    private func deadline(after milliseconds: UInt64) -> UInt64 {
        let (end, overflow) = now().addingReportingOverflow(milliseconds)
        return overflow ? UInt64.max : end
    }

    private func schedulePump() {
        guard let timer else { return }
        let needed = !connections.isEmpty || pairingDeadline != nil
        guard needed != pumpArmed else { return }
        pumpArmed = needed
        if needed {
            // Socket readiness and explicit sends wake the worker between
            // timer ticks. The timer services heartbeats and deadlines.
            timer.schedule(deadline: .now() + .milliseconds(100), repeating: .milliseconds(100), leeway: .milliseconds(5))
        } else {
            timer.schedule(deadline: .distantFuture)
        }
    }

    private func pump() {
        if pairingDeadline != nil && !pairingIsOpen {
            closePairing(failure: "v2 pairing expired")
        }
        for candidate in Array(connections.values) { pump(candidate) }
    }

    private func pump(_ connection: Connection) {
        guard connections[connection.peer.fingerprint] === connection else { return }
        defer {
            if connections[connection.peer.fingerprint] === connection {
                let wantsWrite = connection.tls.wantsWrite
                connection.readiness.arm(read: !wantsWrite, write: wantsWrite || connection.session?.needsWriteReadiness == true)
            }
        }
        do {
            guard let session = connection.session else { try handshake(connection); return }
            if self.connection === connection, clipboardFocus != nil, !gate.withLock({ clipboardEnabled }) {
                guard session.pauseClipboard() else { throw WireError.invalid("Could not safely pause v2 clipboard") }
            }
            try session.step(now: now())
            if session.phase == Phase.pairing.rawValue {
                if pairingConnection !== connection {
                    guard pairingDeadline == nil, self.connection == nil else { throw WireError.invalid("Another focus or pairing is active") }
                    // An already pinned peer may still need confirmation
                    // after the last connection saved trust on just one side.
                    pairing = try makePairing(peerName: connection.peer.name, certificatePEM: connection.peer.certificatePEM)
                    pairingConnection = connection
                    pairingDeadline = deadline(after: 120_000)
                    if let pairing { emit(.comparison(pairing.comparison)) }
                }
                try sendPairConfirmationIfReady(connection)
            }
            if pairingConnection === connection, let pairing {
                guard session.pairingConfirmed, session.drainedWrites else { return }
                guard pairingIsOpen else { throw WireError.invalid("v2 pairing expired") }
                try pairing.confirmation.peerConfirmedOnPinnedTLS()
                try pairing.confirmation.commit(to: trustStore)
                trusted[connection.peer.fingerprint] = connection.peer
                clearPairing()
            }
            if session.phase == Phase.ready.rawValue {
                if self.connection === connection && focus == nil && clipboardRevocation == nil {
                    self.connection = nil
                    if let epoch = connection.releasedEpoch { connection.releasedEpoch = nil; emit(.focusReleased(epoch: epoch)) }
                }
                let screens = try session.screens()
                if screens != connection.announcedScreens {
                    connection.announcedScreens = screens
                    emit(.connected(peer: connection.peer, screens: screens))
                }
            }
            if self.connection === connection {
                if releaseAfterEnter || leaveRequested != nil {
                    releaseOnQueue(pumpImmediately: false)
                } else if let focus, !focusPublished, session.phase == Phase.active.rawValue {
                    gate.lock()
                    admittedEpoch = focus.epoch
                    gate.unlock()
                    focusPublished = true
                    emit(.focused(focus))
                }
                if self.connection === connection { try driveClipboard(session: session) }
            }
        } catch {
            closeConnection(connection, failure: (error as? WireError)?.description ?? "v2 session or trust persistence failed")
        }
    }

    private func handshake(_ connection: Connection) throws {
        guard now() < connection.handshakeDeadline else { throw WireError.invalid("v2 TLS handshake timed out") }
        switch try connection.tls.handshake() {
        case .again: return
        case .closed: throw WireError.invalid("v2 TLS connection closed")
        case .ready: break
        }
        guard let authenticated = connection.tls.authenticatedPeerFingerprint, authenticated == connection.peer.fingerprint,
              let bytes = Self.randomBytes(8) else { throw WireError.invalid("Could not authenticate v2 peer or generate session epoch") }
        let epoch = bytes.reduce(UInt64(0)) { ($0 << 8) | UInt64($1) } & UInt64(Int64.max)
        guard epoch != 0 else { throw WireError.invalid("Invalid v2 session epoch") }
        if connection.requiresPairing {
            guard let pairing, pairingIsOpen else { throw WireError.invalid("v2 pairing expired") }
            try pairing.confirmation.pinnedTLSReady(authenticatedPeerFingerprint: authenticated)
        }
        connection.nextEpoch = epoch
        connection.session = try V2SessionBridge.start(transport: connection.tls, role: .host, platform: 1, sessionEpoch: epoch,
                                                        localScreens: [], now: now(), requirePairing: connection.requiresPairing,
                                                        restore: { [weak self, weak connection] in
                                                            guard let self, let connection, self.connection === connection else { return true }
                                                            return self.restoreFocus()
                                                        },
                                                        leave: { [weak self, weak connection] screen, x, y in
                                                            guard let self, let connection, let focus = self.focus, self.connection === connection else { return false }
                                                            let point = Position(x: x, y: y)
                                                            if self.shouldRelease(connection.peer.fingerprint, screen, focus.epoch, point) { self.leaveRequested = point }
                                                            return true
                                                        }, clipboard: clipboardHooks(for: connection))
        updateAdmission()
    }

    private func sendPairConfirmationIfReady(_ connection: Connection) throws {
        guard pairingConnection === connection, pairing?.confirmation.state == .waitingForPeer,
              let session = connection.session, session.phase == Phase.pairing.rawValue,
              !connection.sentConfirmation else { return }
        guard session.confirmPairing() else { throw WireError.invalid("Could not confirm v2 pairing") }
        connection.sentConfirmation = true
    }

    private func sendInputOnQueue(_ payloads: [[UInt8]], epoch: UInt64) {
        gate.lock()
        pendingInput -= 1
        let admitted = admittedEpoch == epoch
        gate.unlock()
        guard admitted, focus?.epoch == epoch, let connection, let session = connection.session else { return }
        guard payloads.allSatisfy({ session.sendInput($0) }) else {
            closeConnection(failure: "v2 could not send captured input")
            return
        }
        pump(connection)
    }

    private func releaseOnQueue(pumpImmediately: Bool = true) {
        guard let connection, let session = connection.session, let focus else { return }
        if session.phase == Phase.enterWait.rawValue {
            // Let ENTER complete on the wire, then release without publishing focus.
            releaseAfterEnter = true
            return
        }
        guard session.phase == Phase.active.rawValue else { return }
        releaseAfterEnter = false
        guard session.release() else {
            closeConnection()
            return
        }
        connection.releasedEpoch = focus.epoch
        if session.clipboardFormats != 0 {
            if clipboardRevocation != nil {
                connection.awaitingClipboardDrain = true
            } else if !session.completeRelease(now: now()) {
                closeConnection(failure: "v2 clipboard release failed")
                return
            }
        }
        if pumpImmediately { pump(connection) }
    }

    private func sealInput() {
        gate.lock()
        admittedEpoch = nil
        clipboardActor?.invalidate()
        gate.unlock()
    }

    private func restoreFocus(notify: Bool = true) -> Bool {
        releaseAfterEnter = false
        guard focus != nil else { return true }
        sealInput()
        focus = nil
        focusPublished = false
        let position = leaveRequested
        leaveRequested = nil
        let restored = restore(position)
        if notify { emit(.localFocus(position)) }
        return restored
    }

    private func clipboardHooks(for connection: Connection) -> V2SessionBridge.ClipboardHooks? {
        guard let actor = clipboardActor else { return nil }
        return V2SessionBridge.ClipboardHooks(revoke: { [weak self, weak connection] in
            guard let self, let connection, self.connection === connection else { return }
            self.revokeClipboard()
        }, sample: actor.sample, apply: { [weak self, weak connection] format, epoch, bytes, baseline in
                guard let self, let connection, self.connection === connection,
                      self.clipboardFocus?.epoch == epoch else { return true }
                return actor.accept(format: format, peer: connection.peer.fingerprint, epoch: epoch, bytes: bytes, baseline: baseline) { [weak self] in
                    self?.queue.async { [weak self] in
                        guard let self, self.clipboardFocus?.epoch == epoch else { return }
                        self.pauseClipboard()
                    }
                }
            })
    }

    private func prepareClipboard(epoch: UInt64) {
        guard let actor = clipboardActor, let connection, let session = connection.session,
              session.phase == Phase.active.rawValue, session.clipboardFormats != 0,
              focus?.epoch == epoch, focusPublished, clipboardRevocation == nil, connection.clipboardEpoch != epoch else { return }
        connection.clipboardEpoch = epoch
        let state = ClipboardFocus(epoch: epoch)
        let accepted = gate.withLock {
            guard admittedEpoch == epoch else { return false }
            return actor.prepare(peer: connection.peer.fingerprint, epoch: epoch) { [weak self, weak state] ready in
                self?.queue.async { [weak self, weak state] in
                    guard let self, let state, self.clipboardFocus === state,
                          let session = self.connection?.session else { return }
                    if ready && self.clipboardActor?.permitted(state.epoch) == true && session.readyClipboard() { self.pump() }
                    else { self.pauseClipboard() }
                }
            }
        }
        if accepted { clipboardFocus = state }
        else { pauseClipboard() }
    }

    private func pauseClipboard() {
        guard let session = connection?.session else { return }
        if session.pauseClipboard() { pump() }
        else { closeConnection(failure: "Could not safely pause v2 clipboard") }
    }

    private func driveClipboard(session: V2SessionBridge) throws {
        guard let actor = clipboardActor, let state = clipboardFocus, actor.permitted(state.epoch), session.clipboardAvailable else { return }
        if state.transfer != nil {
            guard session.drainedWrites, let offset = state.transfer?.offset, let id = state.transfer?.id else { return }
            // New control and input stay ahead of the next clipboard slice.
            let sent = session.sendClipboardSlice(id: id, data: state.transfer!.data, offset: offset)
            guard sent > 0 else { throw WireError.invalid("v2 clipboard chunk failed") }
            state.transfer?.offset += sent
            if state.transfer?.offset == state.transfer?.data.count {
                if let count = state.transfer?.data.count { state.transfer?.data.resetBytes(in: 0..<count) }
                state.transfer = nil
                try finishInitialClipboard(state, session: session)
            }
            return
        }
        guard !state.pollPending else { return }
        let time = now()
        if let last = state.lastPoll, time >= last, time - last < 100 { return }
        state.lastPoll = time
        state.pollPending = true
        actor.poll(epoch: state.epoch) { [weak self, weak state] payload, initialSettled in
            self?.queue.async { [weak self, weak state] in
                guard let self, let state, self.clipboardFocus === state,
                      let connection = self.connection, let session = connection.session else { return }
                state.pollPending = false
                guard self.clipboardActor?.permitted(state.epoch) == true, session.clipboardAvailable else { return }
                do {
                    try self.sendClipboard(payload, initialSettled: initialSettled, state: state, connection: connection, session: session)
                    if session.needsWriteReadiness { self.pump() }
                } catch { self.closeConnection(failure: "v2 clipboard transfer failed") }
            }
        }
    }

    private func sendClipboard(_ payload: HopClipboardPayload?, initialSettled: Bool, state: ClipboardFocus,
                               connection: Connection, session: V2SessionBridge) throws {
        let format: UInt8
        let data: Data
        switch payload {
        case .text(let bytes): format = 1; data = bytes
        case .png(let bytes): format = 2; data = bytes
        case nil:
            if initialSettled { try finishInitialClipboard(state, session: session) }
            return
        }
        guard session.clipboardFormats & (1 << format) != 0 else {
            if initialSettled { try finishInitialClipboard(state, session: session) }
            return
        }
        guard connection.lastClipboardID < UInt64.max,
              let size = UInt32(exactly: data.count) else { throw WireError.invalid("Invalid v2 clipboard transfer") }
        connection.lastClipboardID += 1
        let id = connection.lastClipboardID
        guard session.startClipboard(id: id, format: format, total: size) else {
            throw WireError.invalid("Could not start v2 clipboard transfer")
        }
        if data.isEmpty { try finishInitialClipboard(state, session: session) }
        else { state.transfer = ClipboardFocus.Transfer(id: id, data: data) }
    }

    private func finishInitialClipboard(_ state: ClipboardFocus, session: V2SessionBridge) throws {
        guard !state.initialSent else { return }
        guard session.finishInitialClipboard() else { throw WireError.invalid("Could not finish v2 initial clipboard snapshot") }
        state.initialSent = true
    }

    private func revokeClipboard() {
        if let focus { connection?.clipboardEpoch = focus.epoch }
        guard let actor = clipboardActor, clipboardFocus != nil else { return }
        clipboardFocus = nil
        let id = UUID()
        clipboardRevocation = id
        actor.revoke { [weak self] in
            self?.queue.async { [weak self] in
                guard let self, self.clipboardRevocation == id else { return }
                self.clipboardRevocation = nil
                guard let connection = self.connection, connection.awaitingClipboardDrain,
                      let session = connection.session else { return }
                connection.awaitingClipboardDrain = false
                if session.completeRelease(now: self.now()) { self.pump() }
                else { self.closeConnection(failure: "v2 clipboard release failed") }
            }
        }
    }

    private func clearPairing() {
        guard pairingDeadline != nil else { return }
        pairing?.confirmation.cancel()
        pairing = nil
        pairingConnection = nil
        pairingDeadline = nil
        listener?.closePairingWindow()
        offer?.job.cancel()
        offer = nil
        emit(.pairingWindowClosed)
    }

    private func closePairing(failure: String? = nil) {
        let pairingWasOpen = pairingDeadline != nil
        if let pairingConnection { closeConnection(pairingConnection) }
        else { clearPairing() }
        if pairingWasOpen { selector?.job.cancel(); selector = nil }
        updateAdmission(); schedulePump()
        if let failure { emit(.failed(failure)) }
    }

    private func closeAllConnections() {
        for candidate in Array(connections.values) { closeConnection(candidate) }
        closePairing()
        selector?.job.cancel(); selector = nil
        updateAdmission()
    }

    private func closeConnection(_ candidate: Connection? = nil, failure: String? = nil) {
        guard let candidate = candidate ?? connection, connections[candidate.peer.fingerprint] === candidate else {
            if let failure { emit(.failed(failure)) }
            return
        }
        if self.connection === candidate {
            _ = restoreFocus()
            revokeClipboard()
            leaveRequested = nil
            self.connection = nil
        }
        if pairingConnection === candidate { clearPairing() }
        connections.removeValue(forKey: candidate.peer.fingerprint)
        candidate.session?.close()
        candidate.tls.close()
        updateAdmission()
        schedulePump()
        emit(.disconnected(fingerprint: candidate.peer.fingerprint))
        if let failure { emit(.failed(failure)) }
    }

    private func emit(_ event: Event) {
        callbackQueue.async { [onEvent] in onEvent(event) }
    }

    private static func randomBytes(_ count: Int) -> [UInt8]? {
        var bytes = [UInt8](repeating: 0, count: count)
        let status = bytes.withUnsafeMutableBytes { SecRandomCopyBytes(kSecRandomDefault, $0.count, $0.baseAddress!) }
        return status == errSecSuccess ? bytes : nil
    }
}
