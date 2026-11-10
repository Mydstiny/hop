import AppKit
import ApplicationServices
import IOKit

protocol PointerSettingsStore {
    func data(forKey key: String) -> Data?
    func set(_ value: Any?, forKey key: String)
}
extension UserDefaults: PointerSettingsStore {}

/// The current v2 session accepts only a neutral input snapshot at ENTER.
/// Refuse crossing instead of dropping held state or inventing releases.
func permitsFocusEntry(modifiers: UInt16, buttons: UInt16, nonmodifierKeysUp: Bool, requireNeutralState: Bool) -> Bool {
    nonmodifierKeysUp && (!requireNeutralState || (modifiers == 0 && buttons == 0))
}

/// Derive the remote modifier snapshot from the actual CGEvent, rather than
/// querying global state after the event has been intercepted. Side-specific
/// bits are defined by IOKit/hidsystem/IOLLEvent.h. Some devices report only
/// aggregate flags, so their flagsChanged event toggles the identified side.
func eventModifiers(previous: UInt16, flags: CGEventFlags, changedBit: UInt16?) -> UInt16 {
    let raw = flags.rawValue
    var result = previous
    let groups: [(UInt16, UInt64, UInt64, UInt16, UInt64, UInt16)] = [
        (3, CGEventFlags.maskShift.rawValue, 0x0002, 1, 0x0004, 2),
        (12, CGEventFlags.maskControl.rawValue, 0x0001, 4, 0x2000, 8),
        (48, CGEventFlags.maskAlternate.rawValue, 0x0020, 16, 0x0040, 32),
        (192, CGEventFlags.maskCommand.rawValue, 0x0008, 64, 0x0010, 128)
    ]
    for (group, aggregate, leftNative, leftWire, rightNative, rightWire) in groups {
        if raw & aggregate == 0 { result &= ~group; continue }
        let sides = raw & (leftNative | rightNative)
        if sides != 0 {
            result &= ~group
            if sides & leftNative != 0 { result |= leftWire }
            if sides & rightNative != 0 { result |= rightWire }
        } else if let bit = changedBit, bit & group != 0 {
            result ^= bit
        } else if result & group == 0 {
            result |= leftWire
        }
    }
    if flags.contains(.maskAlphaShift) { result |= 256 } else { result &= ~UInt16(256) }
    return result
}

/// Per-peer pointer preferences. Defaults preserve the existing behavior.
struct MouseTuning: Codable, Equatable {
    var movement = 1.0
    var horizontal = 1.0
    var vertical = 1.0
    var reverseHorizontal = false
    var reverseVertical = false
    var reportRateHz = 240

