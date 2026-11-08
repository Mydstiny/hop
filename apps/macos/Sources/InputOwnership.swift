import Foundation

/// Tracks where a physical press began, independently of the forwarding/connection state.
/// A remotely swallowed down never becomes a local repeat, release or drag after restoration.
struct InputOwnership {
    private(set) var localKeys = Set<UInt16>()
    private(set) var localButtons = Set<Int>()
    private(set) var remoteKeys = Set<UInt16>()
    private(set) var remoteButtons = Set<Int>()
    var needsDrain: Bool { !remoteKeys.isEmpty || !remoteButtons.isEmpty }

    mutating func seedLocal(keys: Set<UInt16>, buttons: Set<Int>) {
        localKeys.formUnion(keys.subtracting(remoteKeys))
        localButtons.formUnion(buttons.subtracting(remoteButtons))
    }
    /// A removed/lost tap may miss releases. On restart discard entries no longer physically held.
    /// This cannot undo events already delivered while capture was unavailable.
    mutating func reconcile(heldKeys: Set<UInt16>, heldButtons: Set<Int>) {
        localKeys.formIntersection(heldKeys); remoteKeys.formIntersection(heldKeys)
        localButtons.formIntersection(heldButtons); remoteButtons.formIntersection(heldButtons)
    }
    /// true means suppress locally. Repeated down events have exactly the original press's ownership.
    mutating func key(_ code: UInt16, down: Bool, remote: Bool) -> Bool {
        if remote {
            if down { if !localKeys.contains(code) { remoteKeys.insert(code) } }
            else {
                remoteKeys.remove(code)
                // ENTER can mirror a press that macOS already delivered locally. Its release
                // must also reach macOS, even while the same release is forwarded remotely.
                return localKeys.remove(code) == nil
            }
            return true
        }
        if remoteKeys.contains(code) {
            if !down { remoteKeys.remove(code) }
            return true
        }
        if down { localKeys.insert(code) } else { localKeys.remove(code) }
        return false
    }
    mutating func button(_ index: Int, down: Bool, remote: Bool) -> Bool {
        if remote {
            if down { if !localButtons.contains(index) { remoteButtons.insert(index) } }
            else {
                remoteButtons.remove(index)
                return localButtons.remove(index) == nil
            }
            return true
        }
        if remoteButtons.contains(index) {
            if !down { remoteButtons.remove(index) }
            return true
        }
        if down { localButtons.insert(index) } else { localButtons.remove(index) }
        return false
    }
    func suppressLocalDrag(_ index: Int) -> Bool { remoteButtons.contains(index) }
    /// Remove a quarantined modifier group's flag unless another locally-owned side holds that group.
    func modifierGroupsToClear(_ masks: [UInt16: UInt16]) -> UInt16 {
        let remote = remoteKeys.reduce(UInt16(0)) { $0 | (masks[$1] ?? 0) }
        let local = localKeys.reduce(UInt16(0)) { $0 | (masks[$1] ?? 0) }
        return [UInt16(3), 12, 48, 192].reduce(UInt16(0)) { result, group in
            result | (remote & group != 0 && local & group == 0 ? group : 0)
        }
    }
}
