import Foundation
private struct OfferVector: Decodable { let name, type, hex: String; let valid: Bool }
private final class MemoryBlobs: V2SecureBlobStore {
    var values = [String: Data](), writes = 0
    func read(_ account: String) throws -> Data? { values[account] }
    func write(_ account: String, data: Data) throws { writes += 1; values[account] = data }
    func delete(_ account: String) throws { values.removeValue(forKey: account) }
}
@main struct V2TrustTests {
    static func main() throws {
        var checks = 0
        func check(_ condition: Bool, _ message: String) { checks += 1; if !condition { fputs("FAIL v2 trust: \(message)\n", stderr); exit(1) } }
        func rejects(_ message: String, _ body: () throws -> Void) {
            checks += 1
            do { try body(); fputs("FAIL v2 trust accepted: \(message)\n",stderr); exit(1) } catch {}
        }
        let pem = try String(contentsOfFile: CommandLine.arguments[1], encoding: .utf8)
        let fingerprint = CommandLine.arguments[2]
        check(V2Certificate.fingerprint(pem) == fingerprint && fingerprint.count == 64, "matches OpenSSL SHA-256 fingerprint")
        check(V2Certificate.fingerprint(pem + pem) == nil, "multiple certificates rejected")
        check(V2Certificate.fingerprint("bad") == nil && V2Certificate.fingerprint(pem + "garbage") == nil, "invalid certificate rejected")
        rejects("wrong fingerprint") { _ = try V2VerifiedPeer.verified(name: "device", certificatePEM: pem, comparedFingerprint: String(repeating: "0",count:64)) }
        rejects("empty name") { _ = try V2VerifiedPeer.verified(name: "", certificatePEM: pem, comparedFingerprint: fingerprint) }
        let peer = try V2VerifiedPeer.verified(name: "设备", certificatePEM: pem, comparedFingerprint: fingerprint)
        let blobs = MemoryBlobs(), store = V2TrustStore(blobs: blobs)
        check(try store.peers().isEmpty, "empty trust store")
        try store.saveVerified(peer)
        check(try store.peers() == [peer] && blobs.writes == 1, "verified peer stored and revalidated")
        try store.saveVerified(peer)
        check(try store.peers() == [peer], "same fingerprint does not duplicate")
        let forged = V2VerifiedPeer(name: "bad", certificatePEM: pem, fingerprint: String(repeating: "F",count:64))
        rejects("forged fingerprint") { try store.saveVerified(forged) }
        check(try store.peers() == [peer], "invalid write preserves trusted peer")
        try store.revoke(fingerprint: "missing")
        check(try store.peers() == [peer], "unknown revoke leaves trust unchanged")
        try store.revoke(fingerprint: fingerprint)
        check(try store.peers().isEmpty, "revoke removes trusted peer")
        blobs.values["verified-peers"] = Data("broken".utf8)
        rejects("corrupt trust store") { _ = try store.peers() }
        let before = blobs.writes
        rejects("corrupt trust store is not replaced") { try store.saveVerified(peer) }
        check(blobs.writes == before, "failed load does not overwrite trust")
        let peerPEM = try String(contentsOfFile: CommandLine.arguments[3], encoding: .utf8)
        let peerFP = V2Certificate.fingerprint(peerPEM)!
        var clock = 1.0
        let pair = try V2PairingConfirmation(localPEM: pem, peerPEM: peerPEM, peerName: "peer", now: { clock })
        check(pair.localFingerprint == fingerprint && pair.peerFingerprint == peerFP, "pairing shows both fingerprints")
        rejects("TLS before manual comparison") { try pair.pinnedTLSReady(authenticatedPeerFingerprint: peerFP) }
        check(pair.state == .cancelled, "wrong pairing order is terminal")
        let good = try V2PairingConfirmation(localPEM: pem, peerPEM: peerPEM, peerName: "peer", now: { clock })
        try good.confirmComparison(peerOnLocal: peerFP, localOnPeer: fingerprint)
        try good.pinnedTLSReady(authenticatedPeerFingerprint: peerFP)
        try good.peerConfirmedOnPinnedTLS()
        blobs.values.removeAll(); try good.commit(to: store)
        let savedPeers = try store.peers()
        check(good.state == .committed && savedPeers.first?.fingerprint == peerFP, "trust saved only after all confirmations")
        let mismatch = try V2PairingConfirmation(localPEM: pem, peerPEM: peerPEM, peerName: "peer", now: { clock })
        rejects("fingerprints mismatch") { try mismatch.confirmComparison(peerOnLocal: fingerprint, localOnPeer: peerFP) }
        check(mismatch.state == .cancelled, "mismatched fingerprints cancel")
        let timeout = try V2PairingConfirmation(localPEM: pem, peerPEM: peerPEM, peerName: "peer", now: { clock })
        clock += 120
        rejects("expired pairing") { try timeout.confirmComparison(peerOnLocal: peerFP, localOnPeer: fingerprint) }
        check(timeout.state == .cancelled, "expired pairing is cancelled")
        let changed = try V2PairingConfirmation(localPEM: pem, peerPEM: peerPEM, peerName: "peer", now: { clock })
        try changed.confirmComparison(peerOnLocal: peerFP, localOnPeer: fingerprint)
        rejects("changed TLS certificate") { try changed.pinnedTLSReady(authenticatedPeerFingerprint: fingerprint) }
        check(changed.state == .cancelled, "certificate change is terminal")
        let vectors = try JSONDecoder().decode([OfferVector].self, from: Data(contentsOf: URL(fileURLWithPath: CommandLine.arguments[4])))
        for vector in vectors {
            let h = Array(vector.hex); var bytes = [UInt8]()
            for i in stride(from: 0, to: h.count, by: 2) { bytes.append(UInt8(String(h[i...i+1]),radix:16)!) }
            if vector.type == "selector" {
                check((V2PairOffer.selectorFingerprint(bytes) != nil) == vector.valid, "selector vector \(vector.name)")
            } else {
                let decoder = V2PairOfferDecoder(); var accepted = true
                for b in bytes { if !decoder.feed([b]) { accepted = false; break } }
                let offer = accepted ? decoder.finish() : nil
                check((offer != nil) == vector.valid, "offer vector \(vector.name)")
                if let offer { check(offer.encoded() == bytes, "offer byte roundtrip") }
            }
        }
        let ecCert = try String(contentsOfFile: CommandLine.arguments[5], encoding: .utf8)
        let ecKey = try String(contentsOfFile: CommandLine.arguments[6], encoding: .utf8)
        let otherKey = try String(contentsOfFile: CommandLine.arguments[7], encoding: .utf8)
        let identity = V2LocalIdentity(certificatePEM: ecCert, privateKeyPEM: ecKey)
        check(identity.valid && identity.fingerprint != nil, "P-256 local certificate and key match")
        check(!V2LocalIdentity(certificatePEM: ecCert, privateKeyPEM: otherKey).valid, "mismatched local key rejected")
        let identityBlobs = MemoryBlobs(), identities = V2LocalIdentityStore(blobs: identityBlobs)
        check(try identities.load() == nil, "missing identity does not generate or write")
        try identities.saveNew(identity)
        check(try identities.load()?.fingerprint == identity.fingerprint, "local identity roundtrip")
        rejects("identity overwrite") { try identities.saveNew(identity) }
        identityBlobs.values["local-identity"] = Data("corrupt".utf8)
        rejects("corrupt identity") { _ = try identities.load() }
        rejects("corrupt identity is not silently replaced") { try identities.saveNew(identity) }
        print("PASS: \(checks) v2 certificate and trust-store checks using temporary test certificate and memory backend")
    }
}
