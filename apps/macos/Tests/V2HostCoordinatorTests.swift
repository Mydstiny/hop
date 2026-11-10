import Foundation
import Darwin
import CoreGraphics
import ImageIO

@main struct V2HostCoordinatorTests {
    private final class MemoryStore: V2SecureBlobStore {
        let lock = NSLock()
        let allowWrite = DispatchSemaphore(value: 0)
        private var saved: Data?
        private var count = 0
        var writes: Int { lock.withLock { count } }
        var writeMode = 0 // Set by the test while the host is idle.

        func read(_ account: String) throws -> Data? { lock.withLock { saved } }
        func write(_ account: String, data: Data) throws {
            let mode = lock.withLock { count += 1; return writeMode }
            if mode == 1 && allowWrite.wait(timeout: .now() + .seconds(2)) != .success {
                throw WireError.invalid("Test persistence gate timed out")
            }
            if mode == 2 { throw WireError.invalid("Injected trust storage failure") }
            lock.withLock { saved = data }
        }
        func delete(_ account: String) throws { lock.withLock { saved = nil } }
    }

    private final class Journal {
        let lock = NSLock()
        private var events: [V2HostCoordinator.Event] = []
        private var positions: [V2HostCoordinator.Position?] = []
        func record(_ event: V2HostCoordinator.Event) { lock.withLock { events.append(event) } }
        func restore(_ position: V2HostCoordinator.Position?) -> Bool { lock.withLock { positions.append(position); return true } }
        var mark: Int { lock.withLock { events.count } }
        var restores: [V2HostCoordinator.Position?] { lock.withLock { positions } }
        var failures: [String] {
            lock.withLock { events.compactMap { if case .failed(let reason) = $0 { return reason }; return nil } }
        }
        func find<T>(since: Int, _ match: (V2HostCoordinator.Event) -> T?) -> T? {
            lock.withLock { events.dropFirst(since).lazy.compactMap(match).first }
        }
    }

    private final class Clock {
        let lock = NSLock()
        private var offset: UInt64 = 0
        func advance(_ amount: UInt64) { lock.withLock { offset += amount } }
        func now() -> UInt64 { lock.withLock { DispatchTime.now().uptimeNanoseconds / 1_000_000 + offset } }
    }

    private final class MemoryBoard: HopClipboardBackend {
        let lock = NSLock()
        let proceed = DispatchSemaphore(value: 0)
        private var version = 1
        private var value: HopClipboardPayload?
        private var samples = 0, readCount = 0, writeCount = 0, attemptCount = 0
        private var blockWrite = false, waiting = false
        var observed: (probes: Int, reads: Int, writes: Int, attempts: Int, waiting: Bool, value: HopClipboardPayload?) {
            lock.withLock { (samples, readCount, writeCount, attemptCount, waiting, value) }
        }
        var changeCount: Int { lock.withLock { samples += 1; return version } }
        func read(expected: Int) -> HopClipboardRead {
            lock.withLock {
                readCount += 1
                guard expected == version else { return .retry }
                return value.map(HopClipboardRead.payload) ?? .unsupported
            }
        }
        func seed(_ payload: HopClipboardPayload) { lock.withLock { version += 1; value = payload } }
        func blockNextWrite() { lock.withLock { blockWrite = true } }
        func write(_ payload: HopClipboardPayload) -> Int? { write(payload, permitted: { true }) }
        func write(_ payload: HopClipboardPayload, permitted: () -> Bool) -> Int? {
            let shouldBlock = lock.withLock {
                let block = blockWrite
                blockWrite = false
                waiting = block
                return block
            }
            if shouldBlock { _ = proceed.wait(timeout: .now() + .seconds(3)) }
            let allowed = permitted()
            return lock.withLock {
                waiting = false
                attemptCount += 1
                guard allowed else { return nil }
                version += 1; value = payload; writeCount += 1
                return version
            }
        }
    }

    private final class Receiver {
        let tls: V2TLSSocket
        var session: V2SessionBridge?
        var received: [[UInt8]] = []
        var restores = 0
        var heldKeys = Set<UInt16>()
        let withClipboard: Bool
        var clipboardReceived: [HopClipboardPayload] = []

