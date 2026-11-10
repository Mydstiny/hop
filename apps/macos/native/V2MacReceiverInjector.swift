import Foundation
import CoreGraphics
import ColorSync
import CryptoKit
import IOKit.hidsystem

/// Implementations run on one receiver worker. Returning false retains any
/// possibly injected down transition for best-effort cleanup.
protocol V2MacInputBackend: AnyObject {
    var authorized: Bool { get }
    func canBegin() -> Bool
    func move(to: CGPoint, dragging: UInt8?, flags: CGEventFlags) -> Bool
    func button(_ button: UInt8, down: Bool, at: CGPoint, clicks: Int, flags: CGEventFlags) -> Bool
    func wheel(horizontal: Int32, vertical: Int32, at: CGPoint, flags: CGEventFlags) -> Bool
    func key(_ keyCode: UInt16, down: Bool, repeated: Bool, modifier: Bool, flags: CGEventFlags) -> Bool
}

/// Receiver hooks for an already authenticated V2SessionBridge. The owner
/// serializes all calls and runs restore on permission, screen or link loss.
final class V2MacReceiverInjector {
    struct Display: Equatable {
        let id: UInt64
        let bounds: CGRect
        let nativeID: CGDirectDisplayID
        init(id: UInt64, bounds: CGRect, nativeID: CGDirectDisplayID = 0) {
            self.id = id; self.bounds = bounds; self.nativeID = nativeID
        }
    }
    struct Leave: Equatable {
        let screen: UInt64
        let x, y: UInt16
    }
    private struct Click {
        let point: CGPoint
        let at: TimeInterval
        let count: Int
    }
    private static let modifierFlagsByUsage: [(CGEventFlags, UInt64)] = [
        (.maskControl, UInt64(NX_DEVICELCTLKEYMASK)), (.maskShift, UInt64(NX_DEVICELSHIFTKEYMASK)),
        (.maskAlternate, UInt64(NX_DEVICELALTKEYMASK)), (.maskCommand, UInt64(NX_DEVICELCMDKEYMASK)),
        (.maskControl, UInt64(NX_DEVICERCTLKEYMASK)), (.maskShift, UInt64(NX_DEVICERSHIFTKEYMASK)),
        (.maskAlternate, UInt64(NX_DEVICERALTKEYMASK)), (.maskCommand, UInt64(NX_DEVICERCMDKEYMASK))
    ]

    let screenIDs: [UInt64]
    let screenAdvertisement: [UInt8]
    private let displays: [UInt64: CGRect]
    private let keymap: HIDKeymap
    private let backend: V2MacInputBackend
    private let now: () -> TimeInterval
    private let doubleClickInterval: TimeInterval
    private var focus: (epoch: UInt64, display: Display)?
    private var position = CGPoint.zero
    private var normalized: (x: UInt16, y: UInt16) = (0, 0)
    private var leave: Leave?
    private var verticalEdge = false
    private var horizontalEdge = false
    private var keys = Set<UInt16>()
    private var buttons = [UInt8]()
    private var clickHistory: [UInt8: Click] = [:]
    private var capsLock = false
    private var horizontalRemainder: Int32 = 0
    private var verticalRemainder: Int32 = 0

    init(displays: [Display], keymap: HIDKeymap, backend: V2MacInputBackend,
         doubleClickInterval: TimeInterval = 0.5,
         now: @escaping () -> TimeInterval = { ProcessInfo.processInfo.systemUptime }) throws {
        guard (1...64).contains(displays.count), Set(displays.map(\.id)).count == displays.count,
              doubleClickInterval.isFinite, doubleClickInterval >= 0 else { throw WireError.invalid("Invalid macOS receiver displays or click interval") }
        screenIDs = displays.map(\.id)
        self.displays = Dictionary(uniqueKeysWithValues: displays.map { ($0.id, $0.bounds) })
        self.keymap = keymap
        self.backend = backend
        self.now = now
        self.doubleClickInterval = doubleClickInterval
        var screens = [UInt8](repeating: 0, count: 8 + 24 * displays.count)
        screens[0] = 1; screens[1] = 3; screens.put16(4, UInt16(displays.count))
        for (index, display) in displays.enumerated() {
            let b = display.bounds
            guard display.id != 0, let x = Int32(exactly: b.origin.x), let y = Int32(exactly: b.origin.y),
                  let width = UInt16(exactly: b.size.width), let height = UInt16(exactly: b.size.height), width != 0, height != 0 else {
                throw WireError.invalid("Invalid macOS receiver screen geometry")
            }
            let offset = 8 + index * 24
            screens.put64(offset, display.id)
            screens.put32(offset + 8, UInt32(bitPattern: x))
            screens.put32(offset + 12, UInt32(bitPattern: y))
            screens.put32(offset + 16, UInt32(width))
            screens.put32(offset + 20, UInt32(height))
        }
        guard V2Frame(kind: 1, sequence: 1, epoch: 1, payload: screens).valid else { throw WireError.invalid("Invalid v2 screen advertisement") }
        screenAdvertisement = screens
    }

