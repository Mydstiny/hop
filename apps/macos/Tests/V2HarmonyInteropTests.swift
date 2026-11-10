import Foundation
import Darwin
import CoreGraphics

private final class InteropInputDelegate: V2InputRouterDelegate {
    let host: V2SessionBridge
    var entryAccepted = false, acknowledged = false
    init(host: V2SessionBridge) { self.host = host }
    func requestV2Entry(screen: UInt64, x: UInt16, y: UInt16, captureEpoch: UInt32) { entryAccepted = host.enter(epoch: 43, screen: screen, x: x, y: y) }
    func submitV2Input(_ payloads: [[UInt8]], focusEpoch: UInt64) -> Bool {
        focusEpoch == 43 && payloads.allSatisfy { host.sendInput($0) }
    }
    func requestV2Release() { _ = host.release() }
    func acknowledgeV2Capture(_ message: Message) throws { acknowledged = true }
    func restoreV2Capture(y: UInt16?) {}
    func abortV2Session() { host.close() }
}

@main struct V2HarmonyInteropTests {
    static func main() throws {
        var checks = 0
        func check(_ value: Bool, _ message: String) { checks += 1; if !value { fputs("FAIL Mac/Harmony fake interop: \(message)\n",stderr); exit(1) } }
        let helper = CommandLine.arguments[1]
        let certA = CommandLine.arguments[2], keyA = CommandLine.arguments[3], certB = CommandLine.arguments[4], keyB = CommandLine.arguments[5]
        let hostIdentity = V2LocalIdentity(certificatePEM: try String(contentsOfFile: certA, encoding: .utf8), privateKeyPEM: try String(contentsOfFile: keyA, encoding: .utf8))
        let remotePEM = try String(contentsOfFile: certB, encoding: .utf8)
        let remoteFingerprint = V2Certificate.fingerprint(remotePEM)!
        let lock = NSLock(), selected = DispatchSemaphore(value: 0)
        let hostOffer = V2PairOffer(role: 1, nonce: Array(repeating: 7, count: 16), name: "Mac test", certificatePEM: hostIdentity.certificatePEM)
        var reader: V2SelectorReader?, offerJob: V2OfferExchange?, candidate: V2PairOffer?, result: V2SelectorReader.Selection?
        let listener = V2IncomingListener { fd, kind, prefix in
            if kind == .offer {
                guard let job = V2OfferExchange(fd: fd, prefix: prefix, local: hostOffer, completion: { value in
                    if case .success(let peer) = value { lock.lock(); candidate = peer; lock.unlock() }
                }) else { return }
                lock.lock(); offerJob = job; lock.unlock(); job.start(); return
            }
            guard let job = V2SelectorReader(fd: fd, prefix: prefix, completion: { value in
                if case .success(let selection) = value { lock.lock(); result = selection; lock.unlock() }
                selected.signal()
            }) else { selected.signal(); return }
            lock.lock(); reader = job; lock.unlock(); job.start()
        }
        let port = try listener.start(port: 0); listener.openPairingWindow()
        let child = Process(); child.executableURL = URL(fileURLWithPath: helper)
        child.arguments = ["127.0.0.1", String(port), certB, keyB, certA]
        let output = Pipe(), childInput = Pipe(); child.standardOutput = output; child.standardError = output; child.standardInput = childInput
        try child.run()
        check(selected.wait(timeout: .now() + .seconds(5)) == .success, "Harmony receiver reaches Mac listener")
        for _ in 0..<1000 {
            lock.lock(); let available = candidate != nil; lock.unlock()
            if available { break }; Thread.sleep(forTimeInterval: 0.001)
        }
        lock.lock(); let chosen = result, heldReader = reader, heldOffer = offerJob, proposed = candidate; lock.unlock()
        guard let chosen else { throw WireError.invalid("selector unavailable") }
        check(heldReader != nil && heldOffer != nil && proposed?.certificatePEM == remotePEM && chosen.fingerprint == remoteFingerprint,
              "public candidate and selector match compared receiver")
        let tls = try V2TLSSocket.adopt(fd: chosen.fd, client: false, local: hostIdentity, verifiedPeerPEM: remotePEM, verifiedFingerprint: remoteFingerprint)
        let deadline = Date().addingTimeInterval(5)
        while Date() < deadline && !tls.isAuthenticated { _ = try tls.handshake(); Thread.sleep(forTimeInterval: 0.001) }
        check(tls.isAuthenticated, "Mac and Harmony receiver authenticate pinned TLS")
        var restored = 0
        var desktop: V2DesktopLayout?
        var leave: V2DesktopLayout.Route?
        let remoteKey = V2DesktopLayout.Key(device: remoteFingerprint, screen: 7)
        let host = try V2SessionBridge.start(transport: tls, role: .host, platform: 1, sessionEpoch: 42, localScreens: [], now: 0, requirePairing: true,
                                             restore: { restored += 1; return true }, leave: { screen, x, y in
            guard screen == remoteKey.screen else { return false }
            leave = desktop?.remoteEdge(remoteKey, x: x, y: y)
            return true
        })
        var now: UInt64 = 0
        func pumpUntil(_ predicate: () -> Bool) throws {
            for _ in 0..<3000 {
                now += 1; try host.step(now: now)
                if predicate() { return }; Thread.sleep(forTimeInterval: 0.001)
            }
            throw WireError.invalid("Harmony fake interop timed out")
        }
        try pumpUntil { host.phase == 2 }
        check(host.confirmPairing(), "explicit host confirmation after candidate comparison")
        try pumpUntil { host.pairingConfirmed && (try? host.screens().count) == 1 }
        let screen = try host.screens()
        check(screen[0].id == 7 && screen[0].width == 1001 && screen[0].height == 501, "Harmony screen advertised over TLS")
        let keymap = try HIDKeymap(keymap: Keymap.load(url: URL(fileURLWithPath: CommandLine.arguments[6])))
        let inputDelegate = InteropInputDelegate(host: host), router = V2InputRouter(keymap: keymap, delegate: inputDelegate)
        router.selectedScreen = 7
        router.captured(.point(.enter, epoch: 1, x: 32768, y: 32768, extra: [0,0,0,0]))
        check(inputDelegate.entryAccepted && !router.isActive, "Mac requests Harmony focus without early capture")
        try pumpUntil { host.phase == 6 }
        router.focused(epoch: 43, screen: 7, x: 32768, y: 32768)
        check(router.isActive && inputDelegate.acknowledged, "remote acknowledgement activates Mac routing")
        router.captured(Message(type: .key, epoch: 1, payload: [0,0,0,0,1,0]))
        router.captured(Message(type: .key, epoch: 1, payload: [0,0,0,0,0,0]))
        router.captured(Message(type: .wheel, epoch: 1, payload: [0,0,0,0,0,0,0,120]))
        check(router.isActive, "Mac routes captured key and wheel")
        for _ in 0..<150 { now += 1; try host.step(now: now); Thread.sleep(forTimeInterval: 0.001) }
        check(host.release() && restored == 1, "restore host before release acknowledgement")
        try pumpUntil { host.phase == 4 }

        let macKey = V2DesktopLayout.Key(device: "local", screen: 1)
        let macRect = CGRect(x: 0, y: 0, width: 2560, height: 1440)
        let harmonyRect = CGRect(x: 0, y: 0, width: 1001, height: 501)
        let layout = try V2DesktopLayout.arrange(
            [.init(key: macKey, label: "Mac", frame: macRect, physical: macRect)],
            [[.init(key: remoteKey, label: "Harmony", frame: harmonyRect, physical: harmonyRect)]],
            placements: [:])
        func move(x: UInt16, y: UInt16) -> Bool {
            var payload = [UInt8](repeating: 0, count: 16)
            payload[0] = 1; payload[1] = 1
            payload.put64(4, remoteKey.screen); payload.put16(12, x); payload.put16(14, y)
            return host.sendInput(payload)
        }
        for (index, side) in V2LayoutEdge.allCases.enumerated() {
            guard let arranged = layout.placing([remoteKey], by: macKey, on: side),
                  let route = arranged.crossing(from: macKey, edge: side, along: 0.55) else {
                throw WireError.invalid("Could not stage \(side) workspace")
            }
            desktop = arranged; leave = nil
            check(host.enter(epoch: 44 + UInt64(index), screen: route.target.screen, x: route.x, y: route.y),
                  "enter Harmony from \(side)")
            try pumpUntil { host.phase == 6 }
            let x = side == .left ? UInt16.max : side == .right ? 0 : route.x
            let y = side == .top ? UInt16.max : side == .bottom ? 0 : route.y
            check(move(x: x, y: y), "send pointer back through Harmony's \(side.opposite) edge")
            try pumpUntil { leave != nil }
            check(leave?.target == macKey && leave?.entryEdge == side && macRect.contains(leave!.localEntryPoint),
                  "authenticated return matches the same saved Mac seam")
            check(host.release(), "release the returned focus")
            try pumpUntil { host.phase == 4 }
        }
        check(restored == 5, "all four round trips restore local input and leave the TLS session open")
        try childInput.fileHandleForWriting.write(contentsOf: Data([1])); try childInput.fileHandleForWriting.close()
        let childDeadline = Date().addingTimeInterval(3)
        while child.isRunning && Date() < childDeadline { Thread.sleep(forTimeInterval: 0.01) }
        if child.isRunning { child.terminate() }
        child.waitUntilExit(); let helperOutput = String(data: output.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
        check(child.terminationStatus == 0 && helperOutput.contains("PASS Harmony receiver interop fake SDK"), "Harmony fake SDK observes key, wheel and release")
        host.close(); listener.stop()
        print("PASS \(checks) Mac host to Harmony receiver TLS/session loopback checks; fake SDK, no device input")
    }
}
