import Foundation
import Security
import CryptoKit

/// This store is not used by v1. It stores only explicitly verified v2 peers;
/// network discovery or a claimed name never creates trust.
struct V2VerifiedPeer: Codable, Equatable {
    let name: String
    let certificatePEM: String
    let fingerprint: String
    static func verified(name: String, certificatePEM: String, comparedFingerprint: String) throws -> V2VerifiedPeer {
        guard !name.isEmpty && name.utf8.count <= 128 && !name.contains("\0"),
              let fingerprint = V2Certificate.fingerprint(certificatePEM),
              comparedFingerprint == fingerprint else { throw WireError.invalid("Certificate fingerprint confirmation failed") }
        return V2VerifiedPeer(name: name, certificatePEM: certificatePEM, fingerprint: fingerprint)
    }
    var valid: Bool {
        !name.isEmpty && name.utf8.count <= 128 && !name.contains("\0") && V2Certificate.fingerprint(certificatePEM) == fingerprint
    }
}
enum V2Certificate {
    static func fingerprint(_ pem: String) -> String? {
        let begin = "-----BEGIN CERTIFICATE-----", end = "-----END CERTIFICATE-----"
        guard !pem.contains("\0"), pem.utf8.count <= 65536 else { return nil }
        let trimmed = pem.trimmingCharacters(in: .whitespacesAndNewlines)
        guard trimmed.hasPrefix(begin), trimmed.hasSuffix(end) else { return nil }
        let body = trimmed.dropFirst(begin.count).dropLast(end.count)
        let encoded = body.filter { !$0.isWhitespace }
        guard let bytes = Data(base64Encoded: encoded), !bytes.isEmpty,
              let certificate = SecCertificateCreateWithData(nil, bytes as CFData),
              SecCertificateCopyData(certificate) as Data == bytes else { return nil }
        return SHA256.hash(data: bytes).map { String(format: "%02X", $0) }.joined()
    }
}
protocol V2SecureBlobStore: AnyObject {
    func read(_ account: String) throws -> Data?
    func write(_ account: String, data: Data) throws
    func delete(_ account: String) throws
}
/// Keychain access only occurs when a future v2 session invokes this adapter.
final class V2KeychainBlobStore: V2SecureBlobStore {
    private let service = "local.hop.macos.security.v2"
    private func query(_ account: String) -> [String: Any] {
        [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service, kSecAttrAccount as String: account]
    }
    func read(_ account: String) throws -> Data? {
        var q = query(account); q[kSecReturnData as String] = true; q[kSecMatchLimit as String] = kSecMatchLimitOne
        var result: CFTypeRef?; let status = SecItemCopyMatching(q as CFDictionary, &result)
        if status == errSecItemNotFound { return nil }
        guard status == errSecSuccess, let data = result as? Data else { throw WireError.invalid("Cannot read v2 Keychain item (\(status))") }
        return data
    }
    func write(_ account: String, data: Data) throws {
        let q = query(account)
        let update = SecItemUpdate(q as CFDictionary, [kSecValueData as String: data] as CFDictionary)
        if update == errSecSuccess { return }
        guard update == errSecItemNotFound else { throw WireError.invalid("Cannot update v2 Keychain item (\(update))") }
        var add = q; add[kSecValueData as String] = data; add[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        let result = SecItemAdd(add as CFDictionary, nil)
        guard result == errSecSuccess else { throw WireError.invalid("Cannot save v2 Keychain item (\(result))") }
    }
    func delete(_ account: String) throws {
        let status = SecItemDelete(query(account) as CFDictionary)
        guard status == errSecSuccess || status == errSecItemNotFound else { throw WireError.invalid("Cannot delete v2 Keychain item (\(status))") }
    }
}
/// Caller must first verify both fingerprints out of band and complete a pinned
/// TLS confirmation. The host and receiver may finish a handoff on different
/// workers; serialize the read/modify/write on the shared store.
final class V2TrustStore {
    private let blobs: V2SecureBlobStore
    private let account = "verified-peers"
    private let gate = NSRecursiveLock()
    init(blobs: V2SecureBlobStore) { self.blobs = blobs }
    func peers() throws -> [V2VerifiedPeer] {
        gate.lock(); defer { gate.unlock() }
        guard let data = try blobs.read(account) else { return [] }
        guard data.count <= 4 * 1024 * 1024 else { throw WireError.invalid("Invalid v2 trust store size") }
        let peers = try JSONDecoder().decode([V2VerifiedPeer].self, from: data)
        guard peers.count <= 64 && peers.allSatisfy(\.valid) && Set(peers.map(\.fingerprint)).count == peers.count else {
            throw WireError.invalid("Invalid v2 trust store")
        }
        return peers
    }
    func saveVerified(_ peer: V2VerifiedPeer) throws {
        gate.lock(); defer { gate.unlock() }
        guard peer.valid else { throw WireError.invalid("Invalid verified certificate") }
        var all = try peers()
        if let index = all.firstIndex(where: { $0.fingerprint == peer.fingerprint }) { all[index] = peer }
        else { guard all.count < 64 else { throw WireError.invalid("Too many trusted devices") }; all.append(peer) }
        let data = try JSONEncoder().encode(all)
        guard data.count <= 4 * 1024 * 1024 else { throw WireError.invalid("Trust store exceeds limit") }
        try blobs.write(account, data: data)
    }
    func revoke(fingerprint: String) throws {
        gate.lock(); defer { gate.unlock() }
        var all = try peers(); let previous = all.count
        all.removeAll { $0.fingerprint == fingerprint }
        if all.count != previous { try blobs.write(account, data: JSONEncoder().encode(all)) }
    }
}

/// In-memory first-pair state only. The UI must display *both* complete
/// fingerprints and obtain the user's independent comparison; handshake and
/// peer confirmation callbacks must come from the exact pinned TLS connection.
final class V2PairingConfirmation {
    enum State { case waitingForComparison, waitingForTLS, waitingForPeer, ready, committed, cancelled }
    private(set) var state: State = .waitingForComparison
    let localFingerprint, peerFingerprint: String
    private let peerName, peerPEM: String
    private let now: () -> TimeInterval
    private let expires: TimeInterval
    private var verifiedPeer: V2VerifiedPeer?
    init(localPEM: String, peerPEM: String, peerName: String, now: @escaping () -> TimeInterval = { ProcessInfo.processInfo.systemUptime }) throws {
        guard let local = V2Certificate.fingerprint(localPEM), let peer = V2Certificate.fingerprint(peerPEM), local != peer,
              !peerName.isEmpty && peerName.utf8.count <= 128 && !peerName.contains("\0") else { throw WireError.invalid("Invalid v2 pairing certificates") }
        localFingerprint = local; peerFingerprint = peer; self.peerName = peerName; self.peerPEM = peerPEM
        self.now = now; expires = now() + 120
    }
    private func require(_ expected: State) throws {
        guard state == expected && now() < expires else { state = .cancelled; throw WireError.invalid("v2 pairing expired or invalid state") }
    }
    func confirmComparison(peerOnLocal: String, localOnPeer: String) throws {
        try require(.waitingForComparison)
        guard localOnPeer == localFingerprint && peerOnLocal == peerFingerprint else {
            state = .cancelled; throw WireError.invalid("v2 fingerprint mismatch")
        }
        verifiedPeer = try V2VerifiedPeer.verified(name: peerName, certificatePEM: peerPEM, comparedFingerprint: peerOnLocal)
        state = .waitingForTLS
    }
    func pinnedTLSReady(authenticatedPeerFingerprint: String) throws {
        try require(.waitingForTLS)
        guard authenticatedPeerFingerprint == peerFingerprint else { state = .cancelled; throw WireError.invalid("v2 authenticated peer changed") }
        state = .waitingForPeer
    }
    func peerConfirmedOnPinnedTLS() throws { try require(.waitingForPeer); state = .ready }
    func commit(to store: V2TrustStore) throws {
        try require(.ready)
        guard let verifiedPeer else { state = .cancelled; throw WireError.invalid("Missing v2 peer") }
        try store.saveVerified(verifiedPeer)
        state = .committed
    }
    func cancel() { if state != .committed { verifiedPeer = nil; state = .cancelled } }
}

/// Device identity is kept in a separate Keychain item from peer trust. The
/// current TLS generator produces P-256 PEM; unsupported or mismatched material
/// is rejected instead of silently replacing an existing identity.
struct V2LocalIdentity: Codable {
    let certificatePEM, privateKeyPEM: String
    var fingerprint: String? { V2Certificate.fingerprint(certificatePEM) }
    var valid: Bool {
        guard certificatePEM.utf8.count <= 65536, privateKeyPEM.utf8.count <= 65536,
              !privateKeyPEM.contains("\0"), fingerprint != nil,
              let key = try? P256.Signing.PrivateKey(pemRepresentation: privateKeyPEM) else { return false }
        let begin = "-----BEGIN CERTIFICATE-----", end = "-----END CERTIFICATE-----"
        let trimmed = certificatePEM.trimmingCharacters(in: .whitespacesAndNewlines)
        guard trimmed.hasPrefix(begin), trimmed.hasSuffix(end) else { return false }
        let body = trimmed.dropFirst(begin.count).dropLast(end.count).filter { !$0.isWhitespace }
        guard let data = Data(base64Encoded: body), let certificate = SecCertificateCreateWithData(nil, data as CFData),
              let publicKey = SecCertificateCopyKey(certificate),
              let external = SecKeyCopyExternalRepresentation(publicKey, nil) as Data? else { return false }
        return external == key.publicKey.x963Representation
    }
}
final class V2LocalIdentityStore {
    private let blobs: V2SecureBlobStore
    private let account = "local-identity"
    init(blobs: V2SecureBlobStore) { self.blobs = blobs }
    func load() throws -> V2LocalIdentity? {
        guard let data = try blobs.read(account) else { return nil }
        guard data.count <= 160 * 1024 else { throw WireError.invalid("Invalid v2 local identity size") }
        let identity = try JSONDecoder().decode(V2LocalIdentity.self, from: data)
        guard identity.valid else { throw WireError.invalid("Invalid v2 local identity; explicit recovery required") }
        return identity
    }
    func saveNew(_ identity: V2LocalIdentity) throws {
        guard try load() == nil else { throw WireError.invalid("A v2 identity already exists") }
        guard identity.valid else { throw WireError.invalid("Invalid v2 local identity") }
        try blobs.write(account, data: JSONEncoder().encode(identity))
    }
}
