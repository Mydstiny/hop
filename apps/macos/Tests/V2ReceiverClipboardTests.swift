import Foundation

@main struct V2ReceiverClipboardTests {
    private final class MemoryStore: V2SecureBlobStore {
        private var data: Data?
        func read(_ account: String) throws -> Data? { data }
        func write(_ account: String, data: Data) throws { self.data = data }
        func delete(_ account: String) throws { data = nil }
    }

    private final class Board: HopClipboardBackend {
        private let gate = NSLock()
        private var sequence = 1, probes = 0, reads = 0, writes = 0, attempts = 0
        private var value: HopClipboardPayload
        private var blockNext = false, waiting = false
        let releaseWrite = DispatchSemaphore(value: 0)

        init(_ value: HopClipboardPayload) { self.value = value }
        var snapshot: (value: HopClipboardPayload, probes: Int, reads: Int, writes: Int, attempts: Int, waiting: Bool) {
            gate.withLock { (value, probes, reads, writes, attempts, waiting) }
        }
        var changeCount: Int { gate.withLock { probes += 1; return sequence } }
        func read(expected: Int) -> HopClipboardRead {
            gate.withLock { reads += 1; return expected == sequence ? .payload(value) : .retry }
        }
        func copy(_ payload: HopClipboardPayload) { gate.withLock { value = payload; sequence += 1 } }
        func block() { gate.withLock { blockNext = true } }
        func write(_ payload: HopClipboardPayload) -> Int? { write(payload, permitted: { true }) }
        func write(_ payload: HopClipboardPayload, permitted: () -> Bool) -> Int? {
            let block = gate.withLock {
                let block = blockNext
                blockNext = false; waiting = block
                return block
            }
            if block { _ = releaseWrite.wait(timeout: .now() + .seconds(5)) }
            let authorized = permitted()
            return gate.withLock {
                waiting = false; attempts += 1
                guard authorized else { return nil }
                value = payload; sequence += 1; writes += 1
                return sequence
            }
        }
    }

    private final class State {
        let gate = NSLock()
        var port: UInt16?
        var ready = 0, connected = 0, disconnects = 0, restoreCount = 0
        var focus: V2HostCoordinator.Focus?
        var failure: String?
    }

