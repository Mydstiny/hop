import Foundation

/// Integration-test peer only: loopback listener + real production codec/transport/pairing state.
/// It NEVER creates NSApplication, CGEventTap, hides a cursor, accesses UserDefaults or saves Keychain.
@main struct HeadlessServer {
    static func main() {
        let arguments = CommandLine.arguments
        let direction: UInt8
        if arguments.count == 1 { direction = 1 }
        else if arguments.count == 3, arguments[1] == "--direction", let value = UInt8(arguments[2]), value <= 1 { direction = value }
        else { fputs("Usage: headless-server [--direction 0|1]\n", stderr); exit(2) }
        let server = TCPServer()
        let session = PairingSession(identity: [UInt8](repeating: 0x11, count: 16), direction: direction, binding: nil, dimensions: { (2560,1600) })
        session.persist = { _ in } // memory-only test binding
        session.pairingAllowed = true
        var current: UUID?
        var heartbeatAt = monotonicNow()
        var epoch: UInt32 = 0
        var entered: Message?
        var enterAcknowledged = false
        func write(_ text: String) { print(text); fflush(stdout) }
        func close(_ reason: UInt8) {
            if let id = current { server.close(id: id, final: Message(type: .bye, payload: [reason])) }
            current = nil; entered = nil; enterAcknowledged = false
            session.reset(); session.pairingAllowed = true
        }
        func pointer(_ type: MessageType, x: UInt16, y: UInt16) -> Message {
            var p = [UInt8](repeating: 0, count: 4); p.put16(0, x); p.put16(2, y)
            return Message(type: type, epoch: epoch, payload: p)
        }
        func key(_ code: UInt16, modifiers: UInt16, down: Bool, repeatKey: Bool = false) -> Message {
            var p = [UInt8](repeating: 0, count: 6); p.put16(0, code); p.put16(2, modifiers); p[4] = down ? 1 : 0; p[5] = repeatKey ? 1 : 0
            return Message(type: .key, epoch: epoch, payload: p)
        }
        func button(_ code: UInt8, down: Bool, mask: UInt16) -> Message {
            var p = [UInt8](repeating: 0, count: 8); p[0] = code; p[1] = down ? 1 : 0
            p.put16(2, 40000); p.put16(4, 35000); p.put16(6, mask)
            return Message(type: .button, epoch: epoch, payload: p)
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
                if message.type == .enter, session.authenticated {
                    guard let expected = entered, !enterAcknowledged, message == expected else { throw WireError.invalid("ENTER test ACK mismatch") }
                    enterAcknowledged = true; session.noteLegalFrame(); write("ENTERED"); return
                }
                if message.type == .requestLeave, session.authenticated {
                    guard let expected = entered, enterAcknowledged, message.epoch == expected.epoch else { throw WireError.invalid("Unexpected test return") }
                    let p = Array(message.payload.prefix(4)) + [UInt8(0)]
                    session.send(Message(type: .leave, epoch: epoch, payload: p))
                    entered = nil; enterAcknowledged = false; session.noteLegalFrame(); write("RETURNED"); return
                }
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
                    case "enter":
                        guard session.authenticated, entered == nil, let peer = session.peer, peer.width > 8 else { write("ERROR enter state"); return }
                        epoch &+= 1; if epoch == 0 { epoch = 1 }
                        let pixel = direction == 1 ? 2.0 : Double(peer.width - 3)
                        let x = UInt16((pixel * 65535.0 / Double(peer.width - 1)).rounded())
                        var p = [UInt8](repeating: 0, count: 8); p.put16(0, x); p.put16(2, 32768)
                        p.put16(4, 1); p.put16(6, 1) // entering with left Shift + left button held
                        let message = Message(type: .enter, epoch: epoch, payload: p)
                        entered = message; enterAcknowledged = false; session.send(message); write("ENTER_SENT")
                    case "exercise":
                        guard enterAcknowledged else { write("ERROR exercise before ENTER ACK"); return }
                        session.send(pointer(.move, x: 32768, y: 32768)) // arm the return edge
                        session.send(key(56, modifiers: 0, down: false))
                        session.send(key(0, modifiers: 1, down: true)) // Shift+A, target letter by physical map
                        session.send(key(0, modifiers: 1, down: true, repeatKey: true))
                        session.send(button(2, down: true, mask: 3))
                        session.send(pointer(.move, x: 40000, y: 35000))
                        session.send(button(2, down: false, mask: 1))
                        session.send(button(1, down: false, mask: 0))
                        var wheel = [UInt8](repeating: 0, count: 8)
                        wheel.put32(0, UInt32(bitPattern: -120)); wheel.put32(4, 240)
                        session.send(Message(type: .wheel, epoch: epoch, payload: wheel))
                        session.send(key(0, modifiers: 1, down: false))
                        session.send(key(56, modifiers: 0, down: false))
                        session.send(key(62, modifiers: 8, down: true))
                        session.send(key(62, modifiers: 0, down: false))
                        // Deliberately held on return: release Meta+B/left button and restore the Caps latch.
                        session.send(key(57, modifiers: 256, down: true))
                        session.send(key(11, modifiers: 320, down: true))
                        session.send(button(1, down: true, mask: 1)); write("EXERCISED")
                    case "return":
                        guard enterAcknowledged else { write("ERROR return state"); return }
                        session.send(pointer(.move, x: direction == 1 ? 0 : 65535, y: 35000)); write("RETURN_SENT")
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
