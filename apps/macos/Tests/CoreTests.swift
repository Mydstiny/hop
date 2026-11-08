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
private struct Golden: Decodable { let name: String; let type: UInt8; let epoch: UInt32; let payload_hex: String; let hex: String }
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
        let rate = Fixture(); try rate.session.received(keepalive)
        rejects("heartbeat above 10Hz") { try rate.session.received(keepalive) }
        rate.clock.value += 0.101; try rate.session.received(keepalive)
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
        let canonical = try Keymap.load(url: root.appendingPathComponent("protocol/keymap.json"))
        let generated = try Keymap.load(url: root.appendingPathComponent("apps/macos/Resources/keymap.json"))
        expect(canonical.keys.count == 120 && canonical.supported.count == 116, "canonical mapping counts")
        expect(generated.supported == canonical.supported && generated.modifiers == canonical.modifiers, "generated resource agrees with canonical keymap")
        expect(canonical.modifiers.count == 9 && canonical.code(symbol: "kVK_Escape") != nil, "modifier/escape codes derive from canonical map")
        var moves = MoveCoalescer()
        for x in 0..<1000 { moves.offer(.point(.move, epoch: 1, x: UInt16(x), y: 0)) }
        expect(moves.take()?.payload.u16(0) == 999 && moves.take() == nil, "single latest move; no backlog")
        moves.offer(.point(.move, epoch: 2, x: 8, y: 3)); moves.clear()
        expect(moves.take() == nil, "restore/LEAVE clears pending movement")
        var order = [Message](); moves.offer(.point(.move, epoch: 2, x: 9, y: 3))
        if let move = moves.take() { order.append(move) }; order.append(Message(type: .key, epoch: 2, payload: [0,0,0,0,1,0]))
        expect(order.map(\.type) == [.move, .key], "latest move flush precedes nonmove")
        expect(normalized(999, dimension: 1) == 0 && pixel(65535, dimension: 1) == 0, "dimension1")
        expect(normalized(1919, dimension: 1920) == 65535 && pixel(65535, dimension: 1920) == 1919, "coordinate boundaries")
        expect(normalized(-8, dimension: 1920) == 0 && normalized(3000, dimension: 1920) == 65535, "coordinate clamp")
        print("PASS: \(checks) Swift core/golden/pairing/keymap/coalescing checks (no app or input capture)")
    }
}
