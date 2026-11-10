import Foundation

private var checks = 0
private func expect(_ condition: Bool, _ description: String) {
    checks += 1
    if !condition { fputs("FAIL: \(description)\n", stderr); exit(1) }
}
private func rejects(_ description: String, _ body: () throws -> Void) {
    checks += 1
    do { try body(); fputs("FAIL: accepted \(description)\n", stderr); exit(1) } catch {}
}
private func unhex(_ string: String) -> [UInt8] {
    stride(from: 0, to: string.count, by: 2).map {
        let first = string.index(string.startIndex, offsetBy: $0)
        return UInt8(string[first..<string.index(first, offsetBy: 2)], radix: 16)!
    }
}
private struct GoldenV2: Decodable {
    let name: String
    let kind: UInt8
    let sequence, epoch: UInt64
    let payload_hex, frame_hex: String
    let valid: Bool
}
private struct Golden: Decodable { let name: String; let type: UInt8; let epoch: UInt32; let payload_hex: String; let hex: String }
private final class FakeMoveTimer: MoveTimerToken {
    var invalid = false
    let fire: () -> Void
    init(_ fire: @escaping () -> Void) { self.fire = fire }
    func invalidate() { invalid = true }
}
private final class FakeV2InputDelegate: V2InputRouterDelegate {
    var entry: (UInt64, UInt16, UInt16)?
    var inputs: [([UInt8], UInt64)] = []
    var events: [String] = []
    var accept = true, rejectAcknowledgement = false
    var returnY: UInt16?
    func requestV2Entry(screen: UInt64, x: UInt16, y: UInt16, captureEpoch: UInt32) { entry = (screen,x,y) }
    func submitV2Input(_ payloads: [[UInt8]], focusEpoch: UInt64) -> Bool {
        inputs += payloads.map { ($0,focusEpoch) }; return accept
    }
    func requestV2Release() { events.append("release") }
    func acknowledgeV2Capture(_ message: Message) throws { if rejectAcknowledgement { throw WireError.invalid("test") }; events.append("ack") }
    func restoreV2Capture(y: UInt16?) { returnY = y; events.append("restore") }
    func abortV2Session() { events.append("abort") }
}
private final class Clock { var value = 10.0 }
private final class Fixture {
    let clock = Clock()
    let session: PairingSession
    var sent = [Message]()
    var saved = [Binding]()
    let peerID = [UInt8](repeating: 0x22, count: 16)
    var nonce = [UInt8](repeating: 0x31, count: 16)
    init(binding: Binding? = nil, direction: UInt8 = 1) {
        session = PairingSession(identity: [UInt8](repeating: 0x11, count: 16), direction: direction, binding: binding, dimensions: { (2560, 1600) })
        session.now = { [clock] in clock.value }
        session.send = { [weak self] in self?.sent.append($0) }
        session.persist = { [weak self] in self?.saved.append($0) }
        session.random = { count in [UInt8](repeating: 0x77, count: count) }
        session.code = { 123456 }
        session.connected(); session.pairingAllowed = true
    }
    func message(_ phase: HelloPhase, code: UInt32 = 0, token: [UInt8] = [UInt8](repeating: 0, count: 32), id: [UInt8]? = nil, direction: UInt8? = nil) -> Message {
        .hello(Hello(phase: phase, role: 2, direction: direction ?? session.direction, code: code,
                     width: 1920, height: 1080, id: id ?? peerID, nonce: nonce, token: token, name: "Harmony PC"), ack: false)
    }
    func pair() throws { try session.received(message(.pair)) }
    func remoteConfirm() throws { try session.received(message(.confirm, code: 123456)) }
    func commit() throws { try session.received(message(.commit, code: 123456, token: [UInt8](repeating: 0x77, count: 32))) }
    func complete() throws { try pair(); try remoteConfirm(); try session.confirm(); try commit() }
}

