import Foundation
import AppKit
import ImageIO

private final class FakeBoard: HopClipboardBackend {
    var sequence = 10, reads = 0, writes = 0, probes = 0
    var next: HopClipboardRead = .unsupported
    var failWrite = false
    var raceRead = false
    var changeCount: Int { probes += 1; return sequence }
    func read(expected: Int) -> HopClipboardRead { reads += 1; if raceRead { sequence += 1 }; return expected == sequence ? next : .retry }
    func write(_ payload: HopClipboardPayload) -> Int? { writes += 1; if failWrite { return nil }; sequence += 1; return sequence }
}
@main struct ClipboardTests {
    static func main() {
        var checks = 0
        func check(_ condition: Bool, _ label: String) { checks += 1; if !condition { fputs("FAIL clipboard: \(label)\n",stderr); exit(1) } }
        let board = FakeBoard(), sync = HopClipboardSync(backend: board)
        let hello = HopClipboardPayload.text(Data("你好 Hop".utf8))
        sync.configure(enabled: false, verified: false, peer: "", epoch: 0)
        check(!sync.active && board.probes == 0 && sync.poll(epoch: 1) == nil && board.reads == 0, "inactive does not access pasteboard")
        sync.configure(enabled: true, verified: false, peer: "peer-A", epoch: 1)
        check(!sync.active && board.probes == 0, "unverified peer never accesses pasteboard")
        sync.configure(enabled: true, verified: true, peer: "peer-A", epoch: 1)
        check(sync.active && sync.poll(epoch: 1) == nil && board.reads == 0, "activation samples version without reading stale contents")
        board.sequence += 1; board.next = .payload(hello)
        check(sync.poll(epoch: 2) == nil && board.reads == 0, "stale epoch cannot read")
        check(sync.poll(epoch: 1) == hello && board.reads == 1, "changed valid text emitted once")
        check(sync.poll(epoch: 1) == nil && board.reads == 1, "duplicate sequence ignored")
        check(!sync.apply(hello, from: "peer-B", epoch: 1) && board.writes == 0, "wrong peer rejected")
        check(!sync.apply(hello, from: "peer-A", epoch: 2) && board.writes == 0, "stale remote epoch rejected")
        check(sync.apply(hello, from: "peer-A", epoch: 1) && board.writes == 1, "trusted current peer writes")
        check(sync.poll(epoch: 1) == nil && board.reads == 1, "own write does not echo")
        board.sequence += 1
        check(!sync.apply(hello, from: "peer-A", epoch: 1) && board.writes == 1, "unobserved local change wins conflict")
        let pathText = HopClipboardPayload.text(Data("/Users/example/file".utf8))
        board.next = .payload(pathText)
        check(sync.poll(epoch: 1) == pathText, "plain text path is preserved")
        check(HopClipboardValidation.text(Data("C:\\Users\\example\nfile://example".utf8)), "path-looking strings are plain text")
        check(MacPasteboardBackend.containsFileObjects([.string, .fileURL]), "file object rejected even alongside text")
        check(MacPasteboardBackend.containsFileObjects([.string, NSPasteboard.PasteboardType("NSFilenamesPboardType")]), "legacy file list rejected even alongside text")
        check(!MacPasteboardBackend.containsFileObjects([.string]), "ordinary plain text remains eligible")
        check(!HopClipboardValidation.text(Data([0xff])) && !HopClipboardValidation.text(Data([0x61,0,0x62])), "invalid UTF-8 and embedded NUL rejected")
        check(!HopClipboardValidation.text(Data(repeating: 65, count: HopClipboardValidation.maxText+1)), "text limit enforced")
        check(!HopClipboardValidation.png(Data([0x89,0x50,0x4e,0x47])), "invalid PNG rejected")
        let bitmap = CGContext(data: nil, width: 1, height: 1, bitsPerComponent: 8, bytesPerRow: 4,
                               space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
        let encoded = NSMutableData()
        let destination = CGImageDestinationCreateWithData(encoded, "public.png" as CFString, 1, nil)!
        CGImageDestinationAddImage(destination, bitmap.makeImage()!, nil)
        check(CGImageDestinationFinalize(destination), "test PNG created in memory")
        let png = HopClipboardPayload.png(encoded as Data)
        check(HopClipboardValidation.valid(png), "valid PNG accepted")
        let tiff = NSMutableData()
        let tiffDestination = CGImageDestinationCreateWithData(tiff, "public.tiff" as CFString, 1, nil)!
        CGImageDestinationAddImage(tiffDestination, bitmap.makeImage()!, nil)
        check(CGImageDestinationFinalize(tiffDestination), "test TIFF created in memory")
        check(HopClipboardValidation.tiffToPNG(tiff as Data).map(HopClipboardValidation.png) == true, "TIFF converts to valid PNG")
        check(HopClipboardValidation.tiffToPNG(encoded as Data) == nil, "non-TIFF rejected by TIFF converter")
        board.sequence += 1; board.next = .payload(png); board.raceRead = true
        check(sync.poll(epoch: 1) == nil, "clipboard changes during read are rejected")
        board.raceRead = false
        check(sync.poll(epoch: 1) == png, "stable image can be read on next poll")
        board.failWrite = true
        check(!sync.apply(hello, from: "peer-A", epoch: 1) && !sync.active, "write failure pauses sync")
        sync.configure(enabled: false, verified: false, peer: "", epoch: 0)
        sync.configure(enabled: true, verified: true, peer: "peer-B", epoch: 3)
        check(sync.active && sync.poll(epoch: 3) == nil, "new focus re-baselines and may recover")
        let initialBoard = FakeBoard(), initialSync = HopClipboardSync(backend: initialBoard)
        initialBoard.next = .payload(hello)
        initialSync.configure(enabled: true, verified: true, peer: "peer-C", epoch: 4, initialSnapshot: true)
        check(initialBoard.reads == 0 && initialSync.initialSnapshotPending, "initial snapshot does not read during activation")
        check(initialSync.poll(epoch: 4) == hello && initialBoard.reads == 1 && !initialSync.initialSnapshotPending, "active host reads one existing clipboard snapshot")
        check(initialSync.poll(epoch: 4) == nil && initialBoard.reads == 1, "initial snapshot is sent once")
        let targetBoard = FakeBoard(), targetSync = HopClipboardSync(backend: targetBoard)
        targetSync.configure(enabled: true, verified: true, peer: "peer-D", epoch: 5)
        targetBoard.sequence += 1; targetBoard.next = .payload(pathText)
        check(!targetSync.apply(hello, from: "peer-D", epoch: 5, expectedChangeCount: 10) && targetBoard.writes == 0,
              "target copy after focus is not overwritten by initial snapshot")
        check(targetSync.poll(epoch: 5) == pathText, "target's local change remains available to send")
        print("PASS: \(checks) Mac clipboard policy checks using memory backend; no system pasteboard access")
    }
}
