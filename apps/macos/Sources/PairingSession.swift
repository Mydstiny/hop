import Foundation

/// Main-thread state machine; persistence and cryptographic primitives are injected for offline tests.
final class PairingSession {
    struct Candidate { let hello: Hello; let token: [UInt8]; let code: UInt32; let expires: TimeInterval }
    var binding: Binding?
    private(set) var peer: Hello?
    private(set) var authenticated = false
    private(set) var localConfirmed = false
    private(set) var peerConfirmed = false
    private(set) var commitSent = false
    private(set) var challenge: Candidate?
    private(set) var recovery: Candidate?
    private(set) var connectedAt: TimeInterval = 0
    private(set) var lastReceive: TimeInterval = 0
    private var lastHeartbeatReceived: TimeInterval?
    var pairingAllowed = false
    var identity: [UInt8]
    var direction: UInt8
    var dimensions: () -> (UInt32, UInt32)
    var send: (Message) -> Void = { _ in }
    var persist: (Binding) throws -> Void = SecureStore.save
    var random: (Int) throws -> [UInt8] = SecureStore.random
    var code: () throws -> UInt32 = SecureStore.pairingCode
    var now: () -> TimeInterval = monotonicNow
    var changed: () -> Void = {}
    var name: String = Host.current().localizedName ?? "Mac"

    init(identity: [UInt8], direction: UInt8, binding: Binding?, dimensions: @escaping () -> (UInt32, UInt32)) {
        self.identity = identity; self.direction = direction; self.binding = binding; self.dimensions = dimensions
    }
    func connected() { reset(); connectedAt = now(); lastReceive = connectedAt }
    func reset() {
        // Only a double-confirmed COMMIT may survive a lost connection, at most 60 seconds.
        if let challenge, commitSent, localConfirmed, peerConfirmed { recovery = challenge }
        peer = nil; authenticated = false; localConfirmed = false; peerConfirmed = false
        commitSent = false; challenge = nil; lastHeartbeatReceived = nil
        changed()
    }
    func cancelPairing() { reset(); recovery = nil; pairingAllowed = false; changed() }
    func forget() { cancelPairing(); binding = nil; changed() }
    func received(_ message: Message) throws {
        if message.type == .keepalive {
            if let last = lastHeartbeatReceived, now() - last < 0.1 { throw WireError.invalid("Heartbeat exceeds 10 Hz") }
            lastHeartbeatReceived = now(); lastReceive = now(); return
        }
        guard message.type == .hello else { throw WireError.invalid("Unexpected handshake message") }
        guard !authenticated, message.flags == 0 else { throw WireError.invalid("Unexpected HELLO/ACK") }
        let hello = try Hello(payload: message.payload)
        guard hello.role == 2 else { throw WireError.invalid("Peer must be HarmonyOS") }
        if let peer {
            guard SecureStore.constantTimeEqual(peer.id, hello.id), SecureStore.constantTimeEqual(peer.nonce, hello.nonce),
                  peer.width == hello.width, peer.height == hello.height, hello.direction == direction else {
                throw WireError.invalid("HELLO identity/nonce/dimensions/direction changed")
            }
        }
        lastReceive = now()
        switch hello.phase {
        case .pair:
            guard pairingAllowed, binding == nil, peer == nil, challenge == nil else { throw WireError.invalid("Pairing not enabled or already bound") }
            peer = hello
            challenge = Candidate(hello: hello, token: try random(32), code: try code(), expires: now() + 60)
            send(reply(.challenge, code: challenge!.code)); changed()
        case .confirm:
            guard let challenge, hello.code == challenge.code, now() < challenge.expires, peer != nil else { throw WireError.invalid("Incorrect pairing confirmation") }
            peerConfirmed = true; try maybeCommit(); changed()
        case .commit:
            guard let challenge, commitSent, localConfirmed, peerConfirmed, hello.code == challenge.code,
                  now() < challenge.expires, SecureStore.constantTimeEqual(hello.token, challenge.token) else {
                throw WireError.invalid("Unconfirmed or incorrect COMMIT")
            }
            try finish(hello, token: challenge.token)
        case .resume:
            guard peer == nil else { throw WireError.invalid("Unexpected RESUME") }
            if let binding {
                guard SecureStore.constantTimeEqual(binding.peerID, hello.id), SecureStore.constantTimeEqual(binding.token, hello.token) else {
                    throw WireError.invalid("Binding rejected; explicit re-pair required")
                }
            } else {
                guard let candidate = recovery, candidate.expires > now(),
                      SecureStore.constantTimeEqual(candidate.hello.id, hello.id), SecureStore.constantTimeEqual(candidate.token, hello.token) else {
                    throw WireError.invalid("No matching committed binding")
                }
            }
            peer = hello; try finish(hello, token: hello.token)
        default: throw WireError.invalid("Client HELLO phase not permitted")
        }
    }
    func confirm() throws {
        guard let challenge, now() < challenge.expires else { throw WireError.invalid("Pairing expired") }
        localConfirmed = true; try maybeCommit(); changed()
    }
    private func maybeCommit() throws {
        guard let challenge, localConfirmed, peerConfirmed else { return }
        guard now() < challenge.expires else { throw WireError.invalid("Pairing expired") }
        if !commitSent { commitSent = true; send(reply(.commit, code: challenge.code, token: challenge.token)) }
    }
    private func finish(_ hello: Hello, token: [UInt8]) throws {
        let binding = Binding(peerID: hello.id, token: token, name: hello.name)
        try persist(binding) // Never mark authenticated until Keychain write succeeds.
        self.binding = binding; authenticated = true; recovery = nil; challenge = nil; pairingAllowed = false
        send(reply(.ready, token: token)); changed()
    }
    func reply(_ phase: HelloPhase, code: UInt32 = 0, token: [UInt8] = Array(repeating: 0, count: 32)) -> Message {
        let size = dimensions()
        return .hello(Hello(phase: phase, direction: direction, code: code, width: size.0, height: size.1,
                            id: identity, nonce: peer?.nonce ?? Array(repeating: 0, count: 16), token: token, name: name))
    }
    func noteLegalFrame() { lastReceive = now() }
    func checkDeadline() throws {
        let time = now()
        if let recovery, recovery.expires <= time { self.recovery = nil }
        guard time - lastReceive < 3 else { throw WireError.invalid("Heartbeat timeout (3 seconds)") }
        if !authenticated {
            guard time - connectedAt < (peer == nil ? 5 : 60) else { throw WireError.invalid("Handshake/pairing expired") }
            if let challenge, time >= challenge.expires { throw WireError.invalid("Pairing expired") }
        }
    }
}
