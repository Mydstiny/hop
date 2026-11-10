import Foundation

/// UI/input-side adapter. The delegate queues network commands without waiting;
/// all router calls are made on the same executor as the physical event tap.
protocol V2InputRouterDelegate: AnyObject {
    func requestV2Entry(screen: UInt64, x: UInt16, y: UInt16, captureEpoch: UInt32)
    func submitV2Input(_ payloads: [[UInt8]], focusEpoch: UInt64) -> Bool
    func requestV2Release()
    func acknowledgeV2Capture(_ message: Message) throws
    func restoreV2Capture(y: UInt16?)
    func abortV2Session()
}
final class V2InputRouter {
    private struct Pending { let screen: UInt64; let message: Message }
    private struct Active { let screen, focusEpoch: UInt64; let captureEpoch: UInt32 }
    private let keymap: HIDKeymap
    private weak var delegate: V2InputRouterDelegate?
    private var pending: Pending?
    private var active: Active?
    init(keymap: HIDKeymap, delegate: V2InputRouterDelegate) { self.keymap = keymap; self.delegate = delegate }
    /// Selected only from authenticated screen data, never a discovery hint.
    var selectedScreen: UInt64 = 0
    var isActive: Bool { active != nil }
    func captured(_ message: Message) {
        guard let delegate else { return }
        if message.type == .enter {
            guard selectedScreen != 0, pending == nil, active == nil, message.epoch != 0,
                  (try? Message(body: message.body)) != nil, message.payload.u16(4) == 0, message.payload.u16(6) == 0 else { fail(); return }
            pending = Pending(screen: selectedScreen, message: message)
            delegate.requestV2Entry(screen: selectedScreen, x: message.payload.u16(0), y: message.payload.u16(2), captureEpoch: message.epoch)
            return
        }
        if message.type == .leave {
            guard pending?.message.epoch == message.epoch || active?.captureEpoch == message.epoch else { return }
            pending = nil; active = nil; delegate.requestV2Release(); return
        }
        guard let active, active.captureEpoch == message.epoch else { fail(); return }
        do {
            let payloads = try V2CapturedInput.payloads(message, screen: active.screen, keys: keymap)
            guard delegate.submitV2Input(payloads, focusEpoch: active.focusEpoch) else { fail(); return }
        } catch { fail() }
    }
    /// Called after remote ENTER_ACK. A stale/mismatched event must never
    /// activate suppression or transfer newly pressed keys.
    func focused(epoch: UInt64, screen: UInt64, x: UInt16, y: UInt16, captureEpoch: UInt32? = nil) {
        if let captureEpoch, pending?.message.epoch != captureEpoch { return }
        guard let delegate, let pending, epoch != 0, pending.screen == screen,
              pending.message.payload.u16(0) == x, pending.message.payload.u16(2) == y else { fail(); return }
        do {
            try delegate.acknowledgeV2Capture(pending.message)
            active = Active(screen: screen, focusEpoch: epoch, captureEpoch: pending.message.epoch)
            self.pending = nil
        } catch {
            // The cursor or pressed state can legitimately change during the ACK.
            restore(y: nil)
            delegate.requestV2Release()
        }
    }
    /// The coordinator invokes this synchronously on the tap executor before
    /// closing a failed/released session. It must not re-enter the coordinator.
    func restore(y: UInt16?) {
        pending = nil; active = nil; delegate?.restoreV2Capture(y: y)
    }
    private func fail() {
        pending = nil; active = nil
        delegate?.restoreV2Capture(y: nil)
        delegate?.abortV2Session()
    }
}