    init(movement: Double = 1, horizontal: Double = 1, vertical: Double = 1,
         reverseHorizontal: Bool = false, reverseVertical: Bool = false, reportRateHz: Int = 240) {
        self.movement = movement; self.horizontal = horizontal; self.vertical = vertical
        self.reverseHorizontal = reverseHorizontal; self.reverseVertical = reverseVertical
        self.reportRateHz = reportRateHz
    }
    private enum CodingKeys: String, CodingKey { case movement, horizontal, vertical, reverseHorizontal, reverseVertical, reportRateHz }
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        movement = try c.decode(Double.self, forKey: .movement)
        horizontal = try c.decode(Double.self, forKey: .horizontal)
        vertical = try c.decode(Double.self, forKey: .vertical)
        reverseHorizontal = try c.decode(Bool.self, forKey: .reverseHorizontal)
        reverseVertical = try c.decode(Bool.self, forKey: .reverseVertical)
        reportRateHz = try c.decodeIfPresent(Int.self, forKey: .reportRateHz) ?? 240
    }

    var valid: Bool {
        [movement, horizontal, vertical].allSatisfy { $0.isFinite && (0.25...4.0).contains($0) } &&
            [60, 120, 240, 500, 1000].contains(reportRateHz)
    }
    static func key(for peer: [UInt8]) -> String? {
        guard peer.count == 16, peer.contains(where: { $0 != 0 }) else { return nil }
        return "HopPointerSettings." + peer.map { String(format: "%02x", $0) }.joined()
    }
    static func load(peer: [UInt8], defaults: PointerSettingsStore = UserDefaults.standard) -> MouseTuning {
        guard let key = key(for: peer), let data = defaults.data(forKey: key),
              let value = try? JSONDecoder().decode(MouseTuning.self, from: data), value.valid else { return MouseTuning() }
        return value
    }
    @discardableResult func save(peer: [UInt8], defaults: PointerSettingsStore = UserDefaults.standard) -> Bool {
        guard valid, let key = Self.key(for: peer), let data = try? JSONEncoder().encode(self) else { return false }
        defaults.set(data, forKey: key)
        return true
    }
    // V2 settings use the verified certificate fingerprint, never a discovery name.
    // Keep the key namespace separate from legacy device IDs.
    static func key(fingerprint: String) -> String? {
        guard fingerprint.utf8.count == 64,
              fingerprint.utf8.allSatisfy({ (48...57).contains($0) || (65...70).contains($0) }) else { return nil }
        return "HopPointerSettings.v2." + fingerprint
    }
    static func load(fingerprint: String, defaults: PointerSettingsStore = UserDefaults.standard) -> MouseTuning {
        guard let key = key(fingerprint: fingerprint), let data = defaults.data(forKey: key),
              let value = try? JSONDecoder().decode(MouseTuning.self, from: data), value.valid else { return MouseTuning() }
        return value
    }
    @discardableResult func save(fingerprint: String, defaults: PointerSettingsStore = UserDefaults.standard) -> Bool {
        guard valid, let key = Self.key(fingerprint: fingerprint), let data = try? JSONEncoder().encode(self) else { return false }
        defaults.set(data, forKey: key)
        return true
    }
    func wheel(_ raw: Int64, horizontalAxis: Bool) -> Int32 {
        // Clamp before multiplication and again to the v1 protocol limit.
        let safe = max(Int64(-1000), min(Int64(1000), raw))
        let scale = horizontalAxis ? horizontal : vertical
        let reverse = horizontalAxis ? reverseHorizontal : reverseVertical
        let value = -Double(safe) * 120 * scale * (reverse ? -1 : 1)
        return Int32(max(-120000, min(120000, value.rounded())))
    }
}

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
    private var wheelDiagnostics = Set<UInt8>()
    var ignoredEventSourceUserData: Int64?
    private let receiveSafetyLock = NSLock()
    private var receiveSafetyArmed = false
    private var receiveSafetyNeutral = false

    /// Published by the main event tap and read by the receiver worker. Quartz
    /// can report virtual key 0 as down spuriously, so use observed press state.
    var receiverPhysicalInputIsNeutral: Bool { receiveSafetyLock.withLock { receiveSafetyNeutral } }
    func armReceiverSafety() {
        receiveSafetyArmed = true
        publishReceiverSafety()
    }
    private func publishReceiverSafety() {
        let neutral = receiveSafetyArmed && tapUsable && !stopWhenDrained && !ownership.needsDrain &&
            ownership.localKeys.isEmpty && ownership.localButtons.isEmpty
        receiveSafetyLock.withLock { receiveSafetyNeutral = neutral }
    }
    var direction: UInt8 = 1
    struct RoutedEntry {
        let width, height: UInt32
        let x, y: UInt16
        let returnPoint: CGPoint
        let stillNear: (CGPoint, CGFloat) -> Bool
    }
    var routedEdge: ((CGPoint, CGFloat) -> Bool)?
    var routedEntry: ((CGPoint, CGVector) -> RoutedEntry?)?
    private var pendingLocation: ((CGPoint, CGFloat) -> Bool)?
    private(set) var physicalPressSequence: UInt64 = 0
    var routingAnchor: CGPoint? { returnPoint }
    // V1 supports an initial modifier/button snapshot. Enable this for the
    // v2 host until its ENTER state synchronization is fully implemented.
    var requireNeutralEntry = false
    var tuning = MouseTuning() {
        didSet { if !tuning.valid { tuning = oldValue } }
    }
    var mayEnter: () -> Bool = { false }
    var peerSize: () -> (UInt32, UInt32) = { (1, 1) }
    var send: (Message) -> Void = { _ in }
    var fault: (String, UInt8) -> Void = { _, _ in }
    var changed: () -> Void = {}
    var movementAvailable: () -> Void = {}
    var hasPendingMove: Bool { moves.pending != nil }
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
        if Locale.preferredLanguages.first?.lowercased().hasPrefix("zh") == true {
            return "设备控制 \(ax ? "已允许" : "未允许")，输入监控 \(listen ? "已允许" : "未允许")，监听 \(tap ? "可以创建" : "创建失败")"
        }
        return "Accessibility \(ax ? "allowed" : "not allowed"), Input Monitoring \(listen ? "allowed" : "not allowed"), event tap \(tap ? "available" : "unavailable")"
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
        receiveSafetyArmed = false; publishReceiverSafety()
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
    var localCursorRestored: Bool {
        mode == .local && !hidden && !disassociated && !appKitHidden && extraHiddenDisplays.isEmpty
    }
    @discardableResult func restore(y: UInt16? = nil, point override: CGPoint? = nil) -> UInt32? {
        let previousEpoch = mode == .local ? nil : epoch
        let wasCaptured = hidden || disassociated
        mode = .local // stop remote capture FIRST; physical ownership/quarantine deliberately survives
        moves.clear(); enterMessage = nil; pendingLocation = nil; heldKeys.removeAll(); buttons = 0; modifiers = 0
        if disassociated, cursor.associate(true) == .success { disassociated = false }
        if hidden, cursor.show(CGMainDisplayID()) == .success { hidden = false }
        extraHiddenDisplays = extraHiddenDisplays.filter { cursor.show($0) != .success }
        if appKitHidden { NSCursor.unhide(); appKitHidden = false }
        if wasCaptured, var point = override ?? returnPoint {
            if override == nil, let y { let bounds = CGDisplayBounds(CGMainDisplayID()); point.y = bounds.minY + pixel(y, dimension: bounds.height) }
            _ = cursor.warp(point)
        }
        if localCursorRestored { returnPoint = nil }
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
        mode = .remote; enterMessage = nil; pendingLocation = nil
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
        if mode == .pending, let pendingLocation { return pendingLocation(point, margin) }
        if let routedEdge { return routedEdge(point, margin) }
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
    private func begin(_ event: CGEvent) {
        begin(at: event.location, movement: CGVector(dx: CGFloat(event.getIntegerValueField(.mouseEventDeltaX)),
                                                    dy: CGFloat(event.getIntegerValueField(.mouseEventDeltaY))))
    }
    func continueAt(_ plan: RoutedEntry) {
        guard let cursor = CGEvent(source: nil)?.location, plan.stillNear(cursor, 24), canContinueRouting else { return }
        begin(at: cursor, forced: plan)
    }
    var canContinueRouting: Bool {
        tapUsable && !stopWhenDrained && !ownership.needsDrain && ownership.localKeys.isEmpty && ownership.localButtons.isEmpty
    }
    var crossingKeysAreUp: Bool { heldKeys.isEmpty && buttons == 0 && modifiers == 0 }
    /// A LEAVE names the last displayed edge. Accept it only while the
    /// physical pointer is still near that edge and position on our host.
    func matchesRemoteEdge(x: UInt16, y: UInt16, horizontalBoundary: Bool? = nil) -> Bool {
        guard mode == .remote else { return false }
        let currentX = Int(normalized(virtualX, dimension: remoteWidth))
        let currentY = Int(normalized(virtualY, dimension: remoteHeight))
        let dx = abs(currentX - Int(x)), dy = abs(currentY - Int(y))
        let vertical = (x == 0 || x == .max) && dx <= 512 && dy <= 2048
        let horizontal = (y == 0 || y == .max) && dy <= 512 && dx <= 2048
        return horizontalBoundary.map { $0 ? horizontal : vertical } ?? (vertical || horizontal)
    }
    private func begin(at position: CGPoint, movement: CGVector = .zero, forced: RoutedEntry? = nil) {
        guard mode == .local else { return }
        modifiers = currentModifiers(); buttons = currentButtons()
        // Already-held ordinary keys cannot be reconstructed by ENTER. Wait for release before crossing.
        guard permitsFocusEntry(modifiers: modifiers, buttons: buttons, nonmodifierKeysUp: noNonmodifierKeysPressed(),
                                requireNeutralState: requireNeutralEntry) else { return }
        let planned = forced ?? routedEntry?(position, movement)
        if routedEntry != nil && planned == nil { return }
        let size = planned.map { ($0.width, $0.height) } ?? peerSize()
        remoteWidth = Double(size.0); remoteHeight = Double(size.1)
        let bounds = CGDisplayBounds(CGMainDisplayID())
        if let planned {
            virtualX = pixel(planned.x, dimension: remoteWidth)
            virtualY = pixel(planned.y, dimension: remoteHeight)
        } else {
            virtualX = direction == 1 ? min(2, remoteWidth - 1) : max(0, remoteWidth - 3)
            virtualY = pixel(normalized(position.y - bounds.minY, dimension: bounds.height), dimension: remoteHeight)
        }
        let localModifiers = Set(modifierMasks.compactMap { code, bit in bit != 256 && modifiers & bit != 0 ? code : nil })
        let localButtons = Set((0..<32).filter { CGEventSource.buttonState(.hidSystemState, button: CGMouseButton(rawValue: UInt32($0))!) })
        ownership.seedLocal(keys: localModifiers, buttons: localButtons)
        epoch &+= 1; if epoch == 0 { epoch = 1 }
        var tail = [UInt8](repeating: 0, count: 4); tail.put16(0, modifiers); tail.put16(2, buttons)
        let message = Message.point(.enter, epoch: epoch, x: planned?.x ?? normalized(virtualX, dimension: remoteWidth),
                                    y: planned?.y ?? normalized(virtualY, dimension: remoteHeight), extra: tail)
        returnPoint = planned?.returnPoint ?? CGPoint(x: direction == 1 ? bounds.maxX - 8 : bounds.minX + 8, y: position.y)
        pendingLocation = planned?.stillNear
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
    func stageForwardingMoveTest() {
        mode = .remote; epoch = 42; returnPoint = nil
        moves.offer(.point(.move, epoch: epoch, x: 123, y: 456))
    }
    var recoveryTestIsCleared: Bool { heldKeys.isEmpty && buttons == 0 && modifiers == 0 && moves.pending == nil && enterMessage == nil }
    var recoveryTestCursorPending: Bool { hidden || disassociated }
    var recoveryTestQuarantine: Bool { ownership.remoteKeys == [0, 55, 255] && ownership.remoteButtons == [0] }
    var recoveryTestNeedsDrain: Bool { ownership.needsDrain }
    var recoveryTestStopWhenDrained: Bool { stopWhenDrained }
    func stageUsableTapTest() { tapUsable = true } // policy staging ONLY; creates no native tap
    func stageRoutedPointerTest(x: Double, y: Double, width: Double, height: Double) {
        virtualX = x; virtualY = y; remoteWidth = width; remoteHeight = height
    }
    func recoveryTestKey(_ code: UInt16, down: Bool) -> Bool { ownership.key(code, down: down, remote: mode == .remote) }
    func recoveryTestButton(_ index: Int, down: Bool) -> Bool { ownership.button(index, down: down, remote: mode == .remote) }
    func recoveryTestDrag(_ index: Int) -> Bool { ownership.suppressLocalDrag(index) }
    func recoveryTestFault() { restoreAndReportFault("Test fault", reason: 3) }
#endif
    private func handle(_ type: CGEventType, event: CGEvent) -> Bool {
        if let marker = ignoredEventSourceUserData, event.getIntegerValueField(.eventSourceUserData) == marker { return false }
        if freezing { return true } // swallow the cursor-pin warp, do not add it to the remote pointer
        if mode == .remote && type == .scrollWheel {
            // Record only presence/category, never keys, text, pointer positions or wheel values.
            // Some mouse drivers synthesize scrolling even when movement is a HID event.
            let physical = event.getIntegerValueField(.eventSourceUnixProcessID) == 0
            let line = event.getIntegerValueField(.scrollWheelEventDeltaAxis1) != 0 || event.getIntegerValueField(.scrollWheelEventDeltaAxis2) != 0
            let fixed = event.getDoubleValueField(.scrollWheelEventFixedPtDeltaAxis1) != 0 || event.getDoubleValueField(.scrollWheelEventFixedPtDeltaAxis2) != 0
            let shape: UInt8 = (physical ? 1 : 0) | (line ? 2 : 0) | (fixed ? 4 : 0)
            if wheelDiagnostics.insert(shape).inserted { hopLog("wheel diagnostic physical=\(physical) line=\(line) fixed=\(fixed)") }
        }
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
            if receiveSafetyArmed { publishReceiverSafety() }
            restoreAndReportFault("Emergency stop — local input restored", reason: 0)
            return true // never forward the emergency trigger to the peer or a local app
        }
        let remote = mode == .remote
        let eventMask: UInt16? = remote && [.keyDown, .keyUp, .flagsChanged].contains(type)
            ? eventModifiers(previous: modifiers, flags: event.flags,
                             changedBit: type == .flagsChanged ? modifierMasks[key] : nil) : nil
        var suppress = remote
        var physicalKeyDown: Bool?
        switch type {
        case .keyDown, .keyUp:
            physicalKeyDown = type == .keyDown
        case .flagsChanged:
            // Caps Lock is a latch, not a press/release stream. Do not quarantine it until toggled off.
            if let bit = modifierMasks[key], bit != 256 {
                physicalKeyDown = eventMask.map { $0 & bit != 0 } ?? CGEventSource.keyState(.hidSystemState, key: key)
            }
        case .leftMouseDown, .leftMouseUp, .rightMouseDown, .rightMouseUp, .otherMouseDown, .otherMouseUp:
            let index = Int(event.getIntegerValueField(.mouseEventButtonNumber))
            let down = [.leftMouseDown, .rightMouseDown, .otherMouseDown].contains(type)
            suppress = ownership.button(index, down: down, remote: remote) // includes unsupported buttons
        case .leftMouseDragged, .rightMouseDragged, .otherMouseDragged:
            suppress = remote || ownership.suppressLocalDrag(Int(event.getIntegerValueField(.mouseEventButtonNumber)))
        default: break
        }
        if let down = physicalKeyDown { suppress = ownership.key(key, down: down, remote: remote) } // BEFORE supported-code filtering
        if physicalKeyDown != nil || [.leftMouseDown, .leftMouseUp, .rightMouseDown, .rightMouseUp, .otherMouseDown, .otherMouseUp].contains(type) {
            physicalPressSequence &+= 1
            if receiveSafetyArmed { publishReceiverSafety() }
        }
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
            virtualX = max(0, min(remoteWidth - 1, virtualX + Double(event.getIntegerValueField(.mouseEventDeltaX)) * tuning.movement))
            virtualY = max(0, min(remoteHeight - 1, virtualY + Double(event.getIntegerValueField(.mouseEventDeltaY)) * tuning.movement))
            moves.offer(.point(.move, epoch: epoch, x: normalized(virtualX, dimension: remoteWidth), y: normalized(virtualY, dimension: remoteHeight)))
            movementAvailable()
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
            var p = [UInt8](repeating: 0, count: 8)
            p.put32(0, UInt32(bitPattern: tuning.wheel(event.getIntegerValueField(.scrollWheelEventDeltaAxis2), horizontalAxis: true)))
            p.put32(4, UInt32(bitPattern: tuning.wheel(event.getIntegerValueField(.scrollWheelEventDeltaAxis1), horizontalAxis: false)))
            send(Message(type: .wheel, epoch: epoch, payload: p))
        case .keyDown, .keyUp, .flagsChanged:
            modifiers = eventMask ?? currentModifiers()
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
