import Foundation

enum WireError: Error, CustomStringConvertible {
    case invalid(String)
    var description: String { if case .invalid(let reason) = self { return reason }; return "Invalid frame" }
}

enum MessageType: UInt8, CaseIterable {
    case hello = 1, enter, leave, requestLeave, move, button, wheel, key, keepalive, bye
}
enum HelloPhase: UInt8 { case resume = 0, pair, challenge, confirm, ready, commit, reject }

struct Hello: Equatable {
    var phase: HelloPhase
    var role: UInt8 = 1
    var direction: UInt8
    var code: UInt32 = 0
    var width: UInt32
    var height: UInt32
    var id: [UInt8]
    var nonce: [UInt8]
    var token: [UInt8] = Array(repeating: 0, count: 32)
    var name: String

    init(phase: HelloPhase, role: UInt8 = 1, direction: UInt8, code: UInt32 = 0,
         width: UInt32, height: UInt32, id: [UInt8], nonce: [UInt8],
         token: [UInt8] = Array(repeating: 0, count: 32), name: String) {
        self.phase = phase; self.role = role; self.direction = direction; self.code = code
        self.width = width; self.height = height; self.id = id; self.nonce = nonce
        self.token = token; self.name = name
    }
    init(payload p: [UInt8]) throws {
        guard let phase = HelloPhase(rawValue: p[0]), (1...2).contains(p[1]), p[2] <= 1, p[3] == 0,
              (p.u32(4) == 0 || (100000...999999).contains(p.u32(4))),
              (1...65535).contains(p.u32(8)), (1...65535).contains(p.u32(12)),
              p[16..<32].contains(where: { $0 != 0 }), p[32..<48].contains(where: { $0 != 0 }),
              p[112..<120].allSatisfy({ $0 == 0 }),
              let terminator = p[80..<112].firstIndex(of: 0),
              p[terminator..<112].allSatisfy({ $0 == 0 }),
              let name = String(bytes: p[80..<terminator], encoding: .utf8) else {
            throw WireError.invalid("Invalid HELLO fields")
        }
        let token = Array(p[48..<80])
        if [.challenge, .confirm, .commit].contains(phase), p.u32(4) == 0 {
            throw WireError.invalid("Missing pairing code")
        }
        if ![.resume, .commit, .ready].contains(phase), token.contains(where: { $0 != 0 }) {
            throw WireError.invalid("Token disclosed before COMMIT")
        }
        if [.resume, .commit, .ready].contains(phase), !token.contains(where: { $0 != 0 }) {
            throw WireError.invalid("Missing authentication token")
        }
        self.init(phase: phase, role: p[1], direction: p[2], code: p.u32(4), width: p.u32(8),
                  height: p.u32(12), id: Array(p[16..<32]), nonce: Array(p[32..<48]), token: token, name: name)
    }
    var payload: [UInt8] {
        var p = [UInt8](repeating: 0, count: 120)
        p[0] = phase.rawValue; p[1] = role; p[2] = direction
        p.put32(4, code); p.put32(8, width); p.put32(12, height)
        p.replaceSubrange(16..<32, with: id); p.replaceSubrange(32..<48, with: nonce)
        p.replaceSubrange(48..<80, with: token)
        var safeName = ""
        for scalar in name.unicodeScalars {
            let next = safeName + String(scalar)
            if next.utf8.count > 31 { break }; safeName = next
        }
        p.replaceSubrange(80..<(80 + safeName.utf8.count), with: safeName.utf8)
        return p
    }
}