        init(port: UInt16, identity: V2LocalIdentity, host: V2LocalIdentity, selector: String? = nil, clipboard: Bool = false) throws {
            withClipboard = clipboard
            let fd = try V2HostCoordinatorTests.connect(port)
            do { try V2HostCoordinatorTests.sendAll(fd, V2PairOffer.selector(selector ?? identity.fingerprint!)!) }
            catch { Darwin.close(fd); throw error }
            tls = try V2TLSSocket.adopt(fd: fd, client: true, local: identity, verifiedPeerPEM: host.certificatePEM,
                                       verifiedFingerprint: host.fingerprint!)
        }

        func step(requirePairing: Bool) throws {
            let now = DispatchTime.now().uptimeNanoseconds / 1_000_000
            if !tls.isAuthenticated {
                if try tls.handshake() == .closed { throw WireError.invalid("Test TLS closed") }
                guard tls.isAuthenticated else { return }
            }
            if session == nil {
                session = try V2SessionBridge.start(transport: tls, role: .receiver, platform: 4, sessionEpoch: 0,
                                                     localScreens: [7], now: now, requirePairing: requirePairing,
                                                     input: { [weak self] payload, _ in
                                                         guard let self else { return false }
                                                         self.received.append(payload)
                                                         if payload[1] == 4 {
                                                             let key = UInt16(payload[4]) << 8 | UInt16(payload[5])
                                                             if payload[8] != 0 { self.heldKeys.insert(key) }
                                                             else { self.heldKeys.remove(key) }
                                                         }
                                                         return true
                                                     }, restore: { [weak self] in
                                                         guard let self else { return true }
                                                         self.restores += 1
                                                         self.heldKeys.removeAll()
                                                         return true
                                                     }, clipboard: withClipboard ? .init(revoke: {}, sample: { 1 },
                                                        apply: { [weak self] format, _, bytes, _ in
                                                            let data = Data(buffer: bytes)
                                                            self?.clipboardReceived.append(format == 1 ? .text(data) : .png(data))
                                                            return true
                                                        }) : nil)
            }
            try session?.step(now: now)
            if let session, session.phase == 9, !session.completeRelease(now: now) {
                throw WireError.invalid("Receiver clipboard release failed")
            }
        }
        func close() { session?.close(); tls.close() }
    }

