import Foundation

private typealias V2EventCallback = @convention(c) (UnsafeMutableRawPointer?, UnsafePointer<UInt8>?, Int, UInt64) -> Int32
private typealias V2RestoreCallback = @convention(c) (UnsafeMutableRawPointer?) -> Int32
private typealias V2LeaveCallback = @convention(c) (UnsafeMutableRawPointer?, UInt64, UInt16, UInt16) -> Int32
private typealias V2ClipboardRevokeCallback = @convention(c) (UnsafeMutableRawPointer?) -> Void
private typealias V2ClipboardSampleCallback = @convention(c) (UnsafeMutableRawPointer?, UnsafeMutablePointer<UInt64>?) -> Int32
private typealias V2ClipboardApplyCallback = @convention(c) (UnsafeMutableRawPointer?, UInt8, UInt64, UInt64, UnsafePointer<UInt8>?, Int, UInt64) -> Int32
@_silgen_name("hop_v2_create_input_only") private func sessionCreate(_ role: Int32, _ platform: Int32, _ epoch: UInt64, _ screens: UnsafePointer<UInt64>?, _ count: Int, _ caps: UInt64, _ enter: V2EventCallback?, _ input: V2EventCallback?, _ restore: V2RestoreCallback?, _ leave: V2LeaveCallback?, _ user: UnsafeMutableRawPointer?) -> OpaquePointer?
@_silgen_name("hop_v2_create_with_clipboard") private func sessionCreateWithClipboard(_ role: Int32, _ platform: Int32, _ epoch: UInt64, _ screens: UnsafePointer<UInt64>?, _ count: Int, _ caps: UInt64, _ enter: V2EventCallback?, _ input: V2EventCallback?, _ restore: V2RestoreCallback?, _ leave: V2LeaveCallback?, _ revoke: V2ClipboardRevokeCallback?, _ sample: V2ClipboardSampleCallback?, _ apply: V2ClipboardApplyCallback?, _ user: UnsafeMutableRawPointer?) -> OpaquePointer?
@_silgen_name("hop_v2_bind_peer") private func sessionBind(_ session: OpaquePointer, _ fingerprint: UnsafePointer<CChar>) -> Int32
@_silgen_name("hop_v2_require_pairing") private func sessionRequirePairing(_ session: OpaquePointer) -> Int32
@_silgen_name("hop_v2_confirm_pairing") private func sessionConfirmPairing(_ session: OpaquePointer) -> Int32
@_silgen_name("hop_v2_cancel_pairing") private func sessionCancelPairing(_ session: OpaquePointer) -> Int32
@_silgen_name("hop_v2_pairing_confirmed") private func sessionPairingConfirmed(_ session: OpaquePointer) -> Int32
@_silgen_name("hop_v2_start") private func sessionStart(_ session: OpaquePointer, _ authenticated: Int32, _ now: UInt64) -> Int32
@_silgen_name("hop_v2_tick") private func sessionTick(_ session: OpaquePointer, _ authenticated: Int32, _ now: UInt64) -> Int32
@_silgen_name("hop_v2_receive") private func sessionReceive(_ session: OpaquePointer, _ authenticated: Int32, _ bytes: UnsafePointer<UInt8>, _ count: Int, _ now: UInt64) -> Int32
@_silgen_name("hop_v2_pull") private func sessionPull(_ session: OpaquePointer, _ authenticated: Int32, _ output: UnsafeMutablePointer<UInt8>?, _ capacity: Int, _ length: UnsafeMutablePointer<Int>) -> Int32
@_silgen_name("hop_v2_advertise") private func sessionAdvertise(_ session: OpaquePointer, _ payload: UnsafePointer<UInt8>, _ length: Int) -> Int32
@_silgen_name("hop_v2_enter") private func sessionEnter(_ session: OpaquePointer, _ epoch: UInt64, _ screen: UInt64, _ x: UInt16, _ y: UInt16) -> Int32
@_silgen_name("hop_v2_release") private func sessionRelease(_ session: OpaquePointer, _ reason: UInt8) -> Int32
@_silgen_name("hop_v2_complete_release") private func sessionCompleteRelease(_ session: OpaquePointer, _ now: UInt64) -> Int32
@_silgen_name("hop_v2_request_leave") private func sessionRequestLeave(_ session: OpaquePointer, _ x: UInt16, _ y: UInt16) -> Int32
@_silgen_name("hop_v2_send_input") private func sessionInput(_ session: OpaquePointer, _ payload: UnsafePointer<UInt8>, _ length: Int) -> Int32
@_silgen_name("hop_v2_peer_screens") private func sessionScreenCount(_ session: OpaquePointer, _ output: UnsafeMutableRawPointer?, _ capacity: Int) -> Int
@_silgen_name("hop_v2_peer_screen_at") private func sessionScreenAt(_ session: OpaquePointer, _ index: Int, _ id: UnsafeMutablePointer<UInt64>, _ x: UnsafeMutablePointer<Int32>, _ y: UnsafeMutablePointer<Int32>, _ width: UnsafeMutablePointer<UInt32>, _ height: UnsafeMutablePointer<UInt32>) -> Int32
@_silgen_name("hop_v2_phase") private func sessionPhase(_ session: OpaquePointer) -> Int32
@_silgen_name("hop_v2_disconnect") private func sessionDisconnect(_ session: OpaquePointer)
@_silgen_name("hop_v2_free") private func sessionFree(_ session: OpaquePointer)
@_silgen_name("hop_v2_enable_clipboard") private func sessionEnableClipboard(_ session: OpaquePointer, _ text: Int32, _ png: Int32) -> Int32
@_silgen_name("hop_v2_clipboard_formats") private func sessionClipboardFormats(_ session: OpaquePointer) -> UInt64
@_silgen_name("hop_v2_clipboard_ready") private func sessionClipboardReady(_ session: OpaquePointer) -> Int32
@_silgen_name("hop_v2_clipboard_pause") private func sessionClipboardPause(_ session: OpaquePointer) -> Int32
@_silgen_name("hop_v2_clipboard_available") private func sessionClipboardAvailable(_ session: OpaquePointer) -> Int32
@_silgen_name("hop_v2_clipboard_initial_done") private func sessionClipboardInitialDone(_ session: OpaquePointer) -> Int32
@_silgen_name("hop_v2_clipboard_initial_received") private func sessionClipboardInitialReceived(_ session: OpaquePointer) -> Int32
@_silgen_name("hop_v2_clipboard_initial_drained") private func sessionClipboardInitialDrained(_ session: OpaquePointer) -> Int32
@_silgen_name("hop_v2_clipboard_start") private func sessionClipboardStart(_ session: OpaquePointer, _ id: UInt64, _ format: UInt8, _ total: UInt32) -> Int32
@_silgen_name("hop_v2_clipboard_chunk") private func sessionClipboardChunk(_ session: OpaquePointer, _ id: UInt64, _ bytes: UnsafePointer<UInt8>, _ count: Int) -> Int32

