import Foundation
import ApplicationServices

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
        var sent = [Message](); input.send = { sent.append($0) }
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
        print("PASS: \(checks) native recovery-order/idempotency/failure-retry checks with fake cursor APIs; no user input capture")
    }
}
