import AppKit
import ApplicationServices
import IOKit

/// Injectable only for deterministic fail-safe tests; defaults are the native cursor APIs.
struct CursorOperations {
    var associate: (Bool) -> CGError = { CGAssociateMouseAndMouseCursorPosition($0 ? 1 : 0) }
    var hide: (CGDirectDisplayID) -> CGError = { CGDisplayHideCursor($0) }
    var show: (CGDirectDisplayID) -> CGError = { CGDisplayShowCursor($0) }
    var warp: (CGPoint) -> CGError = { CGWarpMouseCursorPosition($0) }
}

final class InputCapture {
    enum Mode { case local, pending, remote }
    private(set) var mode: Mode = .local
    private var tap: CFMachPort?
    private var source: CFRunLoopSource?
    private var hidden = false
    private var disassociated = false
    private var appKitHidden = false
    private var freezing = false
    private var extraHiddenDisplays: [CGDirectDisplayID] = []
    private var returnPoint: CGPoint?
    private(set) var epoch: UInt32 = 0
    private var enterMessage: Message?
    private var enteredAt: TimeInterval = 0
    private var virtualX = 0.0, virtualY = 0.0
    private var remoteWidth = 1.0, remoteHeight = 1.0
    private var moves = MoveCoalescer()
    private var buttons: UInt16 = 0
    private var modifiers: UInt16 = 0
    private var heldKeys = Set<UInt16>()
    private var ownership = InputOwnership()
    private var stopWhenDrained = false
    private var tapUsable = false
    private(set) var generation: UInt64 = 0
    private var cooldownUntil: TimeInterval = 0
    private var insideSince: TimeInterval = 0
    var direction: UInt8 = 1
    var mayEnter: () -> Bool = { false }
    var peerSize: () -> (UInt32, UInt32) = { (1, 1) }
    var send: (Message) -> Void = { _ in }
    var fault: (String, UInt8) -> Void = { _, _ in }
    var changed: () -> Void = {}
    private let supportedCodes: Set<UInt16>
    private let modifierMasks: [UInt16: UInt16]
    private let escapeCode: UInt16?
    private let functionCode: UInt16?
    private let cursor: CursorOperations
    init(keymap: Keymap, cursor: CursorOperations = CursorOperations()) {
        supportedCodes = keymap.supported; modifierMasks = keymap.modifiers
        // Include unsupported Quartz virtual keycodes when postponing crossing for held keys.
        escapeCode = keymap.code(symbol: "kVK_Escape"); functionCode = keymap.code(symbol: "kVK_Function"); self.cursor = cursor
    }
    var isRunning: Bool { tap != nil }
    /// Settings can show both switches on while the older preflight calls stay false
    /// for an ad-hoc build. Creating the tap is the check that matches what sharing needs.
    private static var tapAccepted = false
    static var permissionGranted: Bool {
        // A live sharing tap is the permission. Probing another tap while one
        // is installed can fail and pop the floater even though sharing works.
        if tapAccepted { return true }
        if AXIsProcessTrusted(), listenEventAllowed { return true }
        return probeActiveTap()
    }
    static func noteTapCreated() { tapAccepted = true }
    static func noteTapLost() { tapAccepted = false }
    static var listenEventAllowed: Bool {
        CGPreflightListenEventAccess() || IOHIDCheckAccess(kIOHIDRequestTypeListenEvent) == kIOHIDAccessTypeGranted
    }
    static var permissionReport: String {
        let ax = AXIsProcessTrusted()
        let listen = listenEventAllowed
        let tap = probeActiveTap()
        return "设备控制 \(ax ? "已允许" : "未允许")，输入监控 \(listen ? "已允许" : "未允许")，监听 \(tap ? "可以创建" : "创建失败")"
    }
    private static var probedAt = Date.distantPast
    private static var probed = false
    private static func probeActiveTap() -> Bool {
        let now = Date()
        if now.timeIntervalSince(probedAt) < 1 { return probed }
        probedAt = now
        let mask = CGEventMask(1) << CGEventType.mouseMoved.rawValue
        guard let tap = CGEvent.tapCreate(tap: .cghidEventTap, place: .headInsertEventTap, options: .defaultTap,
                                          eventsOfInterest: mask, callback: { _, _, event, _ in
            Unmanaged.passUnretained(event)
        }, userInfo: nil) else {
            probed = false
            return false
        }
        CFMachPortInvalidate(tap)
        probed = true
        return true
    }
    static func requestPermission() {
        let options = [kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String: true] as CFDictionary
        _ = AXIsProcessTrustedWithOptions(options)
        _ = CGRequestListenEventAccess()
    }
    func start() throws {
        guard Self.permissionGranted else { throw WireError.invalid("Grant Accessibility and Input Monitoring permissions, then enable Hop") }
        generation &+= 1; stopWhenDrained = false
        if tap != nil, tapUsable { return } // re-enable a still-draining tap without losing ownership
        removeTap()
        ownership.reconcile(
            heldKeys: Set(ownership.localKeys.union(ownership.remoteKeys).filter { CGEventSource.keyState(.hidSystemState, key: $0) }),
            heldButtons: Set(ownership.localButtons.union(ownership.remoteButtons).filter { $0 >= 0 && CGEventSource.buttonState(.hidSystemState, button: CGMouseButton(rawValue: UInt32($0))!) }))
        let types: [CGEventType] = [.mouseMoved, .leftMouseDragged, .rightMouseDragged, .otherMouseDragged,
                                   .leftMouseDown, .leftMouseUp, .rightMouseDown, .rightMouseUp, .otherMouseDown,
                                   .otherMouseUp, .scrollWheel, .keyDown, .keyUp, .flagsChanged]
        let mask = types.reduce(CGEventMask(0)) { $0 | (CGEventMask(1) << $1.rawValue) }
        guard let created = CGEvent.tapCreate(tap: .cghidEventTap, place: .headInsertEventTap, options: .defaultTap,
                                             eventsOfInterest: mask, callback: { _, type, event, context in
            guard let context else { return Unmanaged.passUnretained(event) }
            let owner = Unmanaged<InputCapture>.fromOpaque(context).takeUnretainedValue()
            if type == .tapDisabledByTimeout {
                // Main-thread UI work can delay this callback. Re-enable; do not drop the session.
                if let tap = owner.tap { CGEvent.tapEnable(tap: tap, enable: true) }
                owner.tapUsable = true
                hopLog("event tap timeout — re-enabled")
                return Unmanaged.passUnretained(event)
            }
            if type == .tapDisabledByUserInput {
                owner.tapUsable = false // a disabled/lost tap cannot enforce quarantine
                InputCapture.noteTapLost()
                owner.restoreAndReportFault("Global event tap was disabled; re-enable Hop after checking permissions", reason: 3)
                return Unmanaged.passUnretained(event)
            }
            return owner.handle(type, event: event) ? nil : Unmanaged.passUnretained(event)
        }, userInfo: Unmanaged.passUnretained(self).toOpaque()) else {
            throw WireError.invalid("Cannot create active CGEventTap; check permissions")
        }
        tap = created; tapUsable = true
        InputCapture.noteTapCreated()
        source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, created, 0)
        CFRunLoopAddSource(CFRunLoopGetMain(), source, .commonModes)
        CGEvent.tapEnable(tap: created, enable: true)
    }
    func stop(drain: Bool = true) {
        restore(); generation &+= 1
        // Disable keeps ONLY remote-owned tails quarantined; unrelated fresh local input passes.
        // Quit / permission loss cannot retain a working tap and cannot guarantee tail suppression.
        stopWhenDrained = drain && tapUsable && ownership.needsDrain
        if !stopWhenDrained { removeTap() }
    }
    private func removeTap() {
        if let tap { CGEvent.tapEnable(tap: tap, enable: false); CFMachPortInvalidate(tap) }
        if let source { CFRunLoopRemoveSource(CFRunLoopGetMain(), source, .commonModes) }
        source = nil; tap = nil; tapUsable = false; stopWhenDrained = false
        InputCapture.noteTapLost()
    }
    private func removeDrainedTapLater() {
        guard stopWhenDrained, !ownership.needsDrain else { return }
        let expected = generation
        DispatchQueue.main.async { [weak self] in
            guard let self, self.generation == expected, self.stopWhenDrained, !self.ownership.needsDrain else { return }
            self.removeTap() // never invalidate the active tap inside its callback
        }
    }
    private func restoreAndReportFault(_ text: String, reason: UInt8) {
        restore() // synchronous FIRST; controller defers all stop/network/UI work
        fault(text, reason)
    }
    /// Single synchronous, idempotent fail-safe. It does no networking and never synthesizes keys/clicks.
    @discardableResult func restore(y: UInt16? = nil) -> UInt32? {
        let previousEpoch = mode == .local ? nil : epoch
        let wasCaptured = hidden || disassociated
        mode = .local // stop remote capture FIRST; physical ownership/quarantine deliberately survives
        moves.clear(); enterMessage = nil; heldKeys.removeAll(); buttons = 0; modifiers = 0
        if disassociated, cursor.associate(true) == .success { disassociated = false }
        if hidden, cursor.show(CGMainDisplayID()) == .success { hidden = false }
        for id in extraHiddenDisplays { _ = cursor.show(id) }
        extraHiddenDisplays.removeAll()
        if appKitHidden { NSCursor.unhide(); appKitHidden = false }
        if wasCaptured, var point = returnPoint {
            if let y { let bounds = CGDisplayBounds(CGMainDisplayID()); point.y = bounds.minY + pixel(y, dimension: bounds.height) }
            _ = cursor.warp(point)
        }
        if !hidden && !disassociated { returnPoint = nil }
        cooldownUntil = monotonicNow() + 0.5
        insideSince = 0
        return previousEpoch
    }
    func acknowledge(_ message: Message) throws {
        guard mode == .pending, let expected = enterMessage else {
            throw WireError.invalid("ENTER confirmation while not waiting")
        }
        guard message == expected else { throw WireError.invalid("ENTER confirmation does not match") }
        guard monotonicNow() - enteredAt < 1.0 else { throw WireError.invalid("ENTER confirmation too late") }
        guard currentModifiers() == modifiers, currentButtons() == buttons, noNonmodifierKeysPressed() else {
            throw WireError.invalid("Keys changed during ENTER confirmation")
        }
        guard let position = CGEvent(source: nil)?.location, atEdge(position) else {
            throw WireError.invalid("Pointer left the edge before ENTER confirmation")
        }
        // No hiding/disassociation until the peer has positively acknowledged successful injection.
        try hideLocalCursor()
        disassociated = true // conservatively restore association even if the native call reports failure
        guard cursor.associate(false) == .success else {
            restore(); throw WireError.invalid("Cannot disassociate cursor")
        }
        mode = .remote; enterMessage = nil
        // macOS still applies the mouse delta to the local cursor before the event tap.
        // Pin it at the crossing point so only the other computer's pointer moves.
        freezeLocalCursor()
        changed()
    }
    func tick() {
        if mode == .remote { freezeLocalCursor() }
        if mode == .local, hidden || disassociated || appKitHidden || !extraHiddenDisplays.isEmpty { restore() } // retry failed native restoration, never re-enable swallowing
        if mode == .pending, monotonicNow() - enteredAt >= 1.0 {
            hopLog("enter timeout")
            let old = restore(); if let old { send(.point(.leave, epoch: old, x: 0, y: 0, extra: [2])) }; changed()
        }
        // macOS often stops delivering move events before the cursor is reported on the exact edge pixel.
        if mode == .local, mayEnter(), monotonicNow() >= cooldownUntil, !stopWhenDrained, !ownership.needsDrain,
           let point = CGEvent(source: nil)?.location, atEdge(point) {
            begin(at: point)
        }
        flushMove()
    }
    func flushMove() { if mode == .remote, let message = moves.take() { send(message) } }
    /// Hide on every display. CGDisplayHideCursor is per-screen and refcounted; call it once per capture.
    private func hideLocalCursor() throws {
        guard cursor.hide(CGMainDisplayID()) == .success else { throw WireError.invalid("Cannot hide cursor") }
        hidden = true
        for id in Self.displayIDs() where id != CGMainDisplayID() && !extraHiddenDisplays.contains(id) {
            if cursor.hide(id) == .success { extraHiddenDisplays.append(id) }
        }
        if !appKitHidden { NSCursor.hide(); appKitHidden = true }
    }
    private static func displayIDs() -> [CGDirectDisplayID] {
        let ids = NSScreen.screens.compactMap { screen in
            (screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber).map { CGDirectDisplayID($0.uint32Value) }
        }
        return ids.isEmpty ? [CGMainDisplayID()] : ids
    }
    /// The window server moves the pointer before this tap runs. Put it back on every sample.
    private func freezeLocalCursor() {
        guard !freezing, mode == .remote, let point = returnPoint else { return }
        freezing = true
        _ = cursor.warp(point)
        if cursor.associate(false) == .success { disassociated = true }
        freezing = false
    }
    private func atEdge(_ point: CGPoint, margin: CGFloat = 6) -> Bool {
        let b = CGDisplayBounds(CGMainDisplayID())
        guard point.y >= b.minY, point.y < b.maxY else { return false }
        return direction == 1 ? point.x >= b.maxX - margin : point.x <= b.minX + margin
    }
    private func currentModifiers() -> UInt16 {
        var mask: UInt16 = 0
        for (code, bit) in modifierMasks where bit != 256 {
            if CGEventSource.keyState(.hidSystemState, key: CGKeyCode(code)) { mask |= bit }
        }
        if CGEventSource.flagsState(.hidSystemState).contains(.maskAlphaShift) { mask |= 256 }
        return mask
    }
    private func currentButtons() -> UInt16 {
        var mask: UInt16 = 0
        for i in 0..<5 { if CGEventSource.buttonState(.hidSystemState, button: CGMouseButton(rawValue: UInt32(i))!) { mask |= 1 << i } }
        return mask
    }
    private func noNonmodifierKeysPressed() -> Bool {
        // Only keys observed by this tap. CGEventSource.keyState reports keycode 0
        // as held on this Mac when A is not pressed, which blocked every crossing.
        ownership.localKeys.allSatisfy { modifierMasks[$0] != nil }
    }
    private func begin(_ event: CGEvent) { begin(at: event.location) }
    private func begin(at position: CGPoint) {
        let size = peerSize(); remoteWidth = Double(size.0); remoteHeight = Double(size.1)
        let bounds = CGDisplayBounds(CGMainDisplayID())
        virtualX = direction == 1 ? min(2, remoteWidth - 1) : max(0, remoteWidth - 3)
        virtualY = pixel(normalized(position.y - bounds.minY, dimension: bounds.height), dimension: remoteHeight)
        modifiers = currentModifiers(); buttons = currentButtons()
        // Already-held ordinary keys cannot be reconstructed by ENTER. Wait for release before crossing.
        guard noNonmodifierKeysPressed() else { return }
        let localModifiers = Set(modifierMasks.compactMap { code, bit in bit != 256 && modifiers & bit != 0 ? code : nil })
        let localButtons = Set((0..<32).filter { CGEventSource.buttonState(.hidSystemState, button: CGMouseButton(rawValue: UInt32($0))!) })
        ownership.seedLocal(keys: localModifiers, buttons: localButtons)
        epoch &+= 1; if epoch == 0 { epoch = 1 }
        var tail = [UInt8](repeating: 0, count: 4); tail.put16(0, modifiers); tail.put16(2, buttons)
        let message = Message.point(.enter, epoch: epoch, x: normalized(virtualX, dimension: remoteWidth),
                                    y: normalized(virtualY, dimension: remoteHeight), extra: tail)
        returnPoint = CGPoint(x: direction == 1 ? bounds.maxX - 8 : bounds.minX + 8, y: position.y)
        enterMessage = message; mode = .pending; enteredAt = monotonicNow(); insideSince = 0; send(message); changed()
        hopLog("enter sent x=\(position.x) y=\(position.y)")
    }
    #if HOP_TESTING
    // Test-only state staging: invokes NO capture, display mutation, or physical input query.
    func stageRecoveryTest(remote: Bool = true) {
        mode = remote ? .remote : .pending; epoch = 42
        hidden = remote; disassociated = remote; returnPoint = CGPoint(x: 10, y: 20)
        heldKeys = [0, 55]; buttons = 1; modifiers = 64
        ownership = InputOwnership()
        if remote {
            _ = ownership.key(0, down: true, remote: true)
            _ = ownership.key(55, down: true, remote: true)
            _ = ownership.key(255, down: true, remote: true) // unsupported still physically owned
            _ = ownership.button(0, down: true, remote: true)
        }
        moves.offer(.point(.move, epoch: 42, x: 100, y: 200))
        enterMessage = .point(.enter, epoch: 42, x: 100, y: 200)
    }
    var recoveryTestIsCleared: Bool { heldKeys.isEmpty && buttons == 0 && modifiers == 0 && moves.pending == nil && enterMessage == nil }
    var recoveryTestCursorPending: Bool { hidden || disassociated }
    var recoveryTestQuarantine: Bool { ownership.remoteKeys == [0, 55, 255] && ownership.remoteButtons == [0] }
    var recoveryTestNeedsDrain: Bool { ownership.needsDrain }
    var recoveryTestStopWhenDrained: Bool { stopWhenDrained }
    func stageUsableTapTest() { tapUsable = true } // policy staging ONLY; creates no native tap
    func recoveryTestKey(_ code: UInt16, down: Bool) -> Bool { ownership.key(code, down: down, remote: mode == .remote) }
    func recoveryTestButton(_ index: Int, down: Bool) -> Bool { ownership.button(index, down: down, remote: mode == .remote) }
    func recoveryTestDrag(_ index: Int) -> Bool { ownership.suppressLocalDrag(index) }
    func recoveryTestFault() { restoreAndReportFault("Test fault", reason: 3) }
