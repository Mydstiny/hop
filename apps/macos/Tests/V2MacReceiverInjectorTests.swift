import Foundation
import CoreGraphics
import IOKit.hidsystem

@main struct V2MacReceiverInjectorTests {
    private enum Event: Equatable {
        case move(CGPoint, UInt8?, CGEventFlags)
        case button(UInt8, Bool, CGPoint, Int, CGEventFlags)
        case wheel(Int32, Int32, CGPoint, CGEventFlags)
        case key(UInt16, Bool, Bool, Bool, CGEventFlags)
    }
    private final class FakeInput: V2MacInputBackend {
        var authorized = true
        var neutral = true
        var events: [Event] = []
        var fail: (Event) -> Bool = { _ in false }
        func canBegin() -> Bool { neutral }
        func move(to: CGPoint, dragging: UInt8?, flags: CGEventFlags) -> Bool { post(.move(to, dragging, flags)) }
        func button(_ button: UInt8, down: Bool, at: CGPoint, clicks: Int, flags: CGEventFlags) -> Bool {
            post(.button(button, down, at, clicks, flags))
        }
        func wheel(horizontal: Int32, vertical: Int32, at: CGPoint, flags: CGEventFlags) -> Bool {
            post(.wheel(horizontal, vertical, at, flags))
        }
        func key(_ keyCode: UInt16, down: Bool, repeated: Bool, modifier: Bool, flags: CGEventFlags) -> Bool {
            post(.key(keyCode, down, repeated, modifier, flags))
        }
        private func post(_ event: Event) -> Bool { events.append(event); return !fail(event) }
    }

