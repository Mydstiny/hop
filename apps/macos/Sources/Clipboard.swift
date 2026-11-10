import AppKit
import ImageIO

/// Internal clipboard adapter. It is not instantiated by the v1 app: callers
/// must first provide an authenticated encrypted peer and current focus epoch.
enum HopClipboardPayload: Equatable {
    case text(Data)
    case png(Data)
}
enum HopClipboardRead {
    case payload(HopClipboardPayload)
    case unsupported
    case retry
}
protocol HopClipboardBackend: AnyObject {
    var changeCount: Int { get }
    func read(expected: Int) -> HopClipboardRead
    func write(_ payload: HopClipboardPayload) -> Int?
    func write(_ payload: HopClipboardPayload, permitted: () -> Bool) -> Int?
}
extension HopClipboardBackend {
    func write(_ payload: HopClipboardPayload, permitted: () -> Bool) -> Int? {
        permitted() ? write(payload) : nil
    }
}

enum HopClipboardValidation {
    static let maxText = 2 * 1024 * 1024
    static let maxImage = 16 * 1024 * 1024
    static let maxPixels: Int64 = 40_000_000
    static func text(_ data: Data) -> Bool {
        guard data.count <= maxText, let string = String(data: data, encoding: .utf8) else { return false }
        // Path-looking plain text is still text. File objects are rejected by
        // the pasteboard adapter before selecting a text representation.
        return !string.contains("\0")
    }
    static func png(_ data: Data) -> Bool {
        guard !data.isEmpty && data.count <= maxImage,
              let source = CGImageSourceCreateWithData(data as CFData, nil),
              CGImageSourceGetType(source) as String? == "public.png",
              CGImageSourceGetCount(source) == 1,
              let props = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any],
              let width = (props[kCGImagePropertyPixelWidth] as? NSNumber)?.int64Value,
              let height = (props[kCGImagePropertyPixelHeight] as? NSNumber)?.int64Value,
              width > 0 && height > 0 && width <= 16384 && height <= 16384 && width <= maxPixels / height,
              CGImageSourceGetStatus(source) == .statusComplete,
              CGImageSourceCreateImageAtIndex(source, 0, nil) != nil else { return false }
        return true
    }
    // Many macOS applications expose copied images as TIFF. Decode only a
    // single bounded image and re-encode to the common wire format.
    static func tiffToPNG(_ data: Data) -> Data? {
        guard !data.isEmpty && data.count <= maxImage,
              let source = CGImageSourceCreateWithData(data as CFData, nil),
              CGImageSourceGetType(source) as String? == "public.tiff",
              CGImageSourceGetCount(source) == 1,
              let props = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any],
              let width = (props[kCGImagePropertyPixelWidth] as? NSNumber)?.int64Value,
              let height = (props[kCGImagePropertyPixelHeight] as? NSNumber)?.int64Value,
              width > 0 && height > 0 && width <= 16384 && height <= 16384 && width <= maxPixels / height,
              CGImageSourceGetStatus(source) == .statusComplete,
              let image = CGImageSourceCreateImageAtIndex(source, 0, nil) else { return nil }
        let encoded = NSMutableData()
        guard let destination = CGImageDestinationCreateWithData(encoded, "public.png" as CFString, 1, nil) else { return nil }
        CGImageDestinationAddImage(destination, image, nil)
        guard CGImageDestinationFinalize(destination), encoded.length <= maxImage else { return nil }
        let result = encoded as Data
        return png(result) ? result : nil
    }
    static func valid(_ payload: HopClipboardPayload) -> Bool {
        switch payload { case .text(let data): return text(data); case .png(let data): return png(data) }
    }
}