    static func localDisplays() throws -> [Display] {
        var ids = [CGDirectDisplayID](repeating: 0, count: 65)
        var count: UInt32 = 0
        guard CGGetActiveDisplayList(UInt32(ids.count), &ids, &count) == .success, (1...64).contains(count) else {
            throw WireError.invalid("Cannot enumerate macOS receiver displays")
        }
        let current = ids.prefix(Int(count))
        let fingerprints = current.map { id -> UInt64? in
            guard let uuid = CGDisplayCreateUUIDFromDisplayID(id)?.takeRetainedValue() else { return nil }
            let raw = CFUUIDCreateString(nil, uuid) as String
            let hash = SHA256.hash(data: Data(("Hop screen " + raw).utf8)).prefix(8)
            return hash.reduce(UInt64(0)) { ($0 << 8) | UInt64($1) } | (UInt64(1) << 63)
        }
        // WindowServer's numeric ID can change at restart. Prefer the
        // Display UUID, but keep distinct screens if the OS reports a duplicate.
        return zip(current, fingerprints).map { native, fingerprint in
            let wireID = fingerprint.flatMap { value in fingerprints.filter({ $0 == value }).count == 1 ? value : nil } ?? UInt64(native)
            return Display(id: wireID, bounds: CGDisplayBounds(native), nativeID: native)
        }
    }

    func enter(_ payload: [UInt8], epoch: UInt64) -> Bool {
        guard focus == nil, keys.isEmpty, buttons.isEmpty, !capsLock,
              V2Frame(kind: 1, sequence: 1, epoch: epoch, payload: payload).valid,
              payload[1] == 0x10, payload.u16(16) == 0, payload[18] == 0,
              let bounds = displays[payload.u64(4)], backend.authorized, backend.canBegin() else { return false }
        let x = payload.u16(12), y = payload.u16(14)
        let at = Self.point(in: bounds, x: x, y: y)
        guard backend.move(to: at, dragging: nil, flags: []) else { return false }
        focus = (epoch, Display(id: payload.u64(4), bounds: bounds))
        normalized = (x, y); position = at; leave = nil
        verticalEdge = false; horizontalEdge = false
        return true
    }

    func input(_ payload: [UInt8], epoch: UInt64) -> Bool {
        guard let focus, focus.epoch == epoch, backend.authorized,
              V2Frame(kind: 2, sequence: 1, epoch: epoch, payload: payload).valid else { return false }
        switch payload[1] {
        case 1, 2:
            guard payload.u64(4) == focus.display.id else { return false }
            let x = payload.u16(12), y = payload.u16(14)
            let at = Self.point(in: focus.display.bounds, x: x, y: y)
            if payload[1] == 1 {
                guard backend.move(to: at, dragging: buttons.last, flags: modifierFlags()) else { return false }
                trackLeave(screen: focus.display.id, x: x, y: y, moving: true)
                position = at
                return true
            }
            let button = payload[16], down = payload[17] == 1
            let held = buttons.reduce(UInt8(0)) { $0 | (1 << ($1 - 1)) }
            let bit = UInt8(1) << (button - 1)
            guard (held & bit == 0) == down, (down ? held | bit : held & ~bit) == payload[18] else { return false }
            trackLeave(screen: focus.display.id, x: x, y: y, moving: false)
            position = at
            if down { buttons.append(button) }
            let clicks = down ? clickCount(button, at: at) : (clickHistory[button]?.count ?? 1)
            guard backend.button(button, down: down, at: at, clicks: clicks, flags: modifierFlags()) else { return false }
            if !down { buttons.removeAll { $0 == button } }
            return true
        case 3:
            horizontalRemainder += Int32(bitPattern: payload.u32(4))
            verticalRemainder += Int32(bitPattern: payload.u32(8))
            let h = horizontalRemainder / 120, v = verticalRemainder / 120
            horizontalRemainder %= 120; verticalRemainder %= 120
            return (h == 0 && v == 0) || backend.wheel(horizontal: h, vertical: v, at: position, flags: modifierFlags())
        case 4:
            let usage = payload.u16(4), down = payload[8] == 1, repeated = payload[9] == 1
            guard let code = keymap.hidToMac[usage], down ? keys.contains(usage) == repeated : keys.contains(usage) else { return false }
            if down { keys.insert(usage) }
            if usage == 0x39 && down && !repeated { capsLock.toggle() }
            // The wire mask is descriptive. Derive Quartz flags only from
            // successfully tracked explicit transitions on this focus.
            var flags = modifierFlags(excluding: down ? nil : usage)
            if (0x4f...0x52).contains(usage) || (0x54...0x63).contains(usage) || usage == 0x67 { flags.insert(.maskNumericPad) }
            guard backend.key(code, down: down, repeated: repeated, modifier: Self.isModifier(usage), flags: flags) else { return false }
            if !down { keys.remove(usage) }
            return true
        default:
            return false
        }
    }