#endif
    private func handle(_ type: CGEventType, event: CGEvent) -> Bool {
        if freezing { return true } // swallow the cursor-pin warp, do not add it to the remote pointer
        // CGHID events from hardware have no originating userspace PID. Never relay synthetic injection.
        guard event.getIntegerValueField(.eventSourceUnixProcessID) == 0 else { return false }
        let swallowed = handlePhysical(type, event: event)
        if !swallowed { clearQuarantinedModifierFlags(event) }
        removeDrainedTapLater()
        return swallowed
    }
    private func clearQuarantinedModifierFlags(_ event: CGEvent) {
        let groups = ownership.modifierGroupsToClear(modifierMasks)
        var flags = event.flags
        for (group, flag): (UInt16, CGEventFlags) in [(3, .maskShift), (12, .maskControl), (48, .maskAlternate), (192, .maskCommand)] {
            if groups & group != 0 { flags.remove(flag) }
        }
        if let functionCode, ownership.remoteKeys.contains(functionCode) { flags.remove(.maskSecondaryFn) }
        event.flags = flags
    }
    private func handlePhysical(_ type: CGEventType, event: CGEvent) -> Bool {
        let key = UInt16(clamping: event.getIntegerValueField(.keyboardEventKeycode))
        if type == .keyDown, key == escapeCode,
           event.flags.contains([.maskControl, .maskAlternate, .maskCommand]) {
            _ = ownership.key(key, down: true, remote: true) // swallow Esc AND its repeat/up tail
            restoreAndReportFault("Emergency stop — local input restored", reason: 0)
            return true // never forward the emergency trigger to the peer or a local app
        }
        let remote = mode == .remote
        var suppress = remote
        var physicalKeyDown: Bool?
        switch type {
        case .keyDown, .keyUp:
            physicalKeyDown = type == .keyDown
        case .flagsChanged:
            // Caps Lock is a latch, not a press/release stream. Do not quarantine it until toggled off.
            if modifierMasks[key] != 256 { physicalKeyDown = CGEventSource.keyState(.hidSystemState, key: key) }
        case .leftMouseDown, .leftMouseUp, .rightMouseDown, .rightMouseUp, .otherMouseDown, .otherMouseUp:
            let index = Int(event.getIntegerValueField(.mouseEventButtonNumber))
            let down = [.leftMouseDown, .rightMouseDown, .otherMouseDown].contains(type)
            suppress = ownership.button(index, down: down, remote: remote) // includes unsupported buttons
        case .leftMouseDragged, .rightMouseDragged, .otherMouseDragged:
            suppress = remote || ownership.suppressLocalDrag(Int(event.getIntegerValueField(.mouseEventButtonNumber)))
        default: break
        }
        if let down = physicalKeyDown { suppress = ownership.key(key, down: down, remote: remote) } // BEFORE supported-code filtering
        let isMove = [.mouseMoved, .leftMouseDragged, .rightMouseDragged, .otherMouseDragged].contains(type)
        if mode == .local {
            if isMove, !suppress, !stopWhenDrained, !ownership.needsDrain, mayEnter(), monotonicNow() >= cooldownUntil, atEdge(event.location) { begin(event) }
            return suppress
        }
        if mode == .pending {
            let cursor = CGEvent(source: nil)?.location ?? event.location
            // A 1px bounce used to cancel the crossing before the PC could acknowledge it.
            if atEdge(cursor, margin: 24) {
                insideSince = 0
            } else if insideSince == 0 {
                insideSince = monotonicNow()
            } else if monotonicNow() - insideSince >= 0.25 {
                hopLog("pending cancelled x=\(cursor.x) y=\(cursor.y)")
                let old = restore()
                if let old { send(.point(.leave, epoch: old, x: 0, y: 0, extra: [1])) }
                changed()
            }
            return suppress // waiting for ENTER ACK does not claim fresh local presses
        }
        if isMove {
            virtualX = max(0, min(remoteWidth - 1, virtualX + Double(event.getIntegerValueField(.mouseEventDeltaX))))
            virtualY = max(0, min(remoteHeight - 1, virtualY + Double(event.getIntegerValueField(.mouseEventDeltaY))))
            moves.offer(.point(.move, epoch: epoch, x: normalized(virtualX, dimension: remoteWidth), y: normalized(virtualY, dimension: remoteHeight)))
            freezeLocalCursor()
            return true
        }
        flushMove() // ordered before EVERY nonmove, including keys, buttons and wheel
        switch type {
        case .leftMouseDown, .leftMouseUp, .rightMouseDown, .rightMouseUp, .otherMouseDown, .otherMouseUp:
            let index = Int(event.getIntegerValueField(.mouseEventButtonNumber))
            guard (0..<5).contains(index) else { return suppress }
            let down = [.leftMouseDown, .rightMouseDown, .otherMouseDown].contains(type)
            if down { buttons |= 1 << index } else { buttons &= ~(1 << index) }
            var p = [UInt8](repeating: 0, count: 8); p[0] = UInt8(index + 1); p[1] = down ? 1 : 0
            p.put16(2, normalized(virtualX, dimension: remoteWidth)); p.put16(4, normalized(virtualY, dimension: remoteHeight)); p.put16(6, buttons)
            send(Message(type: .button, epoch: epoch, payload: p))
        case .scrollWheel:
            // Quartz's post-system-preference axis deltas are content-scroll signed; protocol is +right/+down.
            // No second natural-scrolling inversion: both mouse wheels and trackpads use the delivered sign.
            func axis(_ field: CGEventField) -> Int32 {
                let bounded = max(Int64(-1000), min(Int64(1000), event.getIntegerValueField(field)))
                return Int32(-bounded * 120) // clamp BEFORE multiplication; no callback overflow trap
            }
            var p = [UInt8](repeating: 0, count: 8)
            p.put32(0, UInt32(bitPattern: axis(.scrollWheelEventDeltaAxis2))); p.put32(4, UInt32(bitPattern: axis(.scrollWheelEventDeltaAxis1)))
            send(Message(type: .wheel, epoch: epoch, payload: p))
        case .keyDown, .keyUp, .flagsChanged:
            modifiers = currentModifiers()
            guard supportedCodes.contains(key) else { return suppress } // ownership includes unsupported physical presses
            var down = type == .keyDown
            if type == .flagsChanged {
                guard let bit = modifierMasks[key] else { return true }
                down = physicalKeyDown ?? (modifiers & bit != 0)
            }
            if down { heldKeys.insert(key) } else { heldKeys.remove(key) }
            var p = [UInt8](repeating: 0, count: 6); p.put16(0, key); p.put16(2, modifiers)
            p[4] = down ? 1 : 0; p[5] = down && type == .keyDown && event.getIntegerValueField(.keyboardEventAutorepeat) != 0 ? 1 : 0
            send(Message(type: .key, epoch: epoch, payload: p))
        default: break
        }
        return suppress // local-owned releases pass ALSO locally, after remote forwarding
    }
}

func hopLog(_ message: String) {
    let line = "\(Date()) \(message)\n"
    let url = URL(fileURLWithPath: "/tmp/hop-cross.log")
    guard let data = line.data(using: .utf8) else { return }
    if FileManager.default.fileExists(atPath: url.path), let handle = try? FileHandle(forWritingTo: url) {
        handle.seekToEndOfFile(); handle.write(data); try? handle.close()
    } else {
        try? data.write(to: url)
    }
}
