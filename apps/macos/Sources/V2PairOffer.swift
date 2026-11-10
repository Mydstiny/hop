import Foundation

/// HPOP is a public, unauthenticated pairing candidate. It must never be
/// treated as trust without independent fingerprint comparison and pinned TLS.
struct V2PairOffer: Equatable {
    let role: UInt8
    let nonce: [UInt8]
    let name, certificatePEM: String
    static let headerSize = 28, maxName = 128, maxCertificate = 16384
    var valid: Bool {
        guard (role == 1 || role == 2), nonce.count == 16, nonce.contains(where: { $0 != 0 }),
              !name.isEmpty, name.utf8.count <= Self.maxName, !certificatePEM.isEmpty,
              certificatePEM.utf8.count <= Self.maxCertificate, V2Certificate.fingerprint(certificatePEM) != nil else { return false }
        return name.unicodeScalars.allSatisfy { scalar in
            let v = scalar.value
            return !(v < 0x20 || (0x7f...0x9f).contains(v) || v == 0x061c || v == 0x200e || v == 0x200f ||
                     (0x2028...0x202e).contains(v) || (0x2066...0x2069).contains(v))
        }
    }
    func encoded() -> [UInt8]? {
        guard valid else { return nil }
        let n = Array(name.utf8), c = Array(certificatePEM.utf8)
        var result: [UInt8] = [0x48,0x50,0x4f,0x50,1,role,UInt8(n.count >> 8),UInt8(n.count & 255)]
        for shift in (0..<4).reversed() { result.append(UInt8((c.count >> (shift*8)) & 255)) }
        result += nonce; result += n; result += c
        return result
    }
    static func selector(_ fingerprint: String) -> [UInt8]? {
        let bytes=Array(fingerprint.utf8)
        guard bytes.count == 64 && bytes.allSatisfy({ (48...57).contains($0) || (65...70).contains($0) }) else { return nil }
        return [0x48,0x50,0x53,0x4c] + bytes
    }
    static func selectorFingerprint(_ bytes: [UInt8]) -> String? {
        guard bytes.count == 68 && Array(bytes[0..<4]) == [0x48,0x50,0x53,0x4c],
              let result = String(bytes: bytes[4...], encoding: .utf8), selector(result) != nil else { return nil }
        return result
    }
}
final class V2PairOfferDecoder {
    private var bytes = [UInt8](), expected: Int?
    private(set) var failed = false
    private var finished = false
    func feed(_ data: [UInt8]) -> Bool {
        guard !failed && !finished && data.count <= V2PairOffer.headerSize + V2PairOffer.maxName + V2PairOffer.maxCertificate - bytes.count else { failed=true; return false }
        bytes.append(contentsOf: data)
        if bytes.count >= V2PairOffer.headerSize && expected == nil {
            let name = Int(bytes[6]) << 8 | Int(bytes[7])
            let cert = bytes[8..<12].reduce(0) { ($0 << 8) | Int($1) }
            guard Array(bytes[0..<4]) == [0x48,0x50,0x4f,0x50], bytes[4] == 1, (1...2).contains(bytes[5]),
                  (1...V2PairOffer.maxName).contains(name), (1...V2PairOffer.maxCertificate).contains(cert) else { failed=true; return false }
            expected=V2PairOffer.headerSize + name + cert
        }
        if let expected, bytes.count > expected { failed=true; return false }
        return true
    }
    func finish() -> V2PairOffer? {
        guard !failed && !finished, let expected, expected == bytes.count else { failed=true; return nil }
        finished=true
        let nameLength = Int(bytes[6]) << 8 | Int(bytes[7]), start = V2PairOffer.headerSize
        guard let name = String(bytes: bytes[start..<(start+nameLength)], encoding: .utf8),
              let pem = String(bytes: bytes[(start+nameLength)...], encoding: .utf8) else { failed=true; return nil }
        let offer = V2PairOffer(role: bytes[5], nonce: Array(bytes[12..<28]), name: name, certificatePEM: pem)
        guard offer.valid else { failed=true; return nil }
        return offer
    }
}