    private func trackLeave(screen: UInt64, x: UInt16, y: UInt16, moving: Bool) {
        verticalEdge = (x == 0 || x == .max) && (normalized.x == x ? verticalEdge : moving)
        horizontalEdge = (y == 0 || y == .max) && (normalized.y == y ? horizontalEdge : moving)
        if (moving || leave != nil) && (verticalEdge || horizontalEdge) {
            leave = Leave(screen: screen,
                          x: horizontalEdge && !verticalEdge ? min(65534, max(1, x)) : x,
                          y: verticalEdge && !horizontalEdge ? min(65534, max(1, y)) : y)
        } else {
            leave = nil
        }
        normalized = (x, y)
    }

    func takeLeave() -> Leave? {
        defer { leave = nil }
        return leave
    }

    @discardableResult
    func restore() -> Bool {
        focus = nil; leave = nil
        verticalEdge = false; horizontalEdge = false
        clickHistory.removeAll()
        horizontalRemainder = 0; verticalRemainder = 0
        guard !keys.isEmpty || !buttons.isEmpty || capsLock else { return true }
        guard backend.authorized else { return false }
        for button in buttons.reversed() {
            if backend.button(button, down: false, at: position, clicks: 1, flags: modifierFlags()) {
                buttons.removeAll { $0 == button }
            }
        }
        // Keep modifiers while ordinary keys are released.
        for usage in keys.sorted(by: { (Self.isModifier($0) ? 1 : 0, $0) < (Self.isModifier($1) ? 1 : 0, $1) }) {
            if let code = keymap.hidToMac[usage],
               backend.key(code, down: false, repeated: false, modifier: Self.isModifier(usage), flags: modifierFlags(excluding: usage)) {
                keys.remove(usage)
            }
        }
        if capsLock, let code = keymap.hidToMac[0x39], !keys.contains(0x39),
           backend.key(code, down: false, repeated: false, modifier: true, flags: modifierFlags(includeCaps: false)) {
            capsLock = false
        }
        return keys.isEmpty && buttons.isEmpty && !capsLock
    }

    private func clickCount(_ button: UInt8, at point: CGPoint) -> Int {
        let time = now()
        var count = 1
        if let last = clickHistory[button], time >= last.at, time - last.at <= doubleClickInterval,
           abs(last.point.x - point.x) <= 4, abs(last.point.y - point.y) <= 4 {
            count = min(last.count + 1, 3)
        }
        clickHistory[button] = Click(point: point, at: time, count: count)
        return count
    }

    private static func point(in bounds: CGRect, x: UInt16, y: UInt16) -> CGPoint {
        CGPoint(x: bounds.minX + (CGFloat(x) * (bounds.width - 1) / 65535).rounded(),
                y: bounds.minY + (CGFloat(y) * (bounds.height - 1) / 65535).rounded())
    }

    private static func isModifier(_ usage: UInt16) -> Bool { usage == 0x39 || (0xe0...0xe7).contains(usage) }

    private func modifierFlags(excluding: UInt16? = nil, includeCaps: Bool = true) -> CGEventFlags {
        var flags: CGEventFlags = capsLock && includeCaps ? .maskAlphaShift : []
        for (index, (family, side)) in Self.modifierFlagsByUsage.enumerated() {
            let usage = UInt16(0xe0 + index)
            if usage != excluding && keys.contains(usage) { flags.formUnion(family); flags.formUnion(CGEventFlags(rawValue: side)) }
        }
        return flags
    }
}

