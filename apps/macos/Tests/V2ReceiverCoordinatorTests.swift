import Foundation

@main struct V2ReceiverCoordinatorTests {
    private final class MemoryStore: V2SecureBlobStore {
        var data: Data?
        func read(_ account: String) throws -> Data? { data }
        func write(_ account: String, data: Data) throws { self.data = data }
        func delete(_ account: String) throws { data = nil }
    }
    private final class Record {
        let gate = NSLock()
        var port: UInt16?
        var hostComparison: V2HostCoordinator.Comparison?
        var receiverComparison: V2ReceiverCoordinator.Comparison?
        var focus: V2HostCoordinator.Focus?
        var hostConnected = 0, receiverConnected = 0, secondConnected = 0, restores = 0, failed = 0
        var input: [[UInt8]] = [], secondInput: [[UInt8]] = []
        var leave = false, returned = false, hostDisconnected = false, secondDisconnected = false, refusedAmbiguous = false
        var releasedEpoch: UInt64?
    }
    static func main() throws {
        let clock = Record()
        let hostID = try V2IdentityFactory.generate(), receiverID = try V2IdentityFactory.generate(), secondID = try V2IdentityFactory.generate()
        let hostTrust = V2TrustStore(blobs: MemoryStore()), receiverTrust = V2TrustStore(blobs: MemoryStore())
        let secondTrust = V2TrustStore(blobs: MemoryStore())
        try hostTrust.saveVerified(.verified(name: "Second receiver", certificatePEM: secondID.certificatePEM, comparedFingerprint: secondID.fingerprint!))
        try secondTrust.saveVerified(.verified(name: "Test host", certificatePEM: hostID.certificatePEM, comparedFingerprint: hostID.fingerprint!))
        let callback = DispatchQueue(label: "local.hop.test.receiver.callback")
        let host = try V2HostCoordinator(deviceName: "Test host", identity: hostID, trustStore: hostTrust,
                                         callbackQueue: callback, onEvent: { event in
            clock.gate.withLock {
                switch event {
                case .listening(let port): clock.port = port
                case .comparison(let request): clock.hostComparison = request
                case .connected(_, let screens) where !screens.isEmpty: clock.hostConnected += 1
                case .focused(let focus): clock.focus = focus
                case .localFocus(let position):
                    if position == nil { clock.refusedAmbiguous = true } else { clock.returned = position?.y == 321 }
                case .focusReleased(let epoch): clock.releasedEpoch = epoch
                case .disconnected(let fingerprint):
                    if fingerprint == secondID.fingerprint { clock.secondDisconnected = true }
                    if fingerprint == receiverID.fingerprint { clock.hostDisconnected = true }
                default: break
                }
            }
        }, restore: { _ in true })
        let receiver = V2ReceiverCoordinator(identity: receiverID, name: "Test Mac receiver", trust: receiverTrust,
                                             callbackQueue: callback) { event in
            clock.gate.withLock {
                switch event {
                case .compare(let request): clock.receiverComparison = request
                case .ready: clock.receiverConnected += 1
                case .failed: clock.failed += 1
                default: break
                }
            }
        }
        let second = V2ReceiverCoordinator(identity: secondID, name: "Second receiver", trust: secondTrust, callbackQueue: callback) { event in
            if case .ready = event { clock.gate.withLock { clock.secondConnected += 1 } }
        }
        let screen: [UInt8] = [1,3,0,0,0,1,0,0, 0,0,0,0,0,0,0,7, 0,0,0,0,0,0,0,0, 0,0,7,128,0,0,4,56]
        let input = V2ReceiverCoordinator.Input(screenIDs: [7], screenAdvertisement: screen, enter: { _,_ in true },
                                                input: { bytes,_ in clock.gate.withLock { clock.input.append(bytes) }; return true },
                                                restore: { clock.gate.withLock { clock.restores += 1 }; return true },
                                                takeLeave: {
            clock.gate.withLock { guard clock.leave else { return nil }; clock.leave = false; return (0, 321) }
        })
        let secondInput = V2ReceiverCoordinator.Input(screenIDs: [7], screenAdvertisement: screen, enter: { _,_ in true },
            input: { bytes, _ in clock.gate.withLock { clock.secondInput.append(bytes) }; return true },
            restore: { true }, takeLeave: { nil })
        defer { receiver.drainOnQuit(); second.drainOnQuit(); host.stop() }
        func check(_ condition: @autoclosure () -> Bool, _ detail: String) throws {
            guard condition() else { throw WireError.invalid("Mac receiver: " + detail) }
        }
        func wait<T>(_ detail: String, _ probe: () -> T?) throws -> T {
            let end = DispatchTime.now() + .seconds(6)
            while DispatchTime.now() < end {
                if let value = clock.gate.withLock(probe) { return value }
                Thread.sleep(forTimeInterval: 0.005)
            }
            let snapshot = clock.gate.withLock { "host=\(clock.hostConnected), receiver=\(clock.receiverConnected), failed=\(clock.failed)" }
            throw WireError.invalid("Mac receiver: timeout " + detail + " (" + snapshot + ")")
        }
        // Keep this fixture independent of a running Hop application.
        host.start(port: 0)
        let port = try wait("listen") { clock.port }
        let address = try V2ReceiverAddress(host: "127.0.0.1", port: port)
        host.openPairingWindow()
        try check(receiver.connect(address: address, fingerprint: nil, input: input), "start pairing job")
        let local = try wait("host and receiver both compare") { clock.hostComparison }
        let remote = try wait("receiver comparison") { clock.receiverComparison }
        let beforePair = try receiverTrust.peers()
        try check(local.peerFingerprint == remote.localFingerprint && local.localFingerprint == remote.peerFingerprint &&
                  beforePair.isEmpty, "candidate fingerprints match, trust empty before explicit approval")
        host.approve(local, peerOnLocal: local.peerFingerprint, localOnPeer: local.localFingerprint)
        // The host must first approve the independently compared public pin.
        // The worker serializes approve and the later selector admission.
        receiver.approveComparison()
        _ = try wait("ready after approval") { clock.hostConnected == 1 && clock.receiverConnected == 1 ? true : nil }
        let trusted = try receiverTrust.peers()
        try check(trusted.map(\.fingerprint) == [hostID.fingerprint!], "stored after both authenticated confirmations")
        try check(second.connect(address: address, fingerprint: hostID.fingerprint, input: secondInput), "admit a different peer with the same screen ID")
        _ = try wait("both peers connected") { clock.hostConnected == 2 && clock.secondConnected == 1 ? true : nil }
        host.enter(screen: 7, x: 1, y: 1)
        _ = try wait("ambiguous bare screen refused") { clock.refusedAmbiguous ? true : nil }
        host.enter(peerFingerprint: receiverID.fingerprint, screen: 7, x: 321, y: 654)
        let focus = try wait("ENTER acknowledged") { clock.focus }
        second.drainOnQuit()
        _ = try wait("other device disconnects") { clock.secondDisconnected ? true : nil }
        let down: [UInt8] = [1,4,0,0,0,4,0,0,1,0,0,0]
        try check(host.submitInput([down], focusEpoch: focus.epoch), "send authenticated input")
        _ = try wait("input arrives at injected backend") { clock.input.first == down ? true : nil }
        clock.gate.withLock { clock.leave = true }
        _ = try wait("leave restores both sides") {
            clock.returned && clock.restores > 0 && clock.receiverConnected == 2 && clock.releasedEpoch == focus.epoch ? true : nil
        }
        try check(second.connect(address: address, fingerprint: hostID.fingerprint, input: secondInput), "idle receiver can reconnect")
        _ = try wait("second receiver returns") { clock.hostConnected == 3 && clock.secondConnected == 2 ? true : nil }
        host.enter(peerFingerprint: secondID.fingerprint, screen: 7, x: 1234, y: 4321, captureEpoch: 2)
        let next = try wait("new focus only after release ACK") { clock.focus.flatMap { $0.epoch > focus.epoch ? $0 : nil } }
        try check(next.captureEpoch == 2 && !host.submitInput([down], focusEpoch: focus.epoch), "capture token echoed and old input invalidated")
        try check(host.submitInput([down], focusEpoch: next.epoch), "new target admits input")
        _ = try wait("new target only") { clock.secondInput.first == down && clock.input.count == 1 ? true : nil }
        host.release()
        _ = try wait("second release acknowledged") { clock.releasedEpoch == next.epoch ? true : nil }
        second.drainOnQuit()
        receiver.drainOnQuit()
        _ = try wait("host sees close") { clock.hostDisconnected ? true : nil }
        try check(receiver.connect(address: address, fingerprint: hostID.fingerprint, input: input), "start pinned reconnect")
        _ = try wait("reconnect with saved identity") { clock.hostConnected == 4 && clock.receiverConnected == 3 ? true : nil }
        receiver.drainOnQuit()
        try check(receiver.connect(address: address, fingerprint: String(repeating: "A", count: 64), input: input), "submit unknown pin")
        _ = try wait("unknown pin rejected") { clock.failed == 1 ? true : nil }
        print("PASS Mac receiver and multi-peer host: pairing, duplicate screen ID isolation, input after unrelated disconnect, leave, pinned reconnect, and unknown pin rejection (loopback, fake input)")
    }
}