    static func main() throws {
        var checks = 0
        func check(_ result: Bool, _ detail: String) {
            checks += 1
            if !result { fputs("FAIL Mac receiver injector: \(detail)\n", stderr); exit(1) }
        }
        let keymap = try HIDKeymap(keymap: Keymap.load(url: URL(fileURLWithPath: CommandLine.arguments[1])))
        let screen = V2MacReceiverInjector.Display(id: 42, bounds: CGRect(x: -900, y: -100, width: 900, height: 501))
        let second = V2MacReceiverInjector.Display(id: 7, bounds: CGRect(x: 0, y: 0, width: 1200, height: 900))
        let fake = FakeInput()
        var time = 2.0
        let receiver = try V2MacReceiverInjector(displays: [screen, second], keymap: keymap, backend: fake, now: { time })
        do {
            _ = try V2MacReceiverInjector(displays: [screen, screen], keymap: keymap, backend: fake)
            check(false, "duplicate displays rejected")
        } catch { check(true, "duplicate displays rejected") }
        let advertisement = receiver.screenAdvertisement
        check(receiver.screenIDs == [42, 7] && advertisement.u16(4) == 2 &&
              Int32(bitPattern: advertisement.u32(16)) == -900 && advertisement.u32(24) == 900 &&
              V2Frame(kind: 1, sequence: 1, epoch: 1, payload: advertisement).valid, "negative global display geometry advertised")

        func point(_ op: UInt8, x: UInt16, y: UInt16, screen: UInt64 = 42, tail: [UInt8] = []) -> [UInt8] {
            var payload = [UInt8](repeating: 0, count: 16)
            payload[0] = 1; payload[1] = op
            payload.put64(4, screen); payload.put16(12, x); payload.put16(14, y)
            return payload + tail
        }
        func key(_ usage: UInt16, down: Bool, repeated: Bool = false, reported: UInt16 = 0) -> [UInt8] {
            var payload: [UInt8] = [1,4,0,0,0,0,0,0,down ? 1 : 0,repeated ? 1 : 0,0,0]
            payload.put16(4, usage); payload.put16(6, reported)
            return payload
        }
        func wheel(_ horizontal: Int32, _ vertical: Int32) -> [UInt8] {
            var payload: [UInt8] = [1,3,0,0,0,0,0,0,0,0,0,0]
            payload.put32(4, UInt32(bitPattern: horizontal)); payload.put32(8, UInt32(bitPattern: vertical))
            return payload
        }
        let entry = point(0x10, x: 0, y: 32768, tail: [0,0,0,0])
        let first = CGPoint(x: -900, y: 150), right = CGPoint(x: -1, y: 150)
        let moveRight = point(1, x: .max, y: 32768)
        let moveInside = point(1, x: 32768, y: 32768)
        let fifthDown = point(2, x: .max, y: 32768, tail: [5,1,16,0])
        let fifthUp = point(2, x: .max, y: 32768, tail: [5,0,0,0])
        let leftDown = point(2, x: .max, y: 32768, tail: [1,1,1,0])
        let a = keymap.hidToMac[4]!, shift = keymap.hidToMac[0xe1]!, caps = keymap.hidToMac[0x39]!
        let shiftFlags: CGEventFlags = [.maskShift, CGEventFlags(rawValue: UInt64(NX_DEVICELSHIFTKEYMASK))]
        fake.authorized = false
        check(!receiver.enter(entry, epoch: 11) && fake.events.isEmpty, "permission denied before enter")
        fake.authorized = true; fake.neutral = false
        check(!receiver.enter(entry, epoch: 11) && fake.events.isEmpty, "physical local input blocks enter")
        fake.neutral = true
        check(!receiver.enter(point(0x10, x: 0, y: 0, tail: [0,1,0,0]), epoch: 11), "non-neutral ENTER rejected")
        check(receiver.enter(entry, epoch: 11) && fake.events.last == .move(first, nil, []), "normalized cursor enters selected display")
        check(!receiver.enter(entry, epoch: 12) && !receiver.input(moveRight, epoch: 10), "active focus cannot be stolen or injected by a stale epoch")
        check(!receiver.input(point(1, x: 4, y: 5, screen: 7), epoch: 11), "another display cannot receive active input")
        check(!receiver.input([], epoch: 11), "truncated input rejected")
        check(receiver.input(point(1, x: 0, y: 32768), epoch: 11) && receiver.takeLeave() == nil, "continued movement along entry edge does not return")
        check(receiver.input(moveRight, epoch: 11) && fake.events.last == .move(right, nil, []) &&
              receiver.takeLeave() == .init(screen: 42, x: .max, y: 32768) && receiver.takeLeave() == nil,
              "edge arrival reports normalized leave once")
        check(receiver.input(moveRight, epoch: 11) && receiver.takeLeave() == .init(screen: 42, x: .max, y: 32768),
              "clamped motion re-arms the same edge after a rejected request")
        check(receiver.input(moveInside, epoch: 11) && receiver.input(moveRight, epoch: 11) &&
              receiver.input(moveInside, epoch: 11) && receiver.takeLeave() == nil, "leave is dropped when cursor moves off boundary before dispatch")

        check(receiver.input(fifthDown, epoch: 11) && !receiver.input(fifthDown, epoch: 11), "forward button and duplicate-down protection")
        check(receiver.input(moveRight, epoch: 11) && fake.events.last == .move(right, 5, []), "held forward button preserves drag type")
        check(receiver.input(fifthUp, epoch: 11), "button up follows drag")
        time += 0.2
        check(receiver.input(fifthDown, epoch: 11) && fake.events.last == .button(5, true, right, 2, []) &&
              receiver.input(fifthUp, epoch: 11) && fake.events.last == .button(5, false, right, 2, []), "double-click state is balanced")
        let count = fake.events.count
        check(receiver.input(wheel(60, -60), epoch: 11) && fake.events.count == count, "fractional detents accumulate without loss")
        check(receiver.input(wheel(60, -60), epoch: 11) && fake.events.last == .wheel(1, -1, right, []), "two wheel axes retain sign")

        check(receiver.input(key(4, down: true, reported: 0x1ff), epoch: 11) &&
              fake.events.last == .key(a, true, false, false, []), "descriptive wire modifier mask cannot synthesize flags")
        check(receiver.input(key(4, down: true, repeated: true), epoch: 11) &&
              fake.events.last == .key(a, true, true, false, []) && receiver.input(key(4, down: false), epoch: 11), "repeat requires and preserves a tracked down")
        check(!receiver.input(key(4, down: true, repeated: true), epoch: 11), "repeat before down rejected")
        check(receiver.input(key(0xe1, down: true), epoch: 11) &&
              fake.events.last == .key(shift, true, false, true, shiftFlags), "explicit left Shift sets Quartz family and side flags")
        check(receiver.input(key(4, down: true), epoch: 11) && fake.events.last == .key(a, true, false, false, shiftFlags),
              "tracked Shift applies to subsequent key event")
        check(receiver.input(leftDown, epoch: 11), "button held during recovery")
        fake.authorized = false
        let beforeRestore = fake.events.count
        check(!receiver.input(moveInside, epoch: 11) && !receiver.restore() && fake.events.count == beforeRestore,
              "revoked permission stops input and retains pending releases")
        fake.authorized = true
        check(!receiver.enter(entry, epoch: 12), "new ENTER cannot skip unreleased input")
        check(receiver.restore() && Array(fake.events.suffix(3)) == [
            .button(1, false, right, 1, shiftFlags),
            .key(a, false, false, false, shiftFlags),
            .key(shift, false, false, true, [])
        ], "cleanup releases only held input and leaves modifiers until ordinary keys are up")
        let afterRestore = fake.events.count
        check(receiver.restore() && fake.events.count == afterRestore, "cleanup is idempotent")

        check(receiver.enter(entry, epoch: 12), "clean focus can re-enter")
        fake.fail = { $0 == .key(a, true, false, false, []) }
        check(!receiver.input(key(4, down: true), epoch: 12), "backend failure aborts attempted injection")
        fake.fail = { $0 == .key(a, false, false, false, []) }
        check(!receiver.restore() && !receiver.enter(entry, epoch: 13), "failed up transition remains pending for retry")
        fake.fail = { _ in false }
        check(receiver.restore() && fake.events.last == .key(a, false, false, false, []), "cleanup retries uncertain down transition")
        check(receiver.enter(entry, epoch: 13) && receiver.input(key(0x39, down: true), epoch: 13) &&
              receiver.input(key(0x39, down: false), epoch: 13) && fake.events.last == .key(caps, false, false, true, .maskAlphaShift),
              "explicit Caps Lock transition controls virtual flags")
        check(receiver.restore() && fake.events.last == .key(caps, false, false, true, []), "focus cleanup clears virtual Caps Lock flags")
        print("PASS \(checks) macOS v2 receiver injector checks; fake backend, no Quartz events posted")
    }
}