/// Quartz backend. A product owner supplies a local key-idle observation from
/// its own input monitor; startup never prompts for Accessibility permission.
final class V2QuartzInputBackend: V2MacInputBackend {
    static let eventMarker: Int64 = 0x484f50324d4143
    private let source: CGEventSource
    private let localKeysAreUp: () -> Bool

    init?(localKeysAreUp: @escaping () -> Bool) {
        guard let source = CGEventSource(stateID: .privateState) else { return nil }
        self.source = source
        self.localKeysAreUp = localKeysAreUp
        source.localEventsSuppressionInterval = 0
        source.setLocalEventsFilterDuringSuppressionState([.permitLocalMouseEvents, .permitLocalKeyboardEvents, .permitSystemDefinedEvents],
                                                         state: .eventSuppressionStateRemoteMouseDrag)
    }
    var authorized: Bool { CGPreflightPostEventAccess() }
    func canBegin() -> Bool {
        let modifiers: CGEventFlags = [.maskAlphaShift, .maskShift, .maskControl, .maskAlternate, .maskCommand, .maskSecondaryFn]
        return localKeysAreUp() && CGEventSource.flagsState(.hidSystemState).intersection(modifiers).isEmpty &&
            (0..<32).allSatisfy { raw in
                guard let button = CGMouseButton(rawValue: UInt32(raw)) else { return true }
                return !CGEventSource.buttonState(.hidSystemState, button: button)
            }
    }
    func move(to: CGPoint, dragging: UInt8?, flags: CGEventFlags) -> Bool {
        let type: CGEventType
        switch dragging {
        case nil: type = .mouseMoved
        case 1: type = .leftMouseDragged
        case 2: type = .rightMouseDragged
        default: type = .otherMouseDragged
        }
        return mouse(type, at: to, button: dragging ?? 1, clicks: 0, flags: flags)
    }
    func button(_ button: UInt8, down: Bool, at: CGPoint, clicks: Int, flags: CGEventFlags) -> Bool {
        let type: CGEventType
        switch button {
        case 1: type = down ? .leftMouseDown : .leftMouseUp
        case 2: type = down ? .rightMouseDown : .rightMouseUp
        default: type = down ? .otherMouseDown : .otherMouseUp
        }
        return mouse(type, at: at, button: button, clicks: clicks, flags: flags)
    }
    func wheel(horizontal: Int32, vertical: Int32, at: CGPoint, flags: CGEventFlags) -> Bool {
        guard (-1000...1000).contains(horizontal), (-1000...1000).contains(vertical),
              let event = CGEvent(scrollWheelEvent2Source: source, units: .line, wheelCount: 2,
                                  wheel1: -vertical, wheel2: -horizontal, wheel3: 0) else { return false }
        event.location = at
        return post(event, flags: flags)
    }
    func key(_ keyCode: UInt16, down: Bool, repeated: Bool, modifier: Bool, flags: CGEventFlags) -> Bool {
        guard let event = CGEvent(keyboardEventSource: source, virtualKey: keyCode, keyDown: down) else { return false }
        if modifier { event.type = .flagsChanged }
        event.setIntegerValueField(.keyboardEventAutorepeat, value: repeated ? 1 : 0)
        return post(event, flags: flags)
    }
    private func mouse(_ type: CGEventType, at: CGPoint, button: UInt8, clicks: Int, flags: CGEventFlags) -> Bool {
        guard (1...5).contains(button) else { return false }
        let raw = UInt32(button - 1)
        guard let event = CGEvent(mouseEventSource: source, mouseType: type, mouseCursorPosition: at,
                                  mouseButton: CGMouseButton(rawValue: raw) ?? .center) else { return false }
        event.setIntegerValueField(.mouseEventButtonNumber, value: Int64(raw))
        if clicks > 0 { event.setIntegerValueField(.mouseEventClickState, value: Int64(clicks)) }
        return post(event, flags: flags)
    }
    private func post(_ event: CGEvent, flags: CGEventFlags) -> Bool {
        event.flags = flags
        event.setIntegerValueField(.eventSourceUserData, value: Self.eventMarker)
        event.post(tap: .cghidEventTap)
        return true
    }
}