private final class V2SessionCallbacks {
    let enter: ([UInt8], UInt64) -> Bool
    let input: ([UInt8], UInt64) -> Bool
    let restore: () -> Bool
    let leave: (UInt64, UInt16, UInt16) -> Bool
    let clipboard: V2SessionBridge.ClipboardHooks?
    init(enter: @escaping ([UInt8], UInt64) -> Bool, input: @escaping ([UInt8], UInt64) -> Bool,
         restore: @escaping () -> Bool, leave: @escaping (UInt64, UInt16, UInt16) -> Bool, clipboard: V2SessionBridge.ClipboardHooks?) {
        self.enter = enter; self.input = input; self.restore = restore; self.leave = leave
        self.clipboard = clipboard
    }
}
private let v2EnterCallback: V2EventCallback = { context, bytes, count, epoch in
    guard let context, let bytes, (1...4096).contains(count) else { return 0 }
    return Unmanaged<V2SessionCallbacks>.fromOpaque(context).takeUnretainedValue().enter(Array(UnsafeBufferPointer(start: bytes, count: count)), epoch) ? 1 : 0
}
private let v2InputCallback: V2EventCallback = { context, bytes, count, epoch in
    guard let context, let bytes, (1...4096).contains(count) else { return 0 }
    return Unmanaged<V2SessionCallbacks>.fromOpaque(context).takeUnretainedValue().input(Array(UnsafeBufferPointer(start: bytes, count: count)), epoch) ? 1 : 0
}
private let v2RestoreCallback: V2RestoreCallback = { context in
    guard let context else { return 0 }
    return Unmanaged<V2SessionCallbacks>.fromOpaque(context).takeUnretainedValue().restore() ? 1 : 0
}
private let v2LeaveCallback: V2LeaveCallback = { context, screen, x, y in
    guard let context else { return 0 }
    return Unmanaged<V2SessionCallbacks>.fromOpaque(context).takeUnretainedValue().leave(screen, x, y) ? 1 : 0
}
private let v2ClipboardRevoke: V2ClipboardRevokeCallback = { context in
    guard let context else { return }
    Unmanaged<V2SessionCallbacks>.fromOpaque(context).takeUnretainedValue().clipboard?.revoke()
}
private let v2ClipboardSample: V2ClipboardSampleCallback = { context, out in
    guard let context, let out,
          let sequence = Unmanaged<V2SessionCallbacks>.fromOpaque(context).takeUnretainedValue().clipboard?.sample() else { return 0 }
    out.pointee = sequence
    return 1
}
private let v2ClipboardApply: V2ClipboardApplyCallback = { context, format, epoch, _, bytes, count, baseline in
    guard let context, (0...16 * 1024 * 1024).contains(count), count == 0 || bytes != nil,
          let hooks = Unmanaged<V2SessionCallbacks>.fromOpaque(context).takeUnretainedValue().clipboard else { return 0 }
    return hooks.apply(format, epoch, UnsafeBufferPointer(start: bytes, count: count), baseline) ? 1 : 0
}