@main struct CoreTests {
    static func main() throws {
        let root = URL(fileURLWithPath: CommandLine.arguments[1])
        let vectors = try JSONDecoder().decode([Golden].self, from: Data(contentsOf: root.appendingPathComponent("protocol/golden-vectors.json")))
        expect(vectors.count == 12, "12 logical shared golden messages")
        expect(Set(vectors.map(\.type)).count == 10, "10 top-level types")
        var combined = Data()
        for vector in vectors {
            let bytes = unhex(vector.hex)
            expect(bytes.count == 132, "\(vector.name) fixed frame size")
            let message = try Message(body: Array(bytes[4...]))
            expect(message.type.rawValue == vector.type && message.epoch == vector.epoch, "\(vector.name) header")
            expect(message.payload == unhex(vector.payload_hex), "\(vector.name) payload")
            expect(message.frame == Data(bytes), "\(vector.name) independent Swift/Python golden round-trip")
            combined.append(contentsOf: bytes)
            var fragmented = FrameDecoder(); var decoded = [Message]()
            for byte in bytes { decoded += try fragmented.feed(Data([byte])) }
            expect(decoded == [message] && !fragmented.hasPartialFrame, "\(vector.name) byte fragmentation")
            if message.type == .hello { expect(try Hello(payload: message.payload).payload == message.payload, "HELLO field encoder") }
        }
        for size in 1...132 {
            var decoder = FrameDecoder(); var output = [Message]()
            for start in stride(from: 0, to: combined.count, by: size) { output += try decoder.feed(combined.subdata(in: start..<min(start + size, combined.count))) }
            expect(output.count == 12 && !decoder.hasPartialFrame, "fragment/concatenation chunk \(size)")
        }
        for prefix: [UInt8] in [[0,0,0,0], [0,0,0,127], [0,0,0,129], [255,255,255,255]] {
            rejects("invalid length \(prefix)") { var decoder = FrameDecoder(); _ = try decoder.feed(Data(prefix)) }
        }
        var partial = FrameDecoder(); _ = try partial.feed(Data(unhex(vectors[0].hex).dropLast()))
        expect(partial.hasPartialFrame, "EOF/short frame detectable")
        let base = Array(unhex(vectors[4].hex)[4...])
        for (index, value): (Int, UInt8) in [(0,2), (1,0), (1,11), (2,1), (3,1), (7,0), (127,1)] {
            rejects("invalid header/reserved \(index)") { var b = base; b[index] = value; _ = try Message(body: b) }
        }
        let hello = Array(unhex(vectors[0].hex)[4...])
        for (index, value): (Int, UInt8) in [(2,1), (8,7), (9,3), (10,2), (11,1), (12,1), (16,0xff), (56,1), (120,1)] {
            rejects("invalid HELLO \(index)") { var b = hello; b[index] = value; _ = try Message(body: b) }
        }
        for range in [24..<40, 40..<56] { rejects("zero identity/nonce") { var b = hello; b.replaceSubrange(range, with: repeatElement(UInt8(0), count: 16)); _ = try Message(body: b) } }
        rejects("zero screen width") { var b = hello; b.replaceSubrange(16..<20, with: [0,0,0,0]); _ = try Message(body: b) }
        rejects("unterminated device name") { var b = hello; b.replaceSubrange(88..<120, with: repeatElement(UInt8(65), count: 32)); _ = try Message(body: b) }
        rejects("invalid UTF8") { var b = hello; b[88] = 0xff; _ = try Message(body: b) }
        rejects("nonzero after NUL") { var b = hello; b[119] = 65; _ = try Message(body: b) }
        for type in [MessageType.enter, .button, .key, .wheel, .bye] {
            rejects("invalid \(type) fields") {
                var p = [UInt8](repeating: 0, count: 120)
                switch type {
                case .enter: p.put16(4, 512)
                case .button: p[0] = 6
                case .key: p.put16(0, 128)
                case .wheel: p.put32(0, 120001)
                case .bye: p[0] = 5
                default: break
                }
                _ = try Message(body: Message(type: type, epoch: type == .bye ? 0 : 1, payload: p).body)
            }
        }
        rejects("key-up repeat") { _ = try Message(body: Message(type: .key, epoch: 1, payload: [0,0,0,0,0,1]).body) }
        rejects("button action mask mismatch") { _ = try Message(body: Message(type: .button, epoch: 1, payload: [1,1,0,0,0,0,0,0]).body) }
        let unicode = Hello(phase: .pair, role: 2, direction: 1, width: 1, height: 1, id: [UInt8](repeating: 1, count: 16), nonce: [UInt8](repeating: 2, count: 16), name: String(repeating: "电脑🙂", count: 20))
        let short = try Hello(payload: unicode.payload)
        expect(short.name.utf8.count <= 31 && !short.name.isEmpty, "UTF8-safe name truncation")

        let f = Fixture(); try f.pair()
        expect(f.saved.isEmpty && !f.session.authenticated, "PAIR cannot authenticate or persist")
        expect(try Hello(payload: f.sent.last!.payload).token.allSatisfy { $0 == 0 }, "CHALLENGE never discloses token")
        try f.remoteConfirm(); expect(f.sent.count == 1 && f.saved.isEmpty, "remote confirmation alone cannot commit")
        try f.remoteConfirm(); expect(f.sent.count == 1, "duplicate CONFIRM cannot commit")
        try f.session.confirm(); expect(f.sent.count == 2 && f.saved.isEmpty && !f.session.authenticated, "double confirm sends COMMIT only")
        try f.session.confirm(); try f.remoteConfirm(); expect(f.sent.count == 2, "double confirmation is idempotent")
        try f.commit(); expect(f.session.authenticated && f.saved.count == 1, "persist only on matching client COMMIT ACK")
        expect(try Hello(payload: f.sent.last!.payload).phase == .ready, "READY after durable save")
        let binding = f.session.binding!
        let resume = Fixture(binding: binding); try resume.session.received(resume.message(.resume, token: binding.token))
        expect(resume.session.authenticated, "stored UUID/token resume")
        let wrong = Fixture(binding: binding)
        rejects("bad resume token") { try wrong.session.received(wrong.message(.resume, token: [UInt8](repeating: 3, count: 32))) }
        rejects("bad resume UUID") { try wrong.session.received(wrong.message(.resume, token: binding.token, id: [UInt8](repeating: 4, count: 16))) }
        rejects("no resume-to-pair downgrade") { try wrong.pair() }
        let closed = Fixture(); closed.session.pairingAllowed = false
        rejects("unsolicited pairing") { try closed.pair() }
        let premature = Fixture(); try premature.pair()
        rejects("COMMIT before double confirm") { try premature.commit() }
        rejects("wrong code") { try premature.session.received(premature.message(.confirm, code: 999999)) }
        premature.nonce = [UInt8](repeating: 7, count: 16)
        rejects("nonce changed") { try premature.remoteConfirm() }
        let localFirst = Fixture(); try localFirst.pair(); try localFirst.session.confirm()
        expect(localFirst.sent.count == 1 && localFirst.saved.isEmpty, "local confirmation alone cannot commit")
        try localFirst.remoteConfirm(); try localFirst.commit(); expect(localFirst.session.authenticated, "either confirmation order works")
        let failedSave = Fixture(); try failedSave.pair(); try failedSave.session.confirm(); try failedSave.remoteConfirm()
        failedSave.session.persist = { _ in throw WireError.invalid("simulated Keychain failure") }
        rejects("Keychain failure") { try failedSave.commit() }
        expect(!failedSave.session.authenticated && failedSave.session.binding == nil, "failed Keychain never authenticated")
        let lost = Fixture(); try lost.pair(); try lost.session.confirm(); try lost.remoteConfirm(); lost.session.connected()
        lost.nonce = [UInt8](repeating: 0x45, count: 16)
        try lost.session.received(lost.message(.resume, token: [UInt8](repeating: 0x77, count: 32)))
        expect(lost.session.authenticated && lost.saved.count == 1, "same-process COMMIT-loss resume")
        let cancelled = Fixture(); try cancelled.pair(); try cancelled.session.confirm(); try cancelled.remoteConfirm(); cancelled.session.cancelPairing(); cancelled.session.connected()
        rejects("cancel clears candidate token") { try cancelled.session.received(cancelled.message(.resume, token: [UInt8](repeating: 0x77, count: 32))) }
        let expired = Fixture(); try expired.pair(); try expired.session.confirm(); try expired.remoteConfirm(); expired.session.connected(); expired.clock.value += 61
        rejects("expired half-pair recovery") { try expired.session.received(expired.message(.resume, token: [UInt8](repeating: 0x77, count: 32))) }
        let timeout = Fixture(); timeout.clock.value += 3
        rejects("3 second heartbeat timeout") { try timeout.session.checkDeadline() }
        let keepalive = Message(type: .keepalive, payload: [UInt8](repeating: 0, count: 8))
        let rate = Fixture()
        for _ in 0..<10 { try rate.session.received(keepalive) }
        rejects("heartbeat above 10Hz") { try rate.session.received(keepalive) }
        rate.clock.value += 1; try rate.session.received(keepalive)
        expect(rate.session.lastReceive == rate.clock.value, "legal frame refreshes monotonic deadline")
        let noHello = Fixture()
        for _ in 0..<5 { noHello.clock.value += 1; try noHello.session.received(keepalive) }
        rejects("initial HELLO within5s despite heartbeats") { try noHello.session.checkDeadline() }
        let pairDeadline = Fixture(); try pairDeadline.pair()
        for _ in 0..<60 { pairDeadline.clock.value += 1; try pairDeadline.session.received(keepalive) }
        rejects("pairing upper limit60s despite heartbeats") { try pairDeadline.session.checkDeadline() }
        let left = Fixture(direction: 0); try left.session.received(left.message(.pair, direction: 1))
        expect(try Hello(payload: left.sent.last!.payload).direction == 0, "Mac direction authoritative")
        rejects("client direction changed after challenge") { try left.session.received(left.message(.confirm, code: 123456, direction: 1)) }
        expect(SecureStore.constantTimeEqual([1,2,3], [1,2,3]) && !SecureStore.constantTimeEqual([1,2], [1,3]) && !SecureStore.constantTimeEqual([1], [1,2]), "constant-time token compare result")
        for _ in 0..<20 { let code = try SecureStore.pairingCode(); expect((100000...999999).contains(code), "secure six digit code") }
        expect(try SecureStore.random(32).count == 32, "secure token32 bytes")
        let v2vectors = try JSONDecoder().decode([GoldenV2].self, from: Data(contentsOf: root.appendingPathComponent("protocol/v2-golden-vectors.json")))
        expect(v2vectors.count == 28, "shared v2 vector count")
        for vector in v2vectors {
            let frame = V2Frame(kind: vector.kind, sequence: vector.sequence, epoch: vector.epoch, payload: unhex(vector.payload_hex))
            let wire = unhex(vector.frame_hex)
            expect(frame.valid == vector.valid, "v2 payload validity \(vector.name)")
            let decoder = V2FrameDecoder(); var received: [V2Frame] = []
            if vector.valid {
                expect(frame.encoded() == wire, "v2 golden bytes \(vector.name)")
                for byte in wire { try decoder.feed([byte]) { received.append($0) } }
                try decoder.finish()
                expect(received == [frame], "v2 byte-fragmented decode \(vector.name)")
            } else {
                expect(frame.encoded() == nil, "v2 refuses invalid payload \(vector.name)")
                rejects("v2 invalid payload \(vector.name)") { try decoder.feed(wire) { _ in } }
                expect(decoder.failed, "v2 decode failure closes session \(vector.name)")
            }
        }
        let example = v2vectors.first!, bytes = unhex(example.frame_hex)
        for split in 0...bytes.count {
            let decoder = V2FrameDecoder(); var received = 0
            try decoder.feed(Array(bytes[..<split])) { _ in received += 1 }
            try decoder.feed(Array(bytes[split...])) { _ in received += 1 }
            try decoder.finish(); expect(received == 1, "v2 arbitrary split")
        }
        let replay = V2FrameDecoder(); try replay.feed(bytes) { _ in }
        rejects("v2 repeated sequence") { try replay.feed(bytes) { _ in } }
        let truncated = V2FrameDecoder(); try truncated.feed(Array(bytes.dropLast())) { _ in }
        rejects("v2 truncated EOF") { try truncated.finish() }
        let rejectedCallback = V2FrameDecoder()
        rejects("v2 application rejection") { try rejectedCallback.feed(bytes) { _ in throw WireError.invalid("callback") } }
        expect(rejectedCallback.failed, "v2 callback failure closes decoder")
        let canonical = try Keymap.load(url: root.appendingPathComponent("protocol/keymap.json"))
        let generated = try Keymap.load(url: root.appendingPathComponent("apps/macos/Resources/keymap.json"))
        expect(canonical.keys.count == 120 && canonical.supported.count == 116, "canonical mapping counts")
        expect(generated.supported == canonical.supported && generated.modifiers == canonical.modifiers, "generated resource agrees with canonical keymap")
        let hid = try HIDKeymap(keymap: canonical)
        expect(hid.macToHID.count == hid.hidToMac.count && hid.macToHID.count > 100, "v2 physical mapping is one-to-one")
        expect(hid.macToHID[0] == 0x04 && hid.hidToMac[0x06] == 8, "ANSI letters use HID usages")
        expect(hid.macToHID[55] == 0xe3 && hid.macToHID[54] == 0xe7, "left and right Command stay distinct")
        expect(hid.macToHID[56] == 0xe1 && hid.macToHID[60] == 0xe5 && hid.macToHID[57] == 0x39, "modifier and Caps Lock physical identities")
        expect(hid.macToHID[71] == nil && hid.macToHID[63] == nil, "uncertain keypad clear and Fn are not guessed")
        expect(hid.macToHID.allSatisfy { hid.hidToMac[$0.value] == $0.key }, "every HID identity round-trips")
        let v2Move = try V2CapturedInput.payloads(.point(.move, epoch: 9, x: 123, y: 456), screen: 7, keys: hid)
        expect(v2Move.count == 1 && v2Move[0].u64(4) == 7 && v2Move[0].u16(12) == 123 && v2Move[0].u16(14) == 456, "v2 movement keeps target screen and position")
        let v2Button = try V2CapturedInput.payloads(Message(type: .button, epoch: 9, payload: [4,1,0,12,0,34,0,8]), screen: 7, keys: hid)
        expect(v2Button.count == 1 && Array(v2Button[0][16..<20]) == [4,1,8,0], "v2 side button preserves state")
        let v2Wheel = try V2CapturedInput.payloads(Message(type: .wheel, epoch: 9, payload: [0xff,0xff,0xff,0x88,0,0,0,120]), screen: 7, keys: hid)
        expect(v2Wheel.count == 1 && Array(v2Wheel[0][4..<12]) == [0xff,0xff,0xff,0x88,0,0,0,120], "v2 wheel preserves both signed axes")
        let v2Key = try V2CapturedInput.payloads(Message(type: .key, epoch: 9, payload: [0,55,0,64,1,0]), screen: 7, keys: hid)
        expect(v2Key.count == 1 && v2Key[0].u16(4) == 0xe3 && v2Key[0].u16(6) == 64, "v2 key uses physical HID and modifiers")
        for (latch, down) in [(UInt16(256), UInt8(1)), (UInt16(0), UInt8(0))] {
            var caps = [UInt8](repeating: 0, count: 6); caps.put16(0, 57); caps.put16(2, latch); caps[4] = down
            let events = try V2CapturedInput.payloads(Message(type: .key, epoch: 9, payload: caps), screen: 7, keys: hid)
            expect(events.count == 2 && events[0].u16(4) == 0x39 && events[0][8] == 1 && events[1][8] == 0 && events[1].u16(6) == latch, "v2 Caps Lock latch produces balanced tap")
        }
        let unsupported = try V2CapturedInput.payloads(Message(type: .key, epoch: 9, payload: [0,63,0,0,1,0]), screen: 7, keys: hid)
        expect(unsupported.isEmpty, "unmapped physical key is not guessed")
        rejects("v2 input requires target screen") { _ = try V2CapturedInput.payloads(.point(.move, epoch: 9, x: 0, y: 0), screen: 0, keys: hid) }
        rejects("v2 input rejects invalid button masks") { _ = try V2CapturedInput.payloads(Message(type: .button, epoch: 9, payload: [4,1,0,0,0,0,0,0]), screen: 7, keys: hid) }
        rejects("v2 input rejects non-input messages") { _ = try V2CapturedInput.payloads(.point(.enter, epoch: 9, x: 0, y: 0, extra: [0,0,0,0]), screen: 7, keys: hid) }
        let routedDelegate = FakeV2InputDelegate(), router = V2InputRouter(keymap: hid, delegate: routedDelegate)
        router.selectedScreen = 7
        let entryMessage = Message.point(.enter, epoch: 9, x: 123, y: 456, extra: [0,0,0,0])
        router.captured(entryMessage)
        expect(routedDelegate.entry?.0 == 7 && routedDelegate.entry?.1 == 123 && routedDelegate.entry?.2 == 456 && !router.isActive, "v2 capture waits for remote focus acknowledgement")
        router.focused(epoch: 43, screen: 7, x: 123, y: 456)
        expect(router.isActive && routedDelegate.events == ["ack"], "matching v2 focus acknowledgement enables capture")
        router.captured(Message(type: .key, epoch: 9, payload: [0,0,0,0,1,0]))
        expect(routedDelegate.inputs.count == 1 && routedDelegate.inputs[0].0.u16(4) == 4 && routedDelegate.inputs[0].1 == 43, "v2 router uses active focus and HID mapping")
        router.restore(y: 321)
        expect(!router.isActive && routedDelegate.returnY == 321 && routedDelegate.events == ["ack","restore"], "v2 remote return restores local cursor at supplied position")
        let badDelegate = FakeV2InputDelegate(), badRouter = V2InputRouter(keymap: hid, delegate: badDelegate)
        badRouter.selectedScreen = 7; badRouter.captured(entryMessage); badRouter.focused(epoch: 44, screen: 8, x: 123, y: 456)
        expect(!badRouter.isActive && badDelegate.events == ["restore","abort"], "stale v2 acknowledgement restores before disconnect")
        let movedDelegate = FakeV2InputDelegate(), movedRouter = V2InputRouter(keymap: hid, delegate: movedDelegate)
        movedDelegate.rejectAcknowledgement = true
        movedRouter.selectedScreen = 7; movedRouter.captured(entryMessage); movedRouter.focused(epoch: 44, screen: 7, x: 123, y: 456)
        expect(!movedRouter.isActive && movedDelegate.events == ["restore","release"], "moving away during ENTER_ACK releases focus without dropping the session")
        let fullDelegate = FakeV2InputDelegate(), fullRouter = V2InputRouter(keymap: hid, delegate: fullDelegate)
        fullRouter.selectedScreen = 7; fullRouter.captured(entryMessage); fullRouter.focused(epoch: 45, screen: 7, x: 123, y: 456)
        fullDelegate.accept = false; fullRouter.captured(Message(type: .key, epoch: 9, payload: [0,0,0,0,1,0]))
        expect(!fullRouter.isActive && Array(fullDelegate.events.suffix(2)) == ["restore","abort"], "input queue failure restores local control first")
        let heldDelegate = FakeV2InputDelegate(), heldRouter = V2InputRouter(keymap: hid, delegate: heldDelegate)
        heldRouter.selectedScreen = 7; heldRouter.captured(.point(.enter, epoch: 9, x: 0, y: 0, extra: [0,64,0,0]))
        expect(heldDelegate.entry == nil && heldDelegate.events == ["restore","abort"], "v2 router rejects a held modifier at entry")
        expect(canonical.modifiers.count == 9 && canonical.code(symbol: "kVK_Escape") != nil, "modifier/escape codes derive from canonical map")
        var moves = MoveCoalescer()
        for x in 0..<1000 { moves.offer(.point(.move, epoch: 1, x: UInt16(x), y: 0)) }
        expect(moves.take()?.payload.u16(0) == 999 && moves.take() == nil, "single latest move; no backlog")
        moves.offer(.point(.move, epoch: 2, x: 8, y: 3)); moves.clear()
        expect(moves.take() == nil, "restore/LEAVE clears pending movement")
        var order = [Message](); moves.offer(.point(.move, epoch: 2, x: 9, y: 3))
        if let move = moves.take() { order.append(move) }; order.append(Message(type: .key, epoch: 2, payload: [0,0,0,0,1,0]))
        expect(order.map(\.type) == [.move, .key], "latest move flush precedes nonmove")
        var pending = false, hz = 240, flushCount = 0
        var posted: [() -> Void] = [], timers: [FakeMoveTimer] = [], intervals: [TimeInterval] = []
        let scheduler = MoveForwardingScheduler(hasPending: { pending }, flush: { flushCount += 1; pending = false }, rate: { hz },
            post: { posted.append($0) }, makeTimer: { interval, action in
                intervals.append(interval); let timer = FakeMoveTimer(action); timers.append(timer); return timer
            })
        expect(timers.isEmpty, "no high-frequency timer when idle")
        pending = true; scheduler.movementAvailable(); scheduler.movementAvailable()
        expect(posted.count == 1 && timers.isEmpty, "movement defers one timer request outside event callback")
        posted.removeFirst()()
        expect(timers.count == 1 && abs(intervals[0] - 1.0 / 240) < 0.000001, "pending move starts configured timer")
        scheduler.movementAvailable(); expect(posted.isEmpty && timers.count == 1, "active timer coalesces burst scheduling")
        timers[0].fire(); expect(flushCount == 1 && !pending, "timer flushes pending move")
        timers[0].fire(); expect(timers[0].invalid, "timer stops when movement becomes idle")
        pending = true; scheduler.movementAvailable(); posted.removeFirst()()
        hz = 1000; scheduler.rateChanged()
        expect(timers[1].invalid && timers.count == 3 && abs(intervals[2] - 0.001) < 0.000001, "rate change replaces timer at new rate")
        scheduler.stop(); expect(timers[2].invalid, "stop cancels timer")
        scheduler.movementAvailable(); expect(posted.isEmpty, "stopped scheduler does not restart")
        var delayed: [() -> Void] = [], created = false
        let stopped = MoveForwardingScheduler(hasPending: { true }, flush: {}, rate: { 240 },
            post: { delayed.append($0) }, makeTimer: { _, action in created = true; return FakeMoveTimer(action) })
        stopped.movementAvailable(); stopped.stop(); delayed.removeFirst()()
        expect(!created, "queued scheduling cannot restart after stop")
        expect(normalized(999, dimension: 1) == 0 && pixel(65535, dimension: 1) == 0, "dimension1")
        expect(normalized(1919, dimension: 1920) == 65535 && pixel(65535, dimension: 1920) == 1919, "coordinate boundaries")
        expect(normalized(-8, dimension: 1920) == 0 && normalized(3000, dimension: 1920) == 65535, "coordinate clamp")
        print("PASS: \(checks) Swift core/golden/pairing/keymap/coalescing checks (no app or input capture)")
    }
}