/// One instance per local board, driven on a single serial executor. The caller
/// controls all timers and must switch off before the secure session/focus ends.
final class HopClipboardSync {
    private let backend: HopClipboardBackend
    private var enabled = false, verified = false, failed = false
    private var peer = ""
    private var epoch: UInt64 = 0
    private var observed = 0
    private var initialPending = false
    init(backend: HopClipboardBackend) { self.backend = backend }
    func configure(enabled: Bool, verified: Bool, peer: String, epoch: UInt64,
                   initialSnapshot: Bool = false, baseline: Int? = nil) {
        if self.enabled != enabled || self.verified != verified || self.peer != peer || self.epoch != epoch {
            // Sample only the sequence here. The active host can request one
            // initial snapshot later on the serialized clipboard actor.
            observed = enabled && verified && !peer.isEmpty && epoch != 0 ? (baseline ?? backend.changeCount) : 0
            initialPending = enabled && verified && !peer.isEmpty && epoch != 0 && initialSnapshot
            failed = false
        }
        self.enabled = enabled; self.verified = verified; self.peer = peer; self.epoch = epoch
    }
    var active: Bool { enabled && verified && !failed && !peer.isEmpty && epoch != 0 }
    // The session owner must keep polling only after READY; a consumed snapshot
    // (including unsupported contents) permits queuing INITIAL_DONE.
    var initialSnapshotPending: Bool { initialPending }
    var sampledChangeCount: Int? { active ? observed : nil }
    func poll(epoch expectedEpoch: UInt64, permitted: () -> Bool = { true }) -> HopClipboardPayload? {
        guard active && expectedEpoch == epoch && permitted() else { return nil }
        let current = backend.changeCount
        guard (current != observed || initialPending) && permitted() else { return nil }
        switch backend.read(expected: current) {
        case .retry: return nil
        case .unsupported: observed = current; initialPending = false; return nil
        case .payload(let value):
            guard permitted(), backend.changeCount == current else { return nil }
            observed = current; initialPending = false
            return HopClipboardValidation.valid(value) ? value : nil
        }
    }
    @discardableResult func apply(_ value: HopClipboardPayload, from target: String, epoch expectedEpoch: UInt64,
                                  expectedChangeCount: Int? = nil, permitted: () -> Bool = { true }) -> Bool {
        guard active && target == peer && expectedEpoch == epoch && permitted() && HopClipboardValidation.valid(value),
              backend.changeCount == observed, expectedChangeCount.map({ $0 == observed }) ?? true else { return false } // local change wins a conflict
        var denied = false
        guard let written = backend.write(value, permitted: {
            denied = !permitted() || backend.changeCount != observed
            return !denied
        }) else { if !denied { failed = true }; return false }
        observed = written; initialPending = false // suppress the notification produced by our own write
        return true
    }
}

final class MacPasteboardBackend: HopClipboardBackend {
    private let board: NSPasteboard
    init(board: NSPasteboard = .general) { self.board = board }
    var changeCount: Int { board.changeCount }
    static func containsFileObjects(_ types: [NSPasteboard.PasteboardType]) -> Bool {
        types.contains(.fileURL) || types.contains(NSPasteboard.PasteboardType("NSFilenamesPboardType"))
    }
    func read(expected: Int) -> HopClipboardRead {
        guard board.changeCount == expected else { return .retry }
        let items = board.pasteboardItems ?? []
        guard items.count == 1, let item = items.first else { return .unsupported }
        guard !Self.containsFileObjects(item.types) else { return .unsupported }
        let payload: HopClipboardPayload
        if item.types.contains(.png), let data = item.data(forType: .png) { payload = .png(data) }
        else if item.types.contains(.tiff), let data = item.data(forType: .tiff), let png = HopClipboardValidation.tiffToPNG(data) { payload = .png(png) }
        else if item.types.contains(.string), let value = item.string(forType: .string), let data = value.data(using: .utf8) { payload = .text(data) }
        else { return .unsupported }
        guard board.changeCount == expected else { return .retry }
        return .payload(payload)
    }
    func write(_ payload: HopClipboardPayload) -> Int? {
        write(payload, permitted: { true })
    }
    func write(_ payload: HopClipboardPayload, permitted: () -> Bool) -> Int? {
        guard HopClipboardValidation.valid(payload) else { return nil }
        let item = NSPasteboardItem()
        let prepared: Bool
        switch payload {
        case .text(let data): prepared = String(data: data, encoding: .utf8).map { item.setString($0, forType: .string) } ?? false
        case .png(let data): prepared = item.setData(data, forType: .png)
        }
        guard prepared, permitted() else { return nil }
        board.clearContents()
        return board.writeObjects([item]) ? board.changeCount : nil
    }
}