/// V2 worker bridge. All methods and callbacks run on one owner
/// executor; callbacks must be prompt, non-reentrant and never wait on network.
/// Clipboard validation and system access belong on a separate executor.
final class V2SessionBridge {
    struct Screen: Equatable { let id: UInt64; let x, y: Int32; let width, height: UInt32 }
    enum Role: Int32 { case host = 1, receiver = 2 }
    struct ClipboardHooks {
        let revoke: () -> Void
        let sample: () -> UInt64?
        /// Borrowed bytes remain valid only during this callback.
        let apply: (UInt8, UInt64, UnsafeBufferPointer<UInt8>, UInt64) -> Bool
    }
    private let transport: V2TLSSocket
    private let callbacks: V2SessionCallbacks
    private var session: OpaquePointer?
    private init(transport: V2TLSSocket, callbacks: V2SessionCallbacks, session: OpaquePointer) {
        self.transport = transport; self.callbacks = callbacks; self.session = session
    }
    deinit { close() }
    static func start(transport: V2TLSSocket, role: Role, platform: Int32, sessionEpoch: UInt64,
                      localScreens: [UInt64], now: UInt64, requirePairing: Bool = false,
                      enter: @escaping ([UInt8], UInt64) -> Bool = { _,_ in true },
                      input: @escaping ([UInt8], UInt64) -> Bool = { _,_ in true },
                      restore: @escaping () -> Bool,
                      leave: @escaping (UInt64, UInt16, UInt16) -> Bool = { _,_,_ in false },
                      clipboard: ClipboardHooks? = nil) throws -> V2SessionBridge {
        guard let fingerprint = transport.authenticatedPeerFingerprint else { throw WireError.invalid("v2 session needs authenticated TLS") }
        let callbacks = V2SessionCallbacks(enter: enter, input: input, restore: restore, leave: leave, clipboard: clipboard)
        let user = Unmanaged.passUnretained(callbacks).toOpaque()
        let session = localScreens.withUnsafeBufferPointer {
            if clipboard != nil {
                return sessionCreateWithClipboard(role.rawValue, platform, sessionEpoch, $0.baseAddress, $0.count, 25,
                                                  v2EnterCallback, v2InputCallback, v2RestoreCallback, v2LeaveCallback,
                                                  v2ClipboardRevoke, v2ClipboardSample, v2ClipboardApply, user)
            }
            return sessionCreate(role.rawValue, platform, sessionEpoch, $0.baseAddress, $0.count, 25,
                                 v2EnterCallback, v2InputCallback, v2RestoreCallback, v2LeaveCallback, user)
        }
        guard let session else { throw WireError.invalid("Cannot create v2 session") }
        guard fingerprint.withCString({ sessionBind(session, $0) }) == 0,
              (clipboard == nil || sessionEnableClipboard(session, 1, 1) == 0),
              (!requirePairing || sessionRequirePairing(session) == 0), sessionStart(session, 1, now) == 0 else {
            sessionFree(session); throw WireError.invalid("Cannot start v2 session")
        }
        return V2SessionBridge(transport: transport, callbacks: callbacks, session: session)
    }
    var phase: Int32 { session.map { sessionPhase($0) } ?? -1 }
    var pairingConfirmed: Bool { session.map { sessionPairingConfirmed($0) == 1 } ?? false }
    /// True after a pump observes both the TLS write and session queue drained.
    /// Pairing persistence must wait for a pump after queuing PAIR_CONFIRM.
    private(set) var drainedWrites = false
    var needsWriteReadiness: Bool {
        transport.retryWriteWhenWritable || (!transport.hasPendingWrite && !drainedWrites)
    }
    /// Caller may confirm only after comparing both complete fingerprints.
    func confirmPairing() -> Bool {
        guard let session, transport.isAuthenticated else { return false }
        drainedWrites = false
        return sessionConfirmPairing(session) == 0
    }
    func cancelPairing() -> Bool {
        guard let session, transport.isAuthenticated else { return false }
        drainedWrites = false
        return sessionCancelPairing(session) == 0
    }
    func screens() throws -> [Screen] {
        guard let session, transport.isAuthenticated else { throw WireError.invalid("v2 session closed") }
        let count = sessionScreenCount(session, nil, 0)
        guard (0...64).contains(count) else { throw WireError.invalid("Invalid remote screen count") }
        return try (0..<count).map { index in
            var id: UInt64 = 0, x: Int32 = 0, y: Int32 = 0, width: UInt32 = 0, height: UInt32 = 0
            guard sessionScreenAt(session,index,&id,&x,&y,&width,&height) == 0 else { throw WireError.invalid("Invalid remote screen") }
            return Screen(id: id, x: x, y: y, width: width, height: height)
        }
    }
    func advertise(_ payload: [UInt8]) -> Bool {
        guard let session, transport.isAuthenticated, !payload.isEmpty else { return false }
        drainedWrites = false
        return payload.withUnsafeBufferPointer { sessionAdvertise(session,$0.baseAddress!,$0.count) == 0 }
    }
    func enter(epoch: UInt64, screen: UInt64, x: UInt16, y: UInt16) -> Bool {
        guard let session, transport.isAuthenticated else { return false }
        drainedWrites = false
        return sessionEnter(session,epoch,screen,x,y) == 0
    }
    func sendInput(_ payload: [UInt8]) -> Bool {
        guard let session, transport.isAuthenticated, !payload.isEmpty else { return false }
        drainedWrites = false
        return payload.withUnsafeBufferPointer { sessionInput(session,$0.baseAddress!,$0.count) == 0 }
    }
    func release(reason: UInt8 = 0) -> Bool {
        // A partially written input frame cannot be dropped from TLS. The
        // owner must close the connection if it cannot safely send RELEASE.
        guard let session, transport.isAuthenticated, !transport.hasPendingWrite else { return false }
        drainedWrites = false
        return sessionRelease(session,reason) == 0
    }
    func requestLeave(x: UInt16, y: UInt16) -> Bool {
        guard let session, transport.isAuthenticated else { return false }
        drainedWrites = false
        return sessionRequestLeave(session, x, y) == 0
    }
    var clipboardFormats: UInt64 { session.map(sessionClipboardFormats) ?? 0 }
    var clipboardAvailable: Bool { session.map { sessionClipboardAvailable($0) == 1 } ?? false }
    var clipboardInitialReceived: Bool { session.map { sessionClipboardInitialReceived($0) == 1 } ?? false }
    func readyClipboard() -> Bool { clipboardCommand(sessionClipboardReady) }
    func pauseClipboard() -> Bool {
        // A partially written record cannot be discarded from this TLS
        // stream. The caller closes rather than finishing a revoked transfer.
        guard !transport.hasPendingWrite else { return false }
        return clipboardCommand(sessionClipboardPause)
    }
    func finishInitialClipboard() -> Bool { clipboardCommand(sessionClipboardInitialDone) }
    func drainInitialClipboard() -> Bool { clipboardCommand(sessionClipboardInitialDrained) }
    func completeRelease(now: UInt64) -> Bool { clipboardCommand { sessionCompleteRelease($0, now) } }
    func startClipboard(id: UInt64, format: UInt8, total: UInt32) -> Bool {
        clipboardCommand { sessionClipboardStart($0, id, format, total) }
    }
    func sendClipboard(id: UInt64, bytes: UnsafeBufferPointer<UInt8>) -> Bool {
        guard let data = bytes.baseAddress, !bytes.isEmpty else { return false }
        return clipboardCommand { sessionClipboardChunk($0, id, data, bytes.count) }
    }
    func sendClipboardSlice(id: UInt64, data: Data, offset: Int) -> Int {
        guard offset >= 0, offset < data.count else { return 0 }
        return data.withUnsafeBytes { raw in
            let bytes = raw.bindMemory(to: UInt8.self)
            let count = min(bytes.count - offset, 16_340) // one TLS record with both frame headers
            let chunk = UnsafeBufferPointer(start: bytes.baseAddress!.advanced(by: offset), count: count)
            return sendClipboard(id: id, bytes: chunk) ? count : 0
        }
    }
    private func clipboardCommand(_ command: (OpaquePointer) -> Int32) -> Bool {
        guard let session, transport.isAuthenticated else { return false }
        drainedWrites = false
        return command(session) == 0
    }
    /// Bounded work per call. The owner drives this with socket readiness and
    /// monotonic deadlines; do not call from an event tap or UI paint callback.
    func step(now: UInt64) throws {
        drainedWrites = false
        guard let session, transport.isAuthenticated else { close(); throw WireError.invalid("v2 transport unavailable") }
        guard sessionTick(session,1,now) == 0 else { close(); throw WireError.invalid("v2 session timed out") }
        do {
            for _ in 0..<16 {
                if transport.hasPendingWrite {
                    switch try transport.flush() {
                    case .again: return
                    case .closed: throw WireError.invalid("v2 peer closed")
                    case .ready: break
                    }
                }
                var size = 0
                let status = sessionPull(session,1,nil,0,&size)
                if status == 0 { drainedWrites = true; break }
                guard status == 1 && (1...65564).contains(size) else { throw WireError.invalid("Invalid v2 outbound frame") }
                var bytes = [UInt8](repeating: 0, count: size), actual = 0
                let pulled = bytes.withUnsafeMutableBufferPointer { sessionPull(session,1,$0.baseAddress!,$0.count,&actual) }
                guard pulled == 1 && actual == size && transport.enqueue(bytes) else { throw WireError.invalid("Cannot queue v2 frame") }
                switch try transport.flush() {
                case .again: return
                case .closed: throw WireError.invalid("v2 peer closed")
                case .ready: break
                }
            }
            for _ in 0..<16 {
                switch try transport.read() {
                case .again: return
                case .closed: throw WireError.invalid("v2 peer closed")
                case .bytes(let bytes):
                    drainedWrites = false
                    guard bytes.withUnsafeBufferPointer({ sessionReceive(session,1,$0.baseAddress!,$0.count,now) }) == 0 else { throw WireError.invalid("Invalid v2 session data") }
                }
            }
        } catch { close(); throw error }
    }
    func close() {
        drainedWrites = false
        if let session { sessionDisconnect(session); sessionFree(session); self.session = nil }
        transport.close()
    }
}
