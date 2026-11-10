import Foundation

/// Converts captured Mac input to v2 payloads. The authenticated session owns
/// focus, sequencing and shortcut translation; this adapter does not grant or
/// change focus and must only be used for the currently active remote screen.
enum V2CapturedInput {
    static func payloads(_ message: Message, screen: UInt64, keys: HIDKeymap) throws -> [[UInt8]] {
        guard screen != 0, message.epoch != 0, (try? Message(body: message.body)) != nil else {
            throw WireError.invalid("Invalid captured input")
        }
        let p = message.payload
        var result: [[UInt8]] = []
        switch message.type {
        case .move:
            var out = [UInt8](repeating: 0, count: 16)
            out[0] = 1; out[1] = 1; out.put64(4, screen); out.put16(12, p.u16(0)); out.put16(14, p.u16(2))
            result = [out]
        case .button:
            var out = [UInt8](repeating: 0, count: 20)
            out[0] = 1; out[1] = 2; out.put64(4, screen); out.put16(12, p.u16(2)); out.put16(14, p.u16(4))
            out[16] = p[0]; out[17] = p[1]; out[18] = UInt8(p.u16(6))
            result = [out]
        case .wheel:
            result = [[1, 3, 0, 0] + Array(p[0..<8])]
        case .key:
            guard let usage = keys.macToHID[p.u16(0)] else { return [] }
            func key(_ down: UInt8, _ repeatKey: UInt8) -> [UInt8] {
                var out = [UInt8](repeating: 0, count: 12)
                out[0] = 1; out[1] = 4; out.put16(4, usage); out.put16(6, p.u16(2)); out[8] = down; out[9] = repeatKey
                return out
            }
            // macOS flagsChanged reports the Caps Lock latch, not a physical
            // down/up pair. Emit a balanced tap on either latch transition.
            result = usage == 0x39 ? [key(1, 0), key(0, 0)] : [key(p[4], p[5])]
        default: throw WireError.invalid("Not a captured input event")
        }
        guard result.allSatisfy({ V2Frame(kind: 2, sequence: 1, epoch: 1, payload: $0).valid }) else {
            throw WireError.invalid("Invalid v2 input payload")
        }
        return result
    }
}
