import Foundation

/// v2 is not used by the shipping v1 listener. Feed this decoder only after
/// mutually authenticated TLS and perform session/focus/capability checks on
/// each validated frame before using its contents.
struct V2Frame: Equatable {
    let kind: UInt8
    let sequence, epoch: UInt64
    let payload: [UInt8]
    static let headerSize = 28, controlMax = 4096, chunkMax = 65536
    private static func read(_ bytes: [UInt8], _ at: Int, _ size: Int) -> UInt64 {
        var value: UInt64 = 0
        for byte in bytes[at..<(at + size)] { value = (value << 8) | UInt64(byte) }
        return value
    }
    private static func signed(_ bytes: [UInt8], _ at: Int) -> Int64 {
        Int64(Int32(bitPattern: UInt32(read(bytes, at, 4))))
    }
    private static func zero(_ bytes: [UInt8], _ start: Int, _ count: Int) -> Bool {
        bytes[start..<(start + count)].allSatisfy { $0 == 0 }
    }
    var valid: Bool {
        let p = payload, n = p.count
        guard sequence != 0, epoch != 0, n >= 4, p[0] == 1, p[2] == 0, p[3] == 0 else { return false }
        func u16(_ i: Int) -> UInt64 { Self.read(p, i, 2) }
        func u32(_ i: Int) -> UInt64 { Self.read(p, i, 4) }
        func u64(_ i: Int) -> UInt64 { Self.read(p, i, 8) }
        switch kind {
        case 1:
            guard n <= Self.controlMax else { return false }
            switch p[1] {
            case 1, 2: return n == 16 && p[4] == (p[1] == 1 ? 1 : 2) && (1...4).contains(p[5]) && Self.zero(p,6,2) && u64(8) & ~UInt64(63) == 0
            case 3:
                guard n >= 8 && Self.zero(p,6,2) else { return false }
                let count = Int(u16(4)); guard count <= 64 && n == 8 + count * 24 else { return false }
                var ids = Set<UInt64>()
                for i in 0..<count {
                    let at = 8 + i * 24, id = u64(at), width = u32(at+16), height = u32(at+20)
                    guard id != 0 && ids.insert(id).inserted && (1...65535).contains(width) && (1...65535).contains(height),
                          Self.signed(p,at+8) + Int64(width) <= Int64(Int32.max),
                          Self.signed(p,at+12) + Int64(height) <= Int64(Int32.max) else { return false }
                }
                return true
            case 0x04, 0x05: return n == 4
            case 0x10: return n == 20 && u64(4) != 0 && u16(16) & ~UInt64(0x1ff) == 0 && p[18] & ~UInt8(31) == 0 && p[19] == 0
            case 0x11: return n == 12 && u64(4) != 0
            case 0x12, 0x1f: return n == 8 && p[4] <= 2 && Self.zero(p,5,3)
            case 0x13, 0x15, 0x16: return n == 4
            case 0x14: return n == 16 && u64(4) != 0
            case 0x1e: return n == 12
            default: return false
            }
        case 2:
            guard n <= Self.controlMax else { return false }
            switch p[1] {
            case 1: return n == 16 && u64(4) != 0
            case 2: return n == 20 && u64(4) != 0 && (1...5).contains(p[16]) && p[17] <= 1 && p[18] & ~UInt8(31) == 0 && p[19] == 0 && ((p[18] & (1 << (p[16]-1))) != 0) == (p[17] != 0)
            case 3: return n == 12 && (-120000...120000).contains(Self.signed(p,4)) && (-120000...120000).contains(Self.signed(p,8))
            case 4: return n == 12 && (1...0xe7).contains(u16(4)) && u16(6) & ~UInt64(0x1ff) == 0 && p[8] <= 1 && p[9] <= 1 && (p[8] != 0 || p[9] == 0) && Self.zero(p,10,2)
            default: return false
            }
        case 3:
            guard n <= Self.controlMax else { return false }
            switch p[1] {
            case 1: return n == 20 && u64(4) != 0 && Self.zero(p,13,3) && ((p[12] == 1 && u32(16) <= 2*1024*1024) || (p[12] == 2 && (1...16*1024*1024).contains(u32(16))))
            case 2: return n == 16 && u64(4) != 0 && Self.zero(p,13,3)
            case 3: return n == 4
            default: return false
            }
        case 4:
            return n >= 17 && n <= Self.chunkMax && p[1] == 1 && u64(4) != 0 && u32(12) <= 16*1024*1024 && UInt64(n-16) <= 16*1024*1024 - u32(12)
        default: return false
        }
    }
    func encoded() -> [UInt8]? {
        guard valid else { return nil }
        var out: [UInt8] = [0x48,0x4f,0x50,0x32,2,kind,0,0]
        func append(_ number: UInt64, _ bytes: Int) { for shift in (0..<bytes).reversed() { out.append(UInt8((number >> (shift*8)) & 255)) } }
        append(UInt64(payload.count),4); append(sequence,8); append(epoch,8); out.append(contentsOf: payload)
        return out
    }
}

final class V2FrameDecoder {
    private var header = [UInt8](), body = [UInt8](), targetSize = 0
    private var kind: UInt8 = 0, sequence: UInt64 = 0, epoch: UInt64 = 0, lastSequence: UInt64 = 0
    private(set) var failed = false
    private var finished = false
    private func read(_ bytes: [UInt8], _ start: Int, _ count: Int) -> UInt64 {
        bytes[start..<(start+count)].reduce(0) { ($0 << 8) | UInt64($1) }
    }
    /// The caller bounds each TLS read and serializes calls. Errors close the session.
    func feed(_ bytes: [UInt8], accept: (V2Frame) throws -> Void) throws {
        guard !failed && !finished else { throw WireError.invalid("v2 decoder closed") }
        do {
            var offset = 0
            while offset < bytes.count {
                if header.count < V2Frame.headerSize {
                    let take = min(V2Frame.headerSize - header.count, bytes.count - offset)
                    header.append(contentsOf: bytes[offset..<(offset+take)]); offset += take
                    if header.count < V2Frame.headerSize { continue }
                    guard Array(header[0..<4]) == [0x48,0x4f,0x50,0x32], header[4] == 2, header[6] == 0, header[7] == 0 else { throw WireError.invalid("v2 header") }
                    kind = header[5]; targetSize = Int(read(header,8,4)); sequence = read(header,12,8); epoch = read(header,20,8)
                    guard (1...4).contains(kind), sequence > lastSequence, epoch != 0,
                          targetSize >= 4, targetSize <= (kind == 4 ? V2Frame.chunkMax : V2Frame.controlMax) else { throw WireError.invalid("v2 frame size or sequence") }
                    body.removeAll(keepingCapacity: true)
                }
                let take = min(targetSize - body.count, bytes.count - offset)
                body.append(contentsOf: bytes[offset..<(offset+take)]); offset += take
                if body.count == targetSize {
                    let frame = V2Frame(kind: kind, sequence: sequence, epoch: epoch, payload: body)
                    guard frame.valid else { throw WireError.invalid("v2 payload") }
                    lastSequence = sequence; header.removeAll(keepingCapacity: true); body.removeAll(keepingCapacity: true)
                    try accept(frame)
                }
            }
        } catch { failed = true; throw error }
    }
    func finish() throws {
        guard !failed && !finished && header.isEmpty else { failed = true; throw WireError.invalid("v2 truncated frame") }
        finished = true
    }
}
