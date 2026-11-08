import Foundation

/// The production ownership policy, with no CGEvent, tap, cursor or physical-state queries.
@main struct OwnershipTests {
    static func main() throws {
        let root = URL(fileURLWithPath: CommandLine.arguments[1])
        let keymap = try Keymap.load(url: root.appendingPathComponent("protocol/keymap.json"))
        var checks = 0
        func check(_ value: Bool, _ label: String) {
            checks += 1
            if !value { fputs("FAIL ownership: \(label)\n", stderr); exit(1) }
        }
        for code: UInt16 in [0, 55, 123, 255, UInt16.max] {
            var ledger = InputOwnership()
            check(ledger.key(code, down: true, remote: true), "remote down swallowed, including unsupported code \(code)")
            check(ledger.remoteKeys == [code] && ledger.needsDrain, "physical ownership independent of supported keymap")
            check(ledger.key(code, down: true, remote: true), "remote repeat stays remote-owned")
            check(ledger.key(code, down: true, remote: false), "LOCAL quarantines remote repeat after restore")
            let fresh: UInt16 = code == 1 ? 2 : 1
            check(!ledger.key(fresh, down: true, remote: false), "fresh LOCAL key passes while draining")
            check(!ledger.key(fresh, down: false, remote: false), "fresh LOCAL up passes while draining")
            check(ledger.key(code, down: false, remote: false), "remote-owned physical up swallowed after return")
            check(!ledger.needsDrain && ledger.remoteKeys.isEmpty, "up drains ownership")
            check(!ledger.key(code, down: true, remote: false), "new press after release belongs locally; no synthetic down")
            check(!ledger.key(code, down: false, remote: false), "new local press release passes")
        }
        for index in [0, 1, 4, 7, 31] {
            var ledger = InputOwnership()
            check(ledger.button(index, down: true, remote: true), "remote button down swallowed, including unsupported \(index)")
            check(ledger.suppressLocalDrag(index), "remote drag tail quarantined until physical release")
            check(ledger.button(index, down: true, remote: false), "repeated button down cannot transfer ownership")
            let fresh = index == 1 ? 2 : 1
            check(!ledger.button(fresh, down: true, remote: false), "fresh local button passes during drain")
            check(!ledger.suppressLocalDrag(fresh), "fresh local drag passes during drain")
            check(!ledger.button(fresh, down: false, remote: false), "fresh local button up passes")
            check(ledger.button(index, down: false, remote: false), "remote button up swallowed")
            check(!ledger.suppressLocalDrag(index) && !ledger.needsDrain, "physical release ends drag quarantine")
            check(!ledger.button(index, down: true, remote: false), "next physical click is local")
            check(!ledger.button(index, down: false, remote: false), "next click release is local")
        }
        let command = keymap.code(symbol: "kVK_Command")!
        let shift = keymap.code(symbol: "kVK_Shift")!
        let rightShift = keymap.code(symbol: "kVK_RightShift")!
        var local = InputOwnership()
        local.seedLocal(keys: [command, shift], buttons: [0, 7])
        check(!local.needsDrain, "ENTER snapshots remain LOCAL-owned, not quarantined")
        check(local.key(command, down: true, remote: true), "remote captures preheld modifier repeats")
        check(local.localKeys.contains(command) && !local.remoteKeys.contains(command), "repeats preserve original local ownership")
        check(!local.key(command, down: false, remote: true), "preheld modifier up passes locally while also forwarded")
        check(!local.button(0, down: false, remote: true), "preheld ENTER button release passes locally while forwarded")
        check(!local.button(7, down: false, remote: true), "unsupported preheld local button release also passes")
        check(!local.key(shift, down: false, remote: false), "preheld modifier up after restoration passes")
        check(local.localKeys.isEmpty && local.localButtons.isEmpty, "observed releases clear local ownership")
        check(local.key(command, down: true, remote: true), "new remote press after local release is remote-owned")
        check(local.key(command, down: false, remote: false), "that new press's up remains quarantined")

        var mixed = InputOwnership()
        _ = mixed.key(shift, down: true, remote: true)
        check(mixed.modifierGroupsToClear(keymap.modifiers) & 3 == 3, "quarantined shift cannot modify unrelated local input")
        mixed.seedLocal(keys: [rightShift], buttons: [])
        check(mixed.modifierGroupsToClear(keymap.modifiers) & 3 == 0, "locally held other side keeps its legitimate modifier group")
        check(!mixed.key(rightShift, down: false, remote: true), "local side release passes even when other side remote-owned")
        check(mixed.modifierGroupsToClear(keymap.modifiers) & 3 == 3, "after local side release, clear remote-only group")
        check(mixed.key(shift, down: false, remote: false), "remote side release swallowed")
        check(mixed.modifierGroupsToClear(keymap.modifiers) == 0, "released remote modifiers no longer affect sanitization")
        for symbol in ["kVK_Control", "kVK_Option", "kVK_Command"] {
            let code = keymap.code(symbol: symbol)!, bit = keymap.modifiers[keymap.code(symbol: symbol)!]!
            var ledger = InputOwnership()
            _ = ledger.key(code, down: true, remote: true)
            check(ledger.modifierGroupsToClear(keymap.modifiers) & bit != 0, "sanitize remote-owned \(symbol)")
            _ = ledger.key(code, down: false, remote: false)
            check(ledger.modifierGroupsToClear(keymap.modifiers) == 0, "\(symbol) drain ends sanitization")
        }
        var restart = InputOwnership()
        restart.seedLocal(keys: [command, shift], buttons: [0, 7])
        _ = restart.key(255, down: true, remote: true)
        _ = restart.button(4, down: true, remote: true)
        restart.reconcile(heldKeys: [shift, 255], heldButtons: [7])
        check(restart.localKeys == [shift] && restart.localButtons == [7], "restart drops local releases missed while tap absent")
        check(restart.remoteKeys == [255] && restart.remoteButtons.isEmpty, "restart keeps only still-held quarantined input")
        restart.seedLocal(keys: [255], buttons: [])
        check(!restart.localKeys.contains(255), "ENTER must never reclassify remote ownership as local")
        restart.reconcile(heldKeys: [], heldButtons: [])
        check(!restart.needsDrain && restart.localKeys.isEmpty && restart.localButtons.isEmpty, "restart reconciliation drains physically released entries")
        print("PASS: \(checks) pure production physical-ownership/quarantine checks; no user input capture or physical queries")
    }
}