    static func main() throws {
        let hostID = try V2IdentityFactory.generate(), receiverID = try V2IdentityFactory.generate()
        let hostTrust = V2TrustStore(blobs: MemoryStore()), receiverTrust = V2TrustStore(blobs: MemoryStore())
        try hostTrust.saveVerified(.verified(name: "Receiver", certificatePEM: receiverID.certificatePEM, comparedFingerprint: receiverID.fingerprint!))
        try receiverTrust.saveVerified(.verified(name: "Host", certificatePEM: hostID.certificatePEM, comparedFingerprint: hostID.fingerprint!))
        let initial = HopClipboardPayload.text(Data(repeating: 65, count: 40 * 1024))
        let before = HopClipboardPayload.text(Data("Receiver before enter".utf8))
        let hostBoard = Board(initial), receiverBoard = Board(before)
        let state = State(), callback = DispatchQueue(label: "test.v2.receiver.clipboard")
        let host = try V2HostCoordinator(deviceName: "Host", identity: hostID, trustStore: hostTrust,
                                         callbackQueue: callback, onEvent: { event in
            state.gate.withLock {
                switch event {
                case .listening(let port): state.port = port
                case .connected(_, let screens) where !screens.isEmpty: state.connected += 1
                case .focused(let focus): state.focus = focus
                case .disconnected: state.disconnects += 1
                default: break
                }
            }
        }, restore: { _ in true }, clipboardBackend: { hostBoard })
        let receiver = V2ReceiverCoordinator(identity: receiverID, name: "Receiver", trust: receiverTrust,
                                             callbackQueue: callback, clipboardBackend: { receiverBoard }) { event in
            state.gate.withLock {
                switch event {
                case .ready: state.ready += 1
                case .failed(let reason): state.failure = reason
                default: break
                }
            }
        }
        defer { receiver.drainOnQuit(); host.stop() }
        let screen: [UInt8] = [1,3,0,0,0,1,0,0, 0,0,0,0,0,0,0,7, 0,0,0,0,0,0,0,0, 0,0,7,128,0,0,4,56]
        let input = V2ReceiverCoordinator.Input(screenIDs: [7], screenAdvertisement: screen, enter: { _, _ in true }, input: { _, _ in true },
                                                restore: { state.gate.withLock { state.restoreCount += 1 }; return true }, takeLeave: { nil })
        var checks = 0
        func check(_ condition: Bool, _ label: String) throws {
            checks += 1
            if !condition { throw WireError.invalid("Mac receiver clipboard: " + label) }
        }
        func until(_ label: String, _ condition: () -> Bool) throws {
            let deadline = DispatchTime.now() + .seconds(4)
            repeat {
                if condition() { return }
                Thread.sleep(forTimeInterval: 0.002)
            } while DispatchTime.now() < deadline
            throw WireError.invalid("Mac receiver clipboard: timed out \(label); \(state.gate.withLock { state.failure ?? "no receiver error" })")
        }
        func focus() throws -> UInt64 {
            state.gate.withLock { state.focus = nil }
            var nextAttempt = DispatchTime.now()
            try until("acknowledged focus") {
                if state.gate.withLock({ state.focus != nil }) { return true }
                if DispatchTime.now() >= nextAttempt {
                    host.enter(peerFingerprint: receiverID.fingerprint, screen: 7, x: 20, y: 40)
                    nextAttempt = .now() + .milliseconds(50)
                }
                return false
            }
            let epoch = state.gate.withLock { state.focus!.epoch }
            host.activateClipboard(focusEpoch: epoch)
            return epoch
        }

        receiver.setClipboardEnabled(true)
        host.setClipboardEnabled(true)
        try check(receiverBoard.snapshot.probes == 0, "preflight requires a verified session")
        host.start(port: 0)
        try until("host listens") { state.gate.withLock { state.port != nil } }
        let address = try V2ReceiverAddress(host: "127.0.0.1", port: state.gate.withLock { state.port! })
        try check(receiver.connect(address: address, fingerprint: hostID.fingerprint, input: input), "pinned receiver starts")
        try until("known ready") { state.gate.withLock { state.ready == 1 && state.connected == 1 } }
        try until("receiver baseline sampled") { receiverBoard.snapshot.probes > 0 }
        try check(receiverBoard.snapshot.reads == 0 && hostBoard.snapshot.probes == 0, "idle verified receiver samples only metadata")
        _ = try focus()
        try until("receiver applies initial text") { receiverBoard.snapshot.writes == 1 }
        try check(receiverBoard.snapshot.value == initial, "multi-record initial snapshot arrives before target updates")
        let target = HopClipboardPayload.text(Data("Receiver live copy".utf8))
        receiverBoard.copy(target)
        try until("host gets receiver live update") { hostBoard.snapshot.writes == 1 }
        try check(hostBoard.snapshot.value == target, "target clipboard is bidirectional after initial drain")

        let baselineProbes = receiverBoard.snapshot.probes
        host.release()
        try until("first release fully acknowledged") { state.gate.withLock { state.ready == 2 } }
        try until("next receiver baseline sampled") { receiverBoard.snapshot.probes > baselineProbes }
        let hostNext = HopClipboardPayload.text(Data("Host new snapshot".utf8))
        hostBoard.copy(hostNext)
        receiverBoard.block()
        let epoch = try focus()
        try until("receiver initial apply waits before commit") { receiverBoard.snapshot.waiting }
        let targetWins = HopClipboardPayload.text(Data("Receiver changed during initial apply".utf8))
        receiverBoard.copy(targetWins)
        Thread.sleep(forTimeInterval: 0.16)
        try check(hostBoard.snapshot.value == hostNext && receiverBoard.snapshot.writes == 1,
                  "target cannot send local content before INITIAL_DONE and queued writes drain")
        receiverBoard.releaseWrite.signal()
        try until("receiver sends winning local update") { hostBoard.snapshot.value == targetWins }
        try check(receiverBoard.snapshot.value == targetWins && receiverBoard.snapshot.writes == 1,
                  "prewrite version check protects local change from initial snapshot")

        receiverBoard.block()
        hostBoard.copy(.text(Data("Revoked on target".utf8)))
        try until("target write is blocked") { receiverBoard.snapshot.waiting }
        let priorRestore = state.gate.withLock { state.restoreCount }
        host.release()
        try until("receiver restores before its clipboard actor drains") {
            state.gate.withLock { state.restoreCount > priorRestore }
        }
        try check(receiverBoard.snapshot.waiting && !host.submitInput([[1,4,0,0,0,4,0,0,1,0,0,0]], focusEpoch: epoch),
                  "blocked target actor cannot retain input focus")
        try until("receiver actor timeout closes the connection") { state.gate.withLock { state.disconnects == 1 } }
        try check(state.gate.withLock { state.ready == 2 } && receiverBoard.snapshot.waiting,
                  "RELEASE_ACK withheld when target actor exceeds barrier timeout")
        receiverBoard.releaseWrite.signal()
        try until("revoked actor write returns") { receiverBoard.snapshot.attempts == 3 }
        try check(receiverBoard.snapshot.value == targetWins && receiverBoard.snapshot.writes == 1, "revoked write cannot mutate clipboard")

        try until("receiver closes after barrier timeout") { state.gate.withLock { state.failure != nil } }
        receiver.setClipboardEnabled(false)
        let disabledProbes = receiverBoard.snapshot.probes
        try check(receiver.connect(address: address, fingerprint: hostID.fingerprint, input: input), "reconnect with receiver clipboard disabled")
        try until("disabled receiver ready") { state.gate.withLock { state.ready == 3 && state.connected == 2 } }
        let inputOnlyEpoch = try focus()
        try check(host.submitInput([[1,4,0,0,0,4,0,0,1,0,0,0]], focusEpoch: inputOnlyEpoch), "disabled clipboard does not disable input")
        Thread.sleep(forTimeInterval: 0.2)
        try check(receiverBoard.snapshot.probes == disabledProbes && receiverBoard.snapshot.writes == 1,
                  "disabled receiver does not sample or write its clipboard")
        print("PASS \(checks) Mac receiver clipboard checks; pinned TCP and memory pasteboards")
    }
}
