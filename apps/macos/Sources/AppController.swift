import AppKit
import ApplicationServices

final class AppController {
    let server = TCPServer()
    let input: InputCapture
    let session: PairingSession
    private(set) var enabled = false
    private(set) var status = "Starting local server…"
    private(set) var listenerPort: UInt16?
    private var connectionID: UUID?
    private var tickTimer: Timer?
    private var moveTimer: Timer?
    private var heartbeatAt: TimeInterval = 0
    private var pairingUntil: TimeInterval = 0
    private var awaitingPermission = false
    private var displaySignature = ""
    private var observers: [NSObjectProtocol] = []
    var changed: () -> Void = {}

    init(keymap: Keymap) throws {
        let direction: UInt8 = UserDefaults.standard.string(forKey: "HopDirection") == "left" ? 0 : 1
        input = InputCapture(keymap: keymap)
        session = PairingSession(identity: SecureStore.identity, direction: direction, binding: try SecureStore.load()) {
            (UInt32(clamping: CGDisplayPixelsWide(CGMainDisplayID())), UInt32(clamping: CGDisplayPixelsHigh(CGMainDisplayID())))
        }
        input.direction = direction
        input.mayEnter = { [weak self] in self?.enabled == true && self?.session.authenticated == true }
        input.peerSize = { [weak self] in (self?.session.peer?.width ?? 1, self?.session.peer?.height ?? 1) }
        input.send = { [weak self] message in self?.send(message) }
        input.fault = { [weak self] text, reason in
            guard let self else { return }
            // InputCapture has ALREADY restored synchronously. Never stop an active tap,
            // touch the network, or fit the popover from inside its callback.
            let generation = self.input.generation, connection = self.connectionID
            DispatchQueue.main.async { [weak self] in
                guard let self, self.input.generation == generation, self.connectionID == connection else { return }
                self.disableForFault(text, byeReason: reason)
            }
        }
        input.changed = { [weak self] in
            // Normal edge detection runs inside CGEventTap: defer all popover layout out of that callback.
            DispatchQueue.main.async { [weak self] in self?.update() }
        }
        session.send = { [weak self] message in self?.send(message) }
        session.changed = { [weak self] in self?.update() }
        server.onListening = { [weak self] port in
            hopLog("listening \(port)")
            self?.listenerPort = port; self?.status = "Listening on TCP \(port)"; self?.update()
        }
        server.onAccept = { [weak self] id in
            guard let self else { return }
            self.connectionID = id; self.session.connected(); self.heartbeatAt = monotonicNow()
            self.status = "Connecting…"; self.update()
        }
        server.onMessage = { [weak self] id, message in self?.receive(id, message: message) }
        server.onFault = { [weak self] id, text in
            guard let self, id == nil || self.connectionID == id else { return }
            self.disconnect(reason: 2, text: text)
            if id == nil { self.server.stop(); self.listenerPort = nil }
        }
    }
    func start() {
        server.start(identity: session.identity)
        if sharingWanted { setEnabled(true) }
        tickTimer = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in self?.tick() }
        moveTimer = Timer.scheduledTimer(withTimeInterval: 1.0 / 120, repeats: true) { [weak self] _ in self?.input.tick() }
        if let tickTimer { RunLoop.main.add(tickTimer, forMode: .common) }
        if let moveTimer { RunLoop.main.add(moveTimer, forMode: .common) }
        PermissionDock.shared.showIfNeeded()
        let workspace = NSWorkspace.shared.notificationCenter
        for name in [NSWorkspace.willSleepNotification, NSWorkspace.screensDidSleepNotification, NSWorkspace.sessionDidResignActiveNotification] {
            observers.append(workspace.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                self?.disableForFault("Sleep/session lock — local input restored; enable Hop to resume", byeReason: 0)
            })
        }
        displaySignature = Self.screenLayout()
        observers.append(NotificationCenter.default.addObserver(forName: NSApplication.didChangeScreenParametersNotification, object: nil, queue: .main) { [weak self] _ in
            guard let self else { return }
            let layout = Self.screenLayout()
            // The Dock, menu bar, and pointer reaching a screen edge post this
            // notification without moving any display. Only a real layout change
            // should drop the session.
            guard layout != self.displaySignature else { return }
            self.displaySignature = layout
            self.disableForFault("Display configuration changed — local input restored", byeReason: 0)
        })
    }
    private func send(_ message: Message) { if let id = connectionID { server.send(message, id: id) } }
    private func receive(_ id: UUID, message: Message) {
        guard connectionID == id else { return }
        do {
            if message.type == .bye {
                if message.payload[0] == 4, !session.authenticated { throw WireError.invalid("Unauthenticated unpair") }
                // FIRST restore, then clear credentials / close / update UI.
                restoreAndNotify(reason: 2)
                if !session.authenticated { session.cancelPairing() } // explicit BYE is cancellation, not COMMIT-loss recovery
                if message.payload[0] == 4 { try SecureStore.delete(); session.forget() }
                close(reason: nil, text: message.payload[0] == 4 ? "Peer unpaired" : "Peer disconnected")
                return
            }
            if message.type == .hello || message.type == .keepalive { try session.received(message); update(); return }
            guard session.authenticated else { throw WireError.invalid("Input before authentication") }
            switch message.type {
            case .enter:
                do { try input.acknowledge(message) }
                catch {
                    hopLog("enter ack failed: \(error)")
                    if let epoch = input.restore() {
                        send(.point(.leave, epoch: epoch, x: 0, y: 0, extra: [2]))
                    }
                    update()
                    return
                }
            case .requestLeave:
                guard input.mode == .remote, message.epoch == input.epoch else {
                    hopLog("ignored request-leave mode=\(input.mode) epoch=\(message.epoch)")
                    update()
                    return
                }
                let old = input.restore(y: message.payload.u16(2))
                if let old { send(.point(.leave, epoch: old, x: message.payload.u16(0), y: message.payload.u16(2), extra: [0])) }
            default: throw WireError.invalid("Unexpected client input type")
            }
            session.noteLegalFrame(); update()
        } catch {
            // Restore before even sending REJECT. No protocol error can retain cursor capture.
            restoreAndNotify(reason: 2)
            if message.type == .hello, !session.authenticated {
                if session.peer == nil, let hello = try? Hello(payload: message.payload) {
                    send(.hello(Hello(phase: .reject, direction: session.direction, width: 1, height: 1,
                                      id: session.identity, nonce: hello.nonce, name: "Hop Mac")))
                } else { send(session.reply(.reject)) }
            }
            if !session.authenticated { session.cancelPairing() }
            close(reason: 1, text: "Connection rejected: \(error)")
        }
    }
    /// The one controller recovery entry. Local synchronous restoration precedes ALL network/UI actions.
    private func restoreAndNotify(reason: UInt8) {
        if let epoch = input.restore() { send(.point(.leave, epoch: epoch, x: 0, y: 0, extra: [reason])) }
    }
    private func close(reason: UInt8?, text: String) {
        hopLog("close \(text)")
        if let id = connectionID {
            server.close(id: id, final: reason.map { Message(type: .bye, payload: [$0]) })
        }
        connectionID = nil; session.reset(); status = text; update()
    }
    private func disconnect(reason: UInt8, text: String) {
        restoreAndNotify(reason: 2); close(reason: reason, text: text)
    }
    private func disableForFault(_ text: String, byeReason: UInt8 = 3, drain: Bool = true) {
        restoreAndNotify(reason: 1); input.stop(drain: drain); enabled = false
        close(reason: byeReason, text: text)
    }
    private static func screenLayout() -> String {
        NSScreen.screens.map { screen in
            let frame = screen.frame
            return "\(Int(frame.origin.x)),\(Int(frame.origin.y)),\(Int(frame.width))x\(Int(frame.height))"
        }.joined(separator: "|")
    }
    private var sharingWanted: Bool {
        if UserDefaults.standard.object(forKey: "HopSharingEnabled") == nil { return true }
        return UserDefaults.standard.bool(forKey: "HopSharingEnabled")
    }
    func setEnabled(_ value: Bool) {
        if !value {
            restoreAndNotify(reason: 1); input.stop(); enabled = false; awaitingPermission = false
            UserDefaults.standard.set(false, forKey: "HopSharingEnabled")
            status = "Sharing disabled; local input restored"; update(); return
        }
        do {
            try input.start(); enabled = true; awaitingPermission = false
            UserDefaults.standard.set(true, forKey: "HopSharingEnabled")
            status = session.authenticated ? "Connected — sharing enabled" : "Sharing enabled — waiting for paired device"
        } catch {
            input.restore(); enabled = false; awaitingPermission = true; status = String(describing: error)
            PermissionDock.shared.show()
        }
        update()
    }
    func requestPermissions() { InputCapture.requestPermission(); PermissionDock.shared.show(); update() }
    func setDirection(_ direction: UInt8) {
        guard direction != session.direction else { return }
        restoreAndNotify(reason: 1)
        close(reason: 0, text: "Direction changed — paired device may reconnect")
        session.direction = direction; input.direction = direction
        UserDefaults.standard.set(direction == 0 ? "left" : "right", forKey: "HopDirection"); update()
    }
    func beginPairing() {
        guard session.binding == nil else { return }
        session.pairingAllowed = true; pairingUntil = monotonicNow() + 60
        status = "Select this Mac on the other computer within 60 seconds"; update()
    }
    func confirmPairing() {
        do { try session.confirm() } catch { disconnect(reason: 1, text: String(describing: error)) }
    }
    func rejectPairing() {
        restoreAndNotify(reason: 1)
        if session.peer != nil { send(session.reply(.reject)) }
        session.cancelPairing()
        close(reason: 0, text: "Pairing cancelled")
    }
    func unpair() {
        restoreAndNotify(reason: 1)
        do {
            try SecureStore.delete()
            let wasAuthenticated = session.authenticated
            session.forget()
            close(reason: wasAuthenticated ? 4 : 0, text: "Unpaired — select Pair to create a new binding")
        } catch { close(reason: 0, text: "Could not remove Keychain binding: \(error)") }
    }
    private func tick() {
        // Preflight APIs stay false for a granted ad-hoc build. Do not treat that as revocation;
        // the event tap callback reports a real loss. Re-enable only while the first grant is pending.
        if !enabled, awaitingPermission, InputCapture.permissionGranted { setEnabled(true); return }
        if pairingUntil != 0, monotonicNow() >= pairingUntil {
            session.pairingAllowed = false; pairingUntil = 0
            if session.challenge == nil { update() }
        }
        guard connectionID != nil else { return }
        do { try session.checkDeadline() } catch { disconnect(reason: 2, text: String(describing: error)); return }
        if monotonicNow() - heartbeatAt >= 1 {
            heartbeatAt = monotonicNow(); var p = [UInt8](repeating: 0, count: 8)
            p.put64(0, UInt64(monotonicNow() * 1000)); send(Message(type: .keepalive, payload: p))
        }
    }
    func quit() {
        restoreAndNotify(reason: 1); input.stop(drain: false); enabled = false
        close(reason: 0, text: "Quitting")
        tickTimer?.invalidate(); moveTimer?.invalidate()
        for observer in observers { NSWorkspace.shared.notificationCenter.removeObserver(observer); NotificationCenter.default.removeObserver(observer) }
        server.stop()
    }
    private func update() {
        PermissionDock.shared.closeIfGranted()
        changed()
    }
    var stateText: String {
        if input.mode == .remote { return "Controlling remote computer" }
        if input.mode == .pending { return "Waiting for remote permission / ENTER acknowledgement" }
        if session.authenticated {
            if enabled { return "Connected — local input" }
            if !InputCapture.permissionGranted,
               status.contains("permission") || status.contains("Grant") || status.contains("Cannot") {
                return status
            }
            return "Connected — sharing disabled"
        }
        return status
    }
}
