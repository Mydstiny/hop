import Foundation

/// Integration-test peer only: loopback listener + real production codec/transport/pairing state.
/// It NEVER creates NSApplication, CGEventTap, hides a cursor, accesses UserDefaults or saves Keychain.
@main struct HeadlessServer {
    static func main() {
        let server = TCPServer()
        let session = PairingSession(identity: [UInt8](repeating: 0x11, count: 16), direction: 1, binding: nil, dimensions: { (2560,1600) })
        session.persist = { _ in } // memory-only test binding
        session.pairingAllowed = true
        var current: UUID?
        var heartbeatAt = monotonicNow()
        func write(_ text: String) { print(text); fflush(stdout) }
        func close(_ reason: UInt8) {
            if let id = current { server.close(id: id, final: Message(type: .bye, payload: [reason])) }
            current = nil; session.reset(); session.pairingAllowed = true
        }
        session.send = { message in if let id = current { server.send(message, id: id) } }
        server.onAccept = { id in current = id; session.connected(); session.pairingAllowed = true; heartbeatAt = monotonicNow() }
        server.onListening = { port in write("LISTEN \(port)") }
        server.onFault = { id, text in
            if id == nil { write("ERROR \(text)"); exit(2) }
            if current == id { close(2) }
        }
        server.onMessage = { id, message in
            guard current == id else { return }
            do {
                if message.type == .bye { close(0); return }
                try session.received(message)
            } catch { session.cancelPairing(); close(1) }
        }
        // Explicitly coordinated test-local confirmation. A production UI does NOT auto-confirm.
        DispatchQueue.global(qos: .utility).async {
            while let line = readLine() {
                DispatchQueue.main.async {
                    switch line {
                    case "confirm":
                        do { try session.confirm(); write("CONFIRMED") } catch { write("ERROR confirmation") }
                    case "status": write("STATE \(session.authenticated ? "authenticated" : "unauthenticated") \(session.binding == nil ? "unbound" : "bound")")
                    case "quit": close(0); server.stop(); exit(0)
                    default: break
                    }
                }
            }
            DispatchQueue.main.async { close(0); server.stop(); exit(0) }
        }
        let timer = Timer.scheduledTimer(withTimeInterval: 0.05, repeats: true) { _ in
            DispatchQueue.main.async {
                guard current != nil else { return }
                do { try session.checkDeadline() } catch { close(2); return }
                if monotonicNow() - heartbeatAt >= 1 {
                    heartbeatAt = monotonicNow()
                    var p = [UInt8](repeating: 0, count: 8); p.put64(0, UInt64(monotonicNow() * 1000))
                    session.send(Message(type: .keepalive, payload: p))
                }
            }
        }
        RunLoop.main.add(timer, forMode: .common)
        server.start(identity: session.identity, port: 0, loopbackOnly: true)
        RunLoop.main.run()
    }
}
