import Foundation
import Darwin

@main struct V2TLSSocketTests {
    static func main() throws {
        var checks = 0
        func check(_ ok: Bool, _ message: String) { checks += 1; if !ok { fputs("FAIL v2 TLS bridge: \(message)\n",stderr); exit(1) } }
        func load(_ cert: String, _ key: String) throws -> V2LocalIdentity {
            V2LocalIdentity(certificatePEM: try String(contentsOfFile: cert, encoding: .utf8), privateKeyPEM: try String(contentsOfFile: key, encoding: .utf8))
        }
        let a = try load(CommandLine.arguments[1], CommandLine.arguments[2])
        let b = try load(CommandLine.arguments[3], CommandLine.arguments[4])
        check(a.valid && b.valid && a.fingerprint != b.fingerprint, "temporary identities")
        let generated = try V2IdentityFactory.generate()
        check(generated.valid && generated.fingerprint != a.fingerprint, "native generator produces a valid distinct device identity")
        let listener = socket(AF_INET, SOCK_STREAM, 0)
        var address = sockaddr_in(); address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        address.sin_family = sa_family_t(AF_INET); address.sin_addr.s_addr = in_addr_t(0x7f000001).bigEndian
        let bindStatus = withUnsafePointer(to: &address) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(listener, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } }
        check(listener >= 0 && bindStatus == 0 && listen(listener, 1) == 0, "loopback listener")
        var size = socklen_t(MemoryLayout<sockaddr_in>.size)
        let got = withUnsafeMutablePointer(to: &address) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { getsockname(listener, $0, &size) } }
        check(got == 0, "loopback address")
        let clientFD = socket(AF_INET, SOCK_STREAM, 0)
        let connected = withUnsafePointer(to: &address) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { connect(clientFD, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } }
        let serverFD = accept(listener,nil,nil); Darwin.close(listener)
        check(clientFD >= 0 && connected == 0 && serverFD >= 0, "connected loopback")
        let client = try V2TLSSocket.adopt(fd: clientFD, client: true, local: a, verifiedPeerPEM: b.certificatePEM, verifiedFingerprint: b.fingerprint!)
        let server = try V2TLSSocket.adopt(fd: serverFD, client: false, local: b, verifiedPeerPEM: a.certificatePEM, verifiedFingerprint: a.fingerprint!)
        check(!client.isAuthenticated && client.pollDescriptor == clientFD && !client.enqueue([1]), "cannot send before authentication")
        let deadline = Date().addingTimeInterval(5)
        while Date() < deadline && (!client.isAuthenticated || !server.isAuthenticated) {
            if !client.isAuthenticated { _ = try client.handshake() }
            if !server.isAuthenticated { _ = try server.handshake() }
            Thread.sleep(forTimeInterval: 0.001)
        }
        check(client.isAuthenticated && server.isAuthenticated, "mutual pinned TLS handshake")
        let original = (0..<65564).map { UInt8($0 & 255) }
        check(client.enqueue(original) && !client.enqueue([9]), "bounded single pending write")
        var received = [UInt8]()
        while Date() < deadline && (client.hasPendingWrite || received.count < original.count) {
            if client.hasPendingWrite { _ = try client.flush() }
            switch try server.read() { case .bytes(let bytes): received += bytes; case .again: break; case .closed: break }
            Thread.sleep(forTimeInterval: 0.001)
        }
        check(!client.hasPendingWrite && received == original, "bounded partial TLS I/O preserves data")
        var hostRestores = 0, remoteRestores = 0, entered = 0, inputEvents: [[UInt8]] = []
        let host = try V2SessionBridge.start(transport: client, role: .host, platform: 1, sessionEpoch: 42, localScreens: [], now: 0, requirePairing: true,
                                             restore: { hostRestores += 1; return true })
        let remote = try V2SessionBridge.start(transport: server, role: .receiver, platform: 4, sessionEpoch: 0, localScreens: [7], now: 0, requirePairing: true,
                                               enter: { _,_ in entered += 1; return true }, input: { payload,_ in inputEvents.append(payload); return true },
                                               restore: { remoteRestores += 1; return true })
        var sessionNow: UInt64 = 0
        func pumpUntil(_ condition: () -> Bool) throws {
            for _ in 1...2000 {
                sessionNow += 1
                try host.step(now: sessionNow); try remote.step(now: sessionNow)
                if condition() { return }; Thread.sleep(forTimeInterval: 0.001)
            }
            throw WireError.invalid("session loopback timeout")
        }
        try pumpUntil { host.phase == 2 && remote.phase == 2 }
        check(!host.pairingConfirmed && !remote.pairingConfirmed && host.confirmPairing() && remote.confirmPairing(), "both temporary peers explicitly confirm")
        try pumpUntil { host.phase == 4 && remote.phase == 4 }
        check(host.pairingConfirmed && remote.pairingConfirmed, "pairing confirms only on authenticated session")
        let screen: [UInt8] = [1,3,0,0,0,1,0,0, 0,0,0,0,0,0,0,7, 0,0,0,0,0,0,0,0, 0,0,7,128,0,0,4,56]
        check(remote.advertise(screen), "remote advertises actual screen")
        try pumpUntil { (try? host.screens().count) == 1 }
        let visibleScreens = try host.screens()
        check(visibleScreens.count == 1 && visibleScreens[0].id == 7 && visibleScreens[0].width == 1920, "authenticated screen snapshot")
        check(host.enter(epoch: 43, screen: 7, x: 100, y: 200), "request focus")
        try pumpUntil { host.phase == 6 && remote.phase == 6 }
        let key: [UInt8] = [1,4,0,0,0,6,0,0,1,0,0,0]
        check(entered == 1 && host.sendInput(key), "focus acknowledgement enables input")
        try pumpUntil { inputEvents.count == 1 }
        check(inputEvents[0] == key, "authenticated session delivers input payload")
        check(host.release() && hostRestores == 1, "host restores immediately on release")
        try pumpUntil { host.phase == 4 && remote.phase == 4 }
        check(remoteRestores == 1, "receiver releases injected state")
        host.close(); remote.close(); check(!client.isAuthenticated && !server.isAuthenticated && client.pollDescriptor == nil, "close revokes authentication and poll handle")
        let invalid = socket(AF_INET, SOCK_STREAM, 0)
        do { _ = try V2TLSSocket.adopt(fd: invalid, client: true, local: a, verifiedPeerPEM: b.certificatePEM, verifiedFingerprint: String(repeating: "0", count: 64)); check(false, "wrong fingerprint rejected") }
        catch { check(fcntl(invalid, F_GETFD) < 0, "wrong fingerprint rejects and closes descriptor") }
        let c = try load(CommandLine.arguments[5], CommandLine.arguments[6])
        let secondListener = socket(AF_INET, SOCK_STREAM, 0)
        var secondAddress = sockaddr_in(); secondAddress.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        secondAddress.sin_family = sa_family_t(AF_INET); secondAddress.sin_addr.s_addr = in_addr_t(0x7f000001).bigEndian
        let secondBind = withUnsafePointer(to: &secondAddress) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(secondListener, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } }
        check(secondListener >= 0 && secondBind == 0 && listen(secondListener, 1) == 0, "mismatch listener")
        var secondSize = socklen_t(MemoryLayout<sockaddr_in>.size)
        let secondGot = withUnsafeMutablePointer(to: &secondAddress) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { getsockname(secondListener, $0, &secondSize) } }
        let mismatchClientFD = socket(AF_INET, SOCK_STREAM, 0)
        let secondConnect = withUnsafePointer(to: &secondAddress) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { connect(mismatchClientFD, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } }
        let mismatchServerFD = accept(secondListener,nil,nil); Darwin.close(secondListener)
        check(secondGot == 0 && secondConnect == 0 && mismatchServerFD >= 0, "mismatch loopback")
        let mismatchClient = try V2TLSSocket.adopt(fd: mismatchClientFD, client: true, local: a, verifiedPeerPEM: b.certificatePEM, verifiedFingerprint: b.fingerprint!)
        let mismatchServer = try V2TLSSocket.adopt(fd: mismatchServerFD, client: false, local: c, verifiedPeerPEM: a.certificatePEM, verifiedFingerprint: a.fingerprint!)
        var rejected = false
        let mismatchDeadline = Date().addingTimeInterval(5)
        while Date() < mismatchDeadline && !rejected {
            do { _ = try mismatchClient.handshake(); _ = try mismatchServer.handshake() }
            catch { rejected = true }
            Thread.sleep(forTimeInterval: 0.001)
        }
        check(rejected && !mismatchClient.isAuthenticated, "wrong peer certificate is rejected")
        mismatchClient.close(); mismatchServer.close()
        print("PASS \(checks) Mac v2 TLS bridge loopback checks; temporary identities, no app or input")
    }
}