struct Message: Equatable {
    let type: MessageType
    let flags: UInt8
    let epoch: UInt32
    let payload: [UInt8]
    init(type: MessageType, flags: UInt8 = 0, epoch: UInt32 = 0, payload: [UInt8] = []) {
        precondition(payload.count <= 120)
        self.type = type; self.flags = flags; self.epoch = epoch
        self.payload = payload + Array(repeating: 0, count: 120 - payload.count)
    }
    init(body: [UInt8]) throws {
        guard body.count == 128, body[0] == 1, let type = MessageType(rawValue: body[1]),
              body[3] == 0, (type == .hello ? body[2] <= 1 : body[2] == 0) else {
            throw WireError.invalid("Invalid frame header")
        }
        self.init(type: type, flags: body[2], epoch: body.u32(4), payload: Array(body[8..<128]))
        let zeroEpoch = [.hello, .keepalive, .bye].contains(type)
        guard zeroEpoch == (epoch == 0) else { throw WireError.invalid("Invalid epoch") }
        var used = 0
        switch type {
        case .hello:
            let hello = try Hello(payload: payload)
            guard flags == (hello.role == 1 ? 1 : 0) else { throw WireError.invalid("HELLO ACK/role mismatch") }
            used = 120
        case .enter:
            used = 8
            guard payload.u16(4) & ~0x1ff == 0, payload.u16(6) & ~0x1f == 0 else { throw WireError.invalid("ENTER masks") }
        case .leave:
            used = 5; guard payload[4] <= 2 else { throw WireError.invalid("LEAVE reason") }
        case .requestLeave, .move: used = 4
        case .button:
            used = 8
            guard (1...5).contains(payload[0]), payload[1] <= 1, payload.u16(6) & ~0x1f == 0,
                  ((payload.u16(6) >> (payload[0] - 1)) & 1) == UInt16(payload[1]) else { throw WireError.invalid("Button fields") }
        case .wheel:
            used = 8
            guard (-120000...120000).contains(Int32(bitPattern: payload.u32(0))),
                  (-120000...120000).contains(Int32(bitPattern: payload.u32(4))) else { throw WireError.invalid("Wheel bounds") }
        case .key:
            used = 6
            guard payload.u16(0) < 128, payload.u16(2) & ~0x1ff == 0, payload[4] <= 1, payload[5] <= 1,
                  !(payload[4] == 0 && payload[5] != 0) else { throw WireError.invalid("Key fields") }
        case .keepalive: used = 8
        case .bye: used = 1; guard payload[0] <= 4 else { throw WireError.invalid("BYE reason") }
        }
        guard payload[used..<120].allSatisfy({ $0 == 0 }) else { throw WireError.invalid("Nonzero reserved bytes") }
    }
    var body: [UInt8] {
        var b: [UInt8] = [1, type.rawValue, flags, 0, 0, 0, 0, 0]
        b.put32(4, epoch); return b + payload
    }
    var frame: Data { Data([0, 0, 0, 128] + body) }
    static func hello(_ hello: Hello, ack: Bool = true) -> Message {
        Message(type: .hello, flags: ack ? 1 : 0, payload: hello.payload)
    }
    static func point(_ type: MessageType, epoch: UInt32, x: UInt16, y: UInt16, extra: [UInt8] = []) -> Message {
        var p: [UInt8] = [0, 0, 0, 0]; p.put16(0, x); p.put16(2, y)
        return Message(type: type, epoch: epoch, payload: p + extra)
    }
}

/// Bounded incremental reader. Rejects invalid length after four bytes, before allocating a body.
struct FrameDecoder {
    private var current = [UInt8]()
    mutating func feed(_ data: Data) throws -> [Message] {
        var result = [Message]()
        for byte in data {
            current.append(byte)
            if current.count == 4 && current != [0, 0, 0, 128] { throw WireError.invalid("Length must equal 128") }
            if current.count == 132 {
                result.append(try Message(body: Array(current[4..<132])))
                current.removeAll(keepingCapacity: true)
            }
        }
        return result
    }
    var hasPartialFrame: Bool { !current.isEmpty }
}

extension Array where Element == UInt8 {
    func u16(_ i: Int) -> UInt16 { UInt16(self[i]) << 8 | UInt16(self[i + 1]) }
    func u32(_ i: Int) -> UInt32 { UInt32(u16(i)) << 16 | UInt32(u16(i + 2)) }
    func u64(_ i: Int) -> UInt64 { UInt64(u32(i)) << 32 | UInt64(u32(i + 4)) }
    mutating func put16(_ i: Int, _ value: UInt16) { self[i] = UInt8(value >> 8); self[i + 1] = UInt8(value & 255) }
    mutating func put32(_ i: Int, _ value: UInt32) { put16(i, UInt16(value >> 16)); put16(i + 2, UInt16(value & 65535)) }
    mutating func put64(_ i: Int, _ value: UInt64) { put32(i, UInt32(value >> 32)); put32(i + 4, UInt32(value & 0xffffffff)) }
}
func monotonicNow() -> TimeInterval { ProcessInfo.processInfo.systemUptime }
func normalized(_ value: Double, dimension: Double) -> UInt16 {
    guard dimension > 1 else { return 0 }
    return UInt16(max(0, min(65535, (value * 65535 / (dimension - 1)).rounded())))
}
func pixel(_ value: UInt16, dimension: Double) -> Double { (Double(value) * max(0, dimension - 1) / 65535).rounded() }
