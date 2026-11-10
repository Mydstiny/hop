import Foundation
import ApplicationServices

private final class MemoryPointerSettings: PointerSettingsStore {
    var values: [String: Data] = [:]
    func data(forKey key: String) -> Data? { values[key] }
    func set(_ value: Any?, forKey key: String) { values[key] = value as? Data }
}

/// Runs the ACTUAL synchronous recovery routine against fake cursor operations.
/// Does not create NSApplication/CGEvent/CGEventTap or query/capture physical keyboard/mouse state.
@main struct RecoveryTests {
    static func main() throws {
        let root = URL(fileURLWithPath: CommandLine.arguments[1])
        let keymap = try Keymap.load(url: root.appendingPathComponent("protocol/keymap.json"))
        var checks = 0
        func check(_ condition: Bool, _ message: String) {
            checks += 1
            if !condition { fputs("FAIL recovery: \(message)\n", stderr); exit(1) }
        }
        var input: InputCapture!
        var calls = [String]()
        var associateFails = false
        var showFails = false
        let cursor = CursorOperations(associate: { value in
            check(value, "restoration only re-associates; never detaches")
            check(input.mode == .local && input.recoveryTestIsCleared, "clear forwarding state BEFORE association")
            check(input.recoveryTestQuarantine, "separate physical quarantine survives forwarding-state restoration")
            calls.append("associate")
            return associateFails ? .failure : .success
        }, hide: { _ in
            fatalError("Recovery tests must never hide cursor")
        }, show: { _ in
            check(input.mode == .local && input.recoveryTestIsCleared, "local state before show")
            calls.append("show"); return showFails ? .failure : .success
        }, warp: { point in
            check(input.mode == .local && point == CGPoint(x: 10, y: 20), "warp safe return point without synthesized keys/clicks")
            calls.append("warp"); return .success
        })
        input = InputCapture(keymap: keymap, cursor: cursor)
        let leftShift = CGEventFlags(rawValue: CGEventFlags.maskShift.rawValue | 0x0002)
        let bothShift = CGEventFlags(rawValue: CGEventFlags.maskShift.rawValue | 0x0002 | 0x0004)
        let rightShift = CGEventFlags(rawValue: CGEventFlags.maskShift.rawValue | 0x0004)
        check(eventModifiers(previous: 0, flags: leftShift, changedBit: 1) == 1, "left shift from event rather than system key-state query")
        check(eventModifiers(previous: 1, flags: bothShift, changedBit: 2) == 3, "independent left and right shifts")
        check(eventModifiers(previous: 3, flags: rightShift, changedBit: 1) == 2, "one side release preserves other")
        check(eventModifiers(previous: 2, flags: [], changedBit: 2) == 0, "aggregate release clears both sides")
        check(eventModifiers(previous: 0, flags: .maskCommand, changedBit: 64) == 64, "aggregate-only modifier press")
        check(eventModifiers(previous: 64, flags: .maskCommand, changedBit: 128) == 192, "aggregate-only second modifier press")
        check(eventModifiers(previous: 192, flags: .maskCommand, changedBit: 64) == 128, "aggregate-only one side release")
        check(eventModifiers(previous: 0, flags: .maskAlphaShift, changedBit: 256) == 256, "caps latch follows the event flag")
        check(eventModifiers(previous: 256, flags: [], changedBit: 256) == 0, "caps latch turns off")
        let originalTuning = MouseTuning()
        check(originalTuning.valid && originalTuning.wheel(1, horizontalAxis: false) == -120, "default v1 wheel sign and magnitude unchanged")
        check(originalTuning.wheel(Int64.min, horizontalAxis: false) == 120000, "wheel clamps before multiplication")
        check(originalTuning.wheel(Int64.max, horizontalAxis: true) == -120000, "wheel maximum cannot overflow")
        let customized = MouseTuning(movement: 2, horizontal: 0.5, vertical: 3, reverseHorizontal: true, reverseVertical: false)
        check(customized.valid && customized.wheel(1, horizontalAxis: true) == 60, "independent horizontal speed and reversal")
        check(customized.wheel(1, horizontalAxis: false) == -360, "independent vertical speed")
        check(customized.wheel(1000, horizontalAxis: false) == -120000, "scaled wheel respects wire limit")
        check(MouseTuning.key(for: [UInt8](repeating: 0, count: 16)) == nil, "reject zero peer identity")
        check(MouseTuning.key(for: [1]) == nil && MouseTuning.key(for: [UInt8](repeating: 1, count: 16)) != nil, "settings keyed by full peer identity")
        let memory = MemoryPointerSettings()
        let peerA = [UInt8](repeating: 1, count: 16), peerB = [UInt8](repeating: 2, count: 16)
        check(customized.save(peer: peerA, defaults: memory), "valid settings saved to peer A")
        check(MouseTuning.load(peer: peerA, defaults: memory) == customized, "peer A settings round-trip")
        check(MouseTuning.load(peer: peerB, defaults: memory) == originalTuning, "peer B starts with independent defaults")
        let fingerprintA = String(repeating: "A", count: 64)
        let fingerprintB = String(repeating: "B", count: 64)
        check(customized.save(fingerprint: fingerprintA, defaults: memory), "v2 fingerprint settings saved")
        check(MouseTuning.load(fingerprint: fingerprintA, defaults: memory) == customized, "v2 fingerprint settings round-trip")
        check(MouseTuning.load(fingerprint: fingerprintB, defaults: memory) == originalTuning, "v2 targets retain independent settings")
        check(!customized.save(fingerprint: String(repeating: "a", count: 64), defaults: memory), "unverified fingerprint format rejected")
        check(!customized.save(fingerprint: String(repeating: "Z", count: 64), defaults: memory), "non-hex fingerprint rejected")
        check(!customized.save(fingerprint: "A", defaults: memory), "truncated fingerprint rejected")
        check(!MouseTuning(movement: .infinity).save(peer: peerA, defaults: memory), "invalid settings are never persisted")
        check(MouseTuning.load(peer: peerA, defaults: memory) == customized, "invalid update preserves saved settings")
        let oldData = Data("{\"movement\":2,\"horizontal\":0.5,\"vertical\":3,\"reverseHorizontal\":true,\"reverseVertical\":false}".utf8)
        memory.set(oldData, forKey: MouseTuning.key(for: peerB)!)
        check(MouseTuning.load(peer: peerB, defaults: memory) == customized, "previous settings migrate without losing speed or direction")
        check(!MouseTuning(reportRateHz: 0).valid && !MouseTuning(reportRateHz: 999999).valid, "unsupported rates cannot reach timer scheduling")
        check(MouseTuning(reportRateHz: 1000).valid, "1000 Hz forwarding ceiling is configurable")
        check(permitsFocusEntry(modifiers: 0, buttons: 0, nonmodifierKeysUp: true, requireNeutralState: true), "v2 neutral entry accepted")
        check(!permitsFocusEntry(modifiers: 64, buttons: 0, nonmodifierKeysUp: true, requireNeutralState: true), "v2 held modifier prevents entry")
        check(!permitsFocusEntry(modifiers: 256, buttons: 0, nonmodifierKeysUp: true, requireNeutralState: true), "v2 caps latch is not silently dropped")
        check(!permitsFocusEntry(modifiers: 0, buttons: 1, nonmodifierKeysUp: true, requireNeutralState: true), "v2 held button prevents entry")
        check(!permitsFocusEntry(modifiers: 0, buttons: 0, nonmodifierKeysUp: false, requireNeutralState: true), "v2 ordinary held key prevents entry")
        check(permitsFocusEntry(modifiers: 64, buttons: 1, nonmodifierKeysUp: true, requireNeutralState: false), "legacy entry behavior retained")
        input.tuning = customized
        input.tuning = MouseTuning(movement: .nan)
        check(input.tuning == customized, "non-finite settings do not reach callback")
        input.tuning = MouseTuning(movement: 5)
        check(input.tuning == customized, "out-of-range settings do not reach callback")
        var sent = [Message](); input.send = { sent.append($0) }
        input.stageForwardingMoveTest()
        input.tick()
        check(sent.isEmpty, "fixed maintenance timer does not bypass selected move rate")
        input.flushMove()
        check(sent.count == 1 && sent[0].type == .move && sent[0].epoch == 42, "move timer forwards queued movement")
        input.flushMove()
        check(sent.count == 1, "empty movement slot does not resend")
        sent.removeAll()
        for label in ["network", "protocol", "heartbeat", "event-tap-disabled", "emergency", "permission", "display", "sleep", "disable", "quit"] {
            calls.removeAll(); input.stageRecoveryTest()
            check(input.restore() == 42, "\(label) old epoch returned")
            check(calls == ["associate", "show", "warp"], "\(label) native restoration order")
            check(!input.recoveryTestCursorPending && input.mode == .local, "\(label) cleared capture")
            let count = calls.count
            check(input.restore() == nil && calls.count == count, "\(label) idempotent; show/hide counts balanced")
            check(sent.isEmpty, "recovery routine performs NO networking")
            check(input.recoveryTestQuarantine && input.recoveryTestIsCleared, "\(label) forwarding clears but quarantine persists")
            check(input.recoveryTestKey(0, down: true), "\(label) remote autorepeat cannot leak locally")
            check(input.recoveryTestKey(255, down: false), "\(label) unsupported remote key up cannot leak locally")
            check(input.recoveryTestDrag(0), "\(label) remote drag remains quarantined")
            check(!input.recoveryTestKey(1, down: true) && !input.recoveryTestKey(1, down: false), "\(label) fresh local keys pass while draining")
            check(input.recoveryTestKey(0, down: false) && input.recoveryTestKey(55, down: false), "\(label) remote releases drain without local downs")
            check(input.recoveryTestButton(0, down: false) && !input.recoveryTestDrag(0), "\(label) button release drains drag quarantine")
            check(!input.recoveryTestNeedsDrain, "\(label) all physical tails drained")
        }
        calls.removeAll(); input.stageRecoveryTest(remote: false)
        check(input.restore() == 42 && calls.isEmpty && input.recoveryTestIsCleared, "ENTER pending restoration never shows/warps/hides cursor")
        calls.removeAll(); input.stageRecoveryTest(); associateFails = true
        _ = input.restore()
        check(input.mode == .local && input.recoveryTestCursorPending, "native association failure cannot re-enable swallowing")
        associateFails = false; calls.removeAll(); input.tick()
        check(calls == ["associate", "warp"] && !input.recoveryTestCursorPending, "next tick retries failed association; no duplicate cursor-show")
        calls.removeAll(); input.stageRecoveryTest(); showFails = true
        _ = input.restore(); showFails = false; calls.removeAll(); input.tick()
        check(calls == ["show", "warp"] && !input.recoveryTestCursorPending, "next tick retries failed show; does not re-associate twice")
        calls.removeAll(); input.stageRecoveryTest()
        var faultCalls = 0
        input.fault = { text, reason in
            faultCalls += 1
            check(input.mode == .local && input.recoveryTestIsCleared && input.recoveryTestQuarantine, "fault notification sees synchronous restoration and separate quarantine")
            check(calls == ["associate", "show", "warp"] && sent.isEmpty, "cursor restoration precedes even fault callback; no network")
            check(text == "Test fault" && reason == 3, "fault identity preserved")
        }
        input.recoveryTestFault()
        check(faultCalls == 1, "actual restore-and-fault entry notifies after synchronous restoration")
        calls.removeAll(); input.stageRecoveryTest(); input.stageUsableTapTest()
        let generation = input.generation
        input.stop()
        check(input.mode == .local && input.recoveryTestIsCleared && input.recoveryTestQuarantine, "stop clears forwarding but preserves physical quarantine")
        check(input.recoveryTestStopWhenDrained && input.generation != generation, "disable requests drain-only policy and invalidates stale callbacks")
        check(!input.recoveryTestButton(1, down: true) && !input.recoveryTestDrag(1) && !input.recoveryTestButton(1, down: false), "disable drain permits fresh local clicks/drags")
        check(input.recoveryTestKey(0, down: true) && input.recoveryTestDrag(0), "disable still quarantines old repeats/drags")
        _ = input.recoveryTestKey(0, down: false); _ = input.recoveryTestKey(55, down: false)
        _ = input.recoveryTestKey(255, down: false); _ = input.recoveryTestButton(0, down: false)
        input.stop()
        check(!input.recoveryTestStopWhenDrained && !input.recoveryTestNeedsDrain, "drained stop can remove its tap")
        calls.removeAll(); input.stageRecoveryTest(); input.stageUsableTapTest()
        input.stop(drain: false)
        check(!input.recoveryTestStopWhenDrained && input.recoveryTestQuarantine, "quit/permission-loss stop cannot retain tap but does not falsely clear ownership")
        check(!input.isRunning && sent.isEmpty, "no event tap or network was started")
        input.stageRecoveryTest()
        input.stageRoutedPointerTest(x: 1000, y: 500, width: 1001, height: 1001)
        check(input.matchesRemoteEdge(x: .max, y: 32768), "authenticated peer can request the physically reached edge")
        check(!input.matchesRemoteEdge(x: 0, y: 32768) && !input.matchesRemoteEdge(x: .max, y: 28000), "wrong edge and stale along-edge position cannot reroute input")
        input.stageRecoveryTest(remote: false)
        check(!input.matchesRemoteEdge(x: .max, y: 32768), "pending capture cannot honor a remote LEAVE")
        print("PASS: \(checks) native recovery-order/idempotency/failure-retry checks with fake cursor APIs; no user input capture")
    }
}