    static func main() throws {
        var checks = 0
        func check(_ value: Bool, _ message: String) {
            checks += 1
            if !value { fputs("FAIL v2 host coordinator: \(message)\n", stderr); exit(1) }
        }

        let own = try V2IdentityFactory.generate()
        let peer = try V2IdentityFactory.generate()
        let stranger = try V2IdentityFactory.generate()
        let blobs = MemoryStore(), store = V2TrustStore(blobs: blobs)
        let journal = Journal(), clock = Clock(), board = MemoryBoard()
        let host = try V2HostCoordinator(deviceName: "Loopback Mac", identity: own, trustStore: store,
                                         callbackQueue: DispatchQueue(label: "test.v2.events"), now: clock.now,
                                         onEvent: journal.record, restore: journal.restore, clipboardBackend: { board })
        let comparison: (V2HostCoordinator.Event) -> V2HostCoordinator.Comparison? = {
            if case .comparison(let value) = $0 { return value }; return nil
        }
        let ready: (V2HostCoordinator.Event) -> Bool? = {
            if case .connected(let paired, let screens) = $0, paired.fingerprint == peer.fingerprint, screens.map(\.id) == [7] { return true }
            return nil
        }
        let failed: (V2HostCoordinator.Event) -> Bool? = { if case .failed = $0 { return true }; return nil }
        let closed: (V2HostCoordinator.Event) -> Bool? = { if case .pairingWindowClosed = $0 { return true }; return nil }
        let disconnected: (V2HostCoordinator.Event) -> Bool? = { if case .disconnected = $0 { return true }; return nil }
        func event<T>(since: Int, _ match: (V2HostCoordinator.Event) -> T?,
                      pumping: () throws -> Void = {}) throws -> T {
            var found: T?
            do {
                try until("event after journal index \(since)") {
                    try pumping()
                    found = journal.find(since: since, match)
                    return found != nil
                }
            } catch {
                throw WireError.invalid("\(error); host failures: \(journal.failures)")
            }
            return found!
        }
        func start() throws -> UInt16 {
            let mark = journal.mark
            host.start(port: 0)
            return try event(since: mark) { if case .listening(let port) = $0 { return port }; return nil }
        }
        func stop() throws {
            let mark = journal.mark
            host.stop()
            _ = try event(since: mark) { if case .stopped = $0 { return true }; return nil }
        }

        let offer = V2PairOffer(role: 2, nonce: Array(repeating: 4, count: 16), name: "Loopback receiver", certificatePEM: peer.certificatePEM)
        let encoded = offer.encoded()!
        var port = try start()
        func candidate() throws -> V2HostCoordinator.Comparison {
            let mark = journal.mark
            host.openPairingWindow()
            _ = try event(since: mark) { if case .pairingWindowOpened = $0 { return true }; return nil }
            let reply = try exchange(port, encoded)
            let decoder = V2PairOfferDecoder()
            check(decoder.feed(reply) && decoder.finish()?.certificatePEM == own.certificatePEM, "candidate exchange returns only public host identity")
            let value = try event(since: mark, comparison)
            check(value.localFingerprint == own.fingerprint && value.peerFingerprint == peer.fingerprint, "both full fingerprints are computed locally")
            return value
        }
        func approve(_ value: V2HostCoordinator.Comparison) {
            host.approve(value, peerOnLocal: value.peerFingerprint, localOnPeer: value.localFingerprint)
        }
        func establish(_ receiver: Receiver, pairing: Bool) throws {
            try until("receiver HELLO") {
                try receiver.step(requirePairing: pairing)
                return receiver.session?.phase == (pairing ? 2 : 4)
            }
        }

        check(try exchange(port, encoded).isEmpty, "HPOP is rejected outside an explicit window")
        let first = try candidate()
        check(try exchange(port, V2PairOffer.selector(first.peerFingerprint)!).isEmpty, "unapproved HPSL cannot authenticate")
        check(blobs.writes == 0, "offer and selector cannot persist trust")
        var mark = journal.mark
        host.approve(first, peerOnLocal: first.peerFingerprint, localOnPeer: first.peerFingerprint)
        _ = try event(since: mark, failed)
        check(blobs.writes == 0, "wrong local fingerprint cancels the candidate")

        let expired = try candidate()
        mark = journal.mark
        clock.advance(121_000)
        _ = try event(since: mark, closed)
        approve(expired)
        check(try exchange(port, V2PairOffer.selector(expired.peerFingerprint)!).isEmpty, "expired candidate cannot be approved")

        let pin = try candidate()
        approve(pin)
        // The comparison command and listener admission are serialized on the host.
        mark = journal.mark
        let wrong = try Receiver(port: port, identity: stranger, host: own, selector: pin.peerFingerprint)
        _ = try event(since: mark, failed, pumping: { try? wrong.step(requirePairing: true) })
        wrong.close()
        check(blobs.writes == 0, "a valid selector with the wrong certificate cannot persist trust")

        let pending = try candidate()
        approve(pending)
        let receiver = try Receiver(port: port, identity: peer, host: own)
        try establish(receiver, pairing: true)
        check(blobs.writes == 0 && !receiver.session!.pairingConfirmed, "pinned TLS alone does not persist trust")
        blobs.lock.withLock { blobs.writeMode = 1 }
        mark = journal.mark
        check(receiver.session!.confirmPairing() && !receiver.session!.drainedWrites, "queuing PAIR_CONFIRM invalidates the previous drain")
        try until("peer sees PAIR_CONFIRM before trust commit") {
            try receiver.step(requirePairing: true)
            return receiver.session!.pairingConfirmed && blobs.writes == 1
        }
        check(try blobs.read("verified-peers") == nil, "blocked trust commit does not publish durable trust")
        check(journal.find(since: mark, ready) == nil, "pending trust commit keeps focus unavailable")
        blobs.allowWrite.signal()
        let screen: [UInt8] = [1,3,0,0,0,1,0,0, 0,0,0,0,0,0,0,7, 0,0,0,0,0,0,0,0, 0,0,7,128,0,0,4,56]
        check(receiver.session!.advertise(screen), "receiver advertises a screen only after both confirmations")
        _ = try event(since: mark, ready, pumping: { try receiver.step(requirePairing: true) })
        check(try store.peers().map(\.fingerprint) == [peer.fingerprint!], "only the compared and pinned peer is persisted")

        let focusEvent: (V2HostCoordinator.Event) -> V2HostCoordinator.Focus? = {
            if case .focused(let focus) = $0 { return focus }; return nil
        }
        mark = journal.mark
        host.enter(screen: 7, x: 321, y: 654)
        try until("receiver ENTER before ACK") {
            try receiver.step(requirePairing: true)
            return receiver.session?.phase == 6
        }
        check(journal.find(since: mark, focusEvent) == nil, "host capture stays local until matching ENTER_ACK")
        let focus = try event(since: mark, focusEvent, pumping: { try receiver.step(requirePairing: true) })
        check(focus.screen == 7 && focus.x == 321 && focus.y == 654, "focus publishes acknowledged coordinates")
        let down: [UInt8] = [1,4,0,0,0,4,0,0,1,0,0,0]
        let up: [UInt8] = [1,4,0,0,0,4,0,0,0,0,0,0]
        check(!host.submitInput([down], focusEpoch: focus.epoch - 1), "stale epochs cannot submit input")
        check(host.submitInput([down, up], focusEpoch: focus.epoch) && host.submitInput([down], focusEpoch: focus.epoch),
              "acknowledged focus accepts ordered input batches")
        try until("input reaches fake receiver") {
            try receiver.step(requirePairing: true)
            return receiver.received.count == 3
        }
        check(receiver.received == [down, up, down] && receiver.heldKeys == [4], "non-move input order and held state are preserved")
        let expected = V2HostCoordinator.Position(x: 65535, y: 1321)
        check(receiver.session!.requestLeave(x: expected.x, y: expected.y), "receiver requests edge return")
        try until("receiver release") {
            try receiver.step(requirePairing: true)
            return receiver.restores > 0
        }
        check(journal.restores == [expected] && receiver.heldKeys.isEmpty, "return restores local position before peer releases its held key")
        check(!host.submitInput([up], focusEpoch: focus.epoch), "released focus rejects input")
        mark = journal.mark
        host.enter(screen: 7, x: 0, y: 0)
        _ = try event(since: mark) { if case .localFocus(nil) = $0 { return true }; return nil }
        check(journal.restores == [expected, nil], "pending capture returns local while RELEASE_ACK is outstanding")
        try until("original release ACK") { try receiver.step(requirePairing: true); return true }
        receiver.close()
        try stop()

        blobs.lock.withLock { blobs.writeMode = 0 }
        port = try start()
        mark = journal.mark
        let reconnect = try Receiver(port: port, identity: peer, host: own)
        try establish(reconnect, pairing: false)
        check(reconnect.session!.advertise(screen), "saved trust reconnects without pairing")
        _ = try event(since: mark, ready, pumping: { try reconnect.step(requirePairing: false) })
        check(journal.find(since: mark, comparison) == nil && blobs.writes == 1, "saved trust reconnect neither prompts nor rewrites")
        mark = journal.mark
        host.enter(screen: 7, x: 120, y: 240)
        // The receiver has not pumped the socket, so it cannot have ACKed ENTER.
        host.release()
        _ = try event(since: mark, { if case .focusReleased = $0 { return true }; return nil },
                      pumping: { try reconnect.step(requirePairing: false) })
        check(journal.find(since: mark, focusEvent) == nil && journal.find(since: mark, disconnected) == nil,
              "cancelling a pending crossing releases the receiver without publishing focus or dropping TLS")
        host.enter(screen: 7, x: 0, y: 0)
        let nextFocus = try event(since: mark, focusEvent, pumping: { try reconnect.step(requirePairing: false) })
        check(nextFocus.epoch != focus.epoch, "reconnected session uses a fresh epoch")
        let refused = try exchange(port, V2PairOffer.selector(peer.fingerprint!)!)
        check(refused.isEmpty && host.submitInput([down], focusEpoch: nextFocus.epoch), "a second selector cannot steal current focus")
        try until("held key before disconnect") { try reconnect.step(requirePairing: false); return reconnect.heldKeys == [4] }
        mark = journal.mark
        host.disconnect()
        _ = try event(since: mark, disconnected)
        try until("receiver sees disconnect") {
            do { try reconnect.step(requirePairing: false); return false }
            catch { return true }
        }
        check(journal.restores.count == 4 && journal.restores.last! == nil && reconnect.heldKeys.isEmpty,
              "closing active transport restores both sides without a synthetic return position")

        blobs.lock.withLock { blobs.writeMode = 2 }
        mark = journal.mark
        let repair = try Receiver(port: port, identity: peer, host: own)
        try establish(repair, pairing: true)
        let reapproval = try event(since: mark, comparison, pumping: { try repair.step(requirePairing: true) })
        check(repair.session!.confirmPairing(), "one-sided trust repair still requires both confirmations")
        try until("repair receiver confirmation") { try repair.step(requirePairing: true); return true }
        check(blobs.writes == 1, "receiver confirmation cannot substitute for local reapproval")
        approve(reapproval)
        _ = try event(since: mark, failed, pumping: { try? repair.step(requirePairing: true) })
        check(blobs.writes == 2 && journal.find(since: mark, ready) == nil, "trust write failure closes before publishing a peer")
        repair.close()

        mark = journal.mark
        host.forgetPeer(fingerprint: peer.fingerprint!)
        _ = try event(since: mark, failed)
        check(try store.peers().map(\.fingerprint) == [peer.fingerprint!], "failed revocation preserves the trust record")
        blobs.lock.withLock { blobs.writeMode = 0 }
        mark = journal.mark
        let revoked = try Receiver(port: port, identity: peer, host: own)
        try establish(revoked, pairing: false)
        check(revoked.session!.advertise(screen), "failed revocation leaves the original pin available for reconnect")
        _ = try event(since: mark, ready, pumping: { try revoked.step(requirePairing: false) })
        host.enter(screen: 7, x: 12, y: 34)
        _ = try event(since: mark, focusEvent, pumping: { try revoked.step(requirePairing: false) })
        host.forgetPeer(fingerprint: peer.fingerprint!)
        _ = try event(since: mark) {
            if case .forgotten(let fingerprint) = $0, fingerprint == peer.fingerprint { return true }; return nil
        }
        check(journal.restores.count == 5 && journal.restores.last! == nil, "forget restores matching active focus before reporting success")
        check(try store.peers().isEmpty, "forget removes the durable trust record")
        check(try exchange(port, V2PairOffer.selector(peer.fingerprint!)!).isEmpty, "revoked selector cannot reconnect")
        revoked.close()
        try stop()

        check(board.observed.probes == 0, "clipboard remains untouched throughout pairing and unnegotiated focus")
        try store.saveVerified(V2VerifiedPeer.verified(name: offer.name, certificatePEM: peer.certificatePEM, comparedFingerprint: peer.fingerprint!))
        port = try start()
        mark = journal.mark
        let disabled = try Receiver(port: port, identity: peer, host: own, clipboard: true)
        try establish(disabled, pairing: false)
        check(disabled.session!.advertise(screen), "clipboard-capable receiver advertises a screen")
        _ = try event(since: mark, ready, pumping: { try disabled.step(requirePairing: false) })
        host.enter(screen: 7, x: 7, y: 7)
        let disabledFocus = try event(since: mark, focusEvent, pumping: { try disabled.step(requirePairing: false) })
        check(disabled.session!.readyClipboard(), "receiver can offer clipboard before host enables it")
        host.activateClipboard(focusEpoch: disabledFocus.epoch)
        try until("default-off READY/PAUSE") { try disabled.step(requirePairing: false); return true }
        mark = journal.mark
        host.disconnect()
        _ = try event(since: mark, disconnected)
        check(board.observed.probes == 0, "clipboard default-off policy blocks even negotiated, acknowledged focus")
        disabled.close()
        host.setClipboardEnabled(true)
        let largeText = HopClipboardPayload.text(Data(repeating: 65, count: 40 * 1024))
        board.seed(largeText)
        mark = journal.mark
        let clipping = try Receiver(port: port, identity: peer, host: own, clipboard: true)
        try establish(clipping, pairing: false)
        check(clipping.session!.clipboardFormats == 6 && clipping.session!.advertise(screen), "text and PNG are negotiated on pinned TLS")
        _ = try event(since: mark, ready, pumping: { try clipping.step(requirePairing: false) })
        host.enter(screen: 7, x: 100, y: 200)
        let clipFocus = try event(since: mark, focusEvent, pumping: { try clipping.step(requirePairing: false) })
        check(board.observed.probes == 0, "remote ENTER_ACK alone cannot sample the local board")
        host.activateClipboard(focusEpoch: clipFocus.epoch)
        try until("local clipboard actor prepared") {
            try clipping.step(requirePairing: false)
            return board.observed.probes != 0
        }
        check(board.observed.reads == 0, "preparation only samples a version before peer READY")
        check(clipping.session!.readyClipboard(), "receiver allows clipboard for acknowledged focus")
        try until("initial clipboard marker") {
            try clipping.step(requirePairing: false)
            return clipping.session!.clipboardInitialReceived
        }
        check(clipping.clipboardReceived == [largeText], "INITIAL_DONE follows every chunk of the initial snapshot")
        check(clipping.session!.drainInitialClipboard(), "receiver drains its initial work before sending")

        let bitmap = CGContext(data: nil, width: 1, height: 1, bitsPerComponent: 8, bytesPerRow: 4,
                               space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
        let encodedImage = NSMutableData()
        let imageDestination = CGImageDestinationCreateWithData(encodedImage, "public.png" as CFString, 1, nil)!
        CGImageDestinationAddImage(imageDestination, bitmap.makeImage()!, nil)
        check(CGImageDestinationFinalize(imageDestination), "test PNG fixture")
        let image = encodedImage as Data
        check(clipping.session!.startClipboard(id: 1, format: 2, total: UInt32(image.count)) && image.withUnsafeBytes {
            clipping.session!.sendClipboard(id: 1, bytes: $0.bindMemory(to: UInt8.self))
        }, "receiver queues PNG")
        try until("host applies remote PNG") {
            try clipping.step(requirePairing: false)
            return board.observed.writes == 1
        }
        check(board.observed.value == .png(image), "verified receiver PNG reaches the memory pasteboard")

        board.blockNextWrite()
        let stale = Data("remote edit".utf8)
        func sendText(_ id: UInt64) -> Bool {
            clipping.session!.startClipboard(id: id, format: 1, total: UInt32(stale.count)) && stale.withUnsafeBytes {
                clipping.session!.sendClipboard(id: id, bytes: $0.bindMemory(to: UInt8.self))
            }
        }
        check(sendText(2), "receiver queues a write during a local copy")
        try until("clipboard write waits for commit") {
            try clipping.step(requirePairing: false)
            return board.observed.waiting
        }
        let localChange = HopClipboardPayload.text(Data("local edit".utf8))
        board.seed(localChange)
        board.proceed.signal()
        try until("local clipboard change is transmitted") {
            try clipping.step(requirePairing: false)
            return clipping.clipboardReceived.last == localChange
        }
        check(board.observed.writes == 1 && board.observed.value == localChange, "new local clipboard content wins an in-flight write")

        board.blockNextWrite()
        check(sendText(3), "receiver queues a text write paused before commit")
        try until("clipboard write reaches precommit barrier") {
            try clipping.step(requirePairing: false)
            return board.observed.waiting
        }
        mark = journal.mark
        host.release()
        _ = try event(since: mark, { if case .localFocus(nil) = $0 { return true }; return nil }, pumping: { try clipping.step(requirePairing: false) })
        try until("receiver releases and ACKs while host actor remains blocked") {
            try clipping.step(requirePairing: false)
            return clipping.session!.phase == 4 && clipping.session!.drainedWrites
        }
        check(!host.submitInput([down], focusEpoch: clipFocus.epoch) && board.observed.waiting,
              "local input restores without waiting for clipboard work")
        mark = journal.mark
        host.enter(screen: 7, x: 101, y: 201)
        _ = try event(since: mark, { if case .localFocus(nil) = $0 { return true }; return nil }, pumping: { try clipping.step(requirePairing: false) })
        check(journal.find(since: mark, focusEvent) == nil, "remote RELEASE_ACK cannot bypass the local actor barrier")
        clock.advance(2_100)
        _ = try event(since: mark, disconnected, pumping: { try? clipping.step(requirePairing: false) })
        check(board.observed.waiting, "two-second actor barrier timeout closes transport while clipboard I/O is blocked")
        board.proceed.signal()
        try until("revoked job exits") { board.observed.attempts == 3 }
        check(board.observed.writes == 1 && board.observed.value == localChange, "revoked write rechecks permission immediately before commit")
        clipping.close()
        try stop()
        print("PASS \(checks) Mac v2 host coordinator loopback checks; ephemeral identities and fake receiver")
    }

    private static func until(_ label: String, _ predicate: () throws -> Bool) throws {
        let deadline = DispatchTime.now() + .seconds(2)
        repeat {
            do { if try predicate() { return } }
            catch { throw WireError.invalid("While waiting for \(label): \(error)") }
            Thread.sleep(forTimeInterval: 0.001)
        } while DispatchTime.now() < deadline
        throw WireError.invalid("Timed out waiting for \(label)")
    }

    private static func connect(_ port: UInt16) throws -> Int32 {
        let fd = socket(AF_INET, SOCK_STREAM, 0)
        guard fd >= 0 else { throw WireError.invalid("Cannot create test socket") }
        var timeout = timeval(tv_sec: 2, tv_usec: 0), one: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size))
        var address = sockaddr_in()
        address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        address.sin_family = sa_family_t(AF_INET)
        address.sin_port = port.bigEndian
        address.sin_addr.s_addr = in_addr_t(0x7f000001).bigEndian
        let status = withUnsafePointer(to: &address) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { Darwin.connect(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) }
        }
        guard status == 0 else { Darwin.close(fd); throw WireError.invalid("Cannot connect test socket") }
        return fd
    }

    private static func sendAll(_ fd: Int32, _ bytes: [UInt8]) throws {
        var offset = 0
        while offset < bytes.count {
            let sent = bytes.withUnsafeBytes { send(fd, $0.baseAddress!.advanced(by: offset), bytes.count - offset, 0) }
            if sent < 0 && errno == EINTR { continue }
            guard sent > 0 else { throw WireError.invalid("Test write failed") }
            offset += sent
        }
    }

    private static func exchange(_ port: UInt16, _ bytes: [UInt8]) throws -> [UInt8] {
        let fd = try connect(port)
        defer { Darwin.close(fd) }
        try sendAll(fd, bytes)
        shutdown(fd, SHUT_WR)
        var response = [UInt8]()
        var buffer = [UInt8](repeating: 0, count: 4096)
        while response.count <= V2PairOffer.headerSize + V2PairOffer.maxName + V2PairOffer.maxCertificate {
            let received = buffer.withUnsafeMutableBytes { recv(fd, $0.baseAddress!, $0.count, 0) }
            if received == 0 || (received < 0 && errno == ECONNRESET) { return response }
            if received < 0 && errno == EINTR { continue }
            guard received > 0 else { throw WireError.invalid("Test exchange did not finish") }
            response += buffer.prefix(received)
        }
        throw WireError.invalid("Test exchange exceeded HPOP limit")
    }
}
