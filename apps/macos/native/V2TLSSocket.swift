import Foundation
import Darwin

// C ABI from the separately built, certificate-pinned v2 native core.
@_silgen_name("hop_posix_tls_adopt") private func tlsAdopt(_ fd: Int32, _ client: Int32, _ own: UnsafePointer<CChar>, _ key: UnsafePointer<CChar>, _ peer: UnsafePointer<CChar>) -> OpaquePointer?
@_silgen_name("hop_posix_tls_handshake") private func tlsHandshake(_ socket: OpaquePointer) -> Int32
@_silgen_name("hop_posix_tls_read") private func tlsRead(_ socket: OpaquePointer, _ output: UnsafeMutablePointer<UInt8>, _ capacity: Int, _ count: UnsafeMutablePointer<Int>) -> Int32
@_silgen_name("hop_posix_tls_write") private func tlsWrite(_ socket: OpaquePointer, _ input: UnsafePointer<UInt8>, _ length: Int, _ count: UnsafeMutablePointer<Int>) -> Int32
@_silgen_name("hop_posix_tls_wants_write") private func tlsWantsWrite(_ socket: OpaquePointer) -> Int32
@_silgen_name("hop_posix_tls_authenticated_peer") private func tlsAuthenticatedPeer(_ socket: OpaquePointer) -> UnsafePointer<CChar>?
@_silgen_name("hop_posix_tls_free") private func tlsFree(_ socket: OpaquePointer)

/// Nonblocking, single-worker wrapper. The caller supplies timeouts, polling,
/// frame bounds and session ownership; this object never establishes trust.
/// adopt() consumes the descriptor even when validation or setup fails.
final class V2TLSSocket {
    enum Progress { case ready, again, closed }
    enum ReadResult { case bytes([UInt8]), again, closed }
    private var socket: OpaquePointer?
    private let descriptor: Int32
    private let expectedPeer: String
    private var authenticated = false
    private var pending: [UInt8] = []
    private var offset = 0
    private(set) var retryWriteWhenWritable = false
    private init(socket: OpaquePointer, descriptor: Int32, expectedPeer: String) { self.socket = socket; self.descriptor = descriptor; self.expectedPeer = expectedPeer }
    deinit { close() }
    static func adopt(fd: Int32, client: Bool, local: V2LocalIdentity, verifiedPeerPEM: String, verifiedFingerprint: String) throws -> V2TLSSocket {
        guard fd >= 0 else { throw WireError.invalid("Invalid v2 socket") }
        guard local.valid, let ownFingerprint = local.fingerprint,
              let fingerprint = V2Certificate.fingerprint(verifiedPeerPEM), fingerprint == verifiedFingerprint,
              fingerprint != ownFingerprint else { Darwin.close(fd); throw WireError.invalid("Invalid verified v2 identity") }
        let handle = local.certificatePEM.withCString { cert in
            local.privateKeyPEM.withCString { key in
                verifiedPeerPEM.withCString { peer in tlsAdopt(fd, client ? 1 : 0, cert, key, peer) }
            }
        }
        guard let handle else { throw WireError.invalid("Cannot create pinned v2 channel") }
        return V2TLSSocket(socket: handle, descriptor: fd, expectedPeer: fingerprint)
    }
    var isAuthenticated: Bool { authenticated && socket != nil }
    var authenticatedPeerFingerprint: String? { isAuthenticated ? expectedPeer : nil }
    /// For readiness polling only. The TLS object retains descriptor ownership.
    var pollDescriptor: Int32? { socket == nil ? nil : descriptor }
    var wantsWrite: Bool { socket.map { tlsWantsWrite($0) == 1 } ?? false }
    func handshake() throws -> Progress {
        guard let socket else { return .closed }
        let status = tlsHandshake(socket)
        if status == 0 { return .again }
        if status == 2 { close(); return .closed }
        guard status == 1, let actual = tlsAuthenticatedPeer(socket), String(cString: actual) == expectedPeer else {
            close(); throw WireError.invalid("Pinned v2 authentication failed")
        }
        authenticated = true; return .ready
    }
    func read(maximum: Int = 16384) throws -> ReadResult {
        guard let socket else { return .closed }
        guard authenticated && (1...16384).contains(maximum) else { throw WireError.invalid("v2 read before authentication or invalid limit") }
        var bytes = [UInt8](repeating: 0, count: maximum), count = 0
        let status = bytes.withUnsafeMutableBufferPointer { tlsRead(socket, $0.baseAddress!, maximum, &count) }
        if status == 0 { return .again }
        if status == 2 { close(); return .closed }
        guard status == 1 && count > 0 && count <= maximum else { close(); throw WireError.invalid("v2 read failed") }
        return .bytes(Array(bytes.prefix(count)))
    }
    /// One bounded write may be pending. Retry flush() without changing it.
    func enqueue(_ bytes: [UInt8]) -> Bool {
        // Maximum wire chunk includes the 28-byte frame header.
        guard isAuthenticated && pending.isEmpty && !bytes.isEmpty && bytes.count <= 65564 else { return false }
        pending = bytes; offset = 0; return true
    }
    var hasPendingWrite: Bool { !pending.isEmpty }
    func flush() throws -> Progress {
        guard let socket else { return .closed }
        guard authenticated else { throw WireError.invalid("v2 write before authentication") }
        guard !pending.isEmpty else { return .ready }
        var count = 0
        let length = min(16384, pending.count - offset)
        let status = pending.withUnsafeBufferPointer { tlsWrite(socket, $0.baseAddress!.advanced(by: offset), length, &count) }
        if status == 0 { retryWriteWhenWritable = wantsWrite; return .again }
        if status == 2 { close(); return .closed }
        guard status == 1 && count > 0 && count <= length else { close(); throw WireError.invalid("v2 write failed") }
        offset += count
        if offset == pending.count { wipePending(); return .ready }
        retryWriteWhenWritable = true
        return .again
    }
    func close() {
        authenticated = false; wipePending()
        if let socket { tlsFree(socket); self.socket = nil }
    }
    private func wipePending() {
        for index in pending.indices { pending[index] = 0 }
        pending.removeAll(keepingCapacity: false); offset = 0; retryWriteWhenWritable = false
    }
}
