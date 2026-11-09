import AppKit

/// In-memory PRESENTATION fixture only. No AppController/InputCapture is created;
/// none of these actions start listeners, touch permissions/storage, or inject input.
private final class MemoryPopoverModel: PopoverModel {
    var presentation = PopoverPresentation(status: "UI TEST FIXTURE · sharing disabled")
    var actions: [String] = []
    var popoverPresentation: PopoverPresentation { presentation }
    func setEnabled(_ value: Bool) { actions.append("enable:\(value)"); presentation.enabled = value }
    func setDirection(_ value: UInt8) { actions.append("direction:\(value)"); presentation.direction = value }
    func beginPairing() { actions.append("pair"); presentation.pairingAllowed = true }
    func unpair() { actions.append("unpair"); presentation.pairedName = nil }
    func confirmPairing() { actions.append("confirm"); presentation.localConfirmed = true }
    func rejectPairing() { actions.append("reject"); presentation.code = nil; presentation.pairingAllowed = false }
    func requestPermissions() { actions.append("permissions") }
}

@main struct PopoverTests {
    static var checks = 0
    static var failures: [String] = []
    static func expect(_ condition: @autoclosure () -> Bool, _ reason: String) {
        checks += 1
        if !condition() { failures.append(reason) }
    }
    static func descendants(_ view: NSView) -> [NSView] {
        let children = (view as? NSStackView)?.arrangedSubviews ?? view.subviews
        return children.flatMap { [$0] + descendants($0) }
    }
    static func button(_ root: NSView, title: String) -> NSButton {
        guard let result = descendants(root).compactMap({ $0 as? NSButton }).first(where: { $0.title == title }) else {
            fatalError("Missing native button: \(title)")
        }
        return result
    }
    static func fitsHorizontally(_ child: NSView, root: NSView) -> Bool {
        let frame = child.convert(child.bounds, to: root)
        guard let parent = child.superview else { return false }
        // Native constraints align alignment rects, not decorated control frames
        // (NSLayoutConstraint.h). NSTextField frames extend 2pt beyond that rect.
        let alignment = parent.convert(child.alignmentRect(forFrame: child.frame), to: root)
        return frame.minX >= 0 && frame.maxX <= root.bounds.width && alignment.minX >= 19 && alignment.maxX <= 341
    }
    static func render(_ controller: PopoverController, name: String, output: URL) throws {
        controller.refresh()
        let root = controller.view
        root.layoutSubtreeIfNeeded()
        expect(root.bounds.width == 360, "\(name): fixed native panel width")
        expect(root.bounds.height == controller.preferredContentSize.height, "\(name): content/preferred size agreement")
        expect(root.bounds.height >= 360 && root.bounds.height <= 640, "\(name): menu height, not a full-screen column")
        expect(root.window == nil, "\(name): no main/popover window created for offscreen test")
        guard let stack = descendants(root).compactMap({ $0 as? NSStackView }).first else { fatalError("Missing production NSStackView") }
        var previous: NSRect?
        for child in stack.arrangedSubviews where !child.isHidden {
            let bounds = child.convert(child.bounds, to: root)
            expect(bounds.width > 0 && bounds.height > 0, "\(name): visible zero-size view \(child)")
            expect(fitsHorizontally(child, root: root), "\(name): horizontal frame clipping or alignment-margin violation \(bounds)")
            expect(bounds.minY >= 17 && bounds.maxY <= root.bounds.height - 17, "\(name): vertical clipping \(bounds) within \(root.bounds)")
            if let previous { expect(!previous.insetBy(dx: 0, dy: 0.5).intersects(bounds.insetBy(dx: 0, dy: 0.5)), "\(name): arranged views overlap") }
            previous = bounds
        }
        root.appearance = NSAppearance(named: .aqua)
        var image: NSBitmapImageRep?
        root.appearance?.performAsCurrentDrawingAppearance {
            root.wantsLayer = true
            root.layer?.backgroundColor = NSColor.windowBackgroundColor.cgColor
            image = root.bitmapImageRepForCachingDisplay(in: root.bounds)
            if let image { root.cacheDisplay(in: root.bounds, to: image) }
        }
        guard let image, let png = image.representation(using: .png, properties: [:]) else { fatalError("Native offscreen render failed") }
        try png.write(to: output.appendingPathComponent("\(name).png"))
    }
    static func main() throws {
        guard CommandLine.arguments.count == 2 else { fatalError("Usage: popover-tests OUTPUT_DIRECTORY") }
        let output = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true).standardizedFileURL
        try FileManager.default.createDirectory(at: output, withIntermediateDirectories: true)
        let app = NSApplication.shared
        expect(app.setActivationPolicy(.accessory), "Native accessory activation policy")
        let initialWindows = app.windows.count
        let model = MemoryPopoverModel()
        let panel = PopoverController(controller: model)
        panel.loadView()
        let root = panel.view
        expect(app.mainWindow == nil, "No initial main window")
        expect(!button(root, title: "Pair a computer…").isHidden, "Unpaired exposes Pair")
        expect(button(root, title: "Unpair").isHidden, "Unpaired hides Unpair")
        expect(button(root, title: "Codes match — Confirm").isHidden, "Unpaired hides confirmation")
        try render(panel, name: "unpaired", output: output)
        let title = descendants(root).compactMap({ $0 as? NSTextField }).first(where: { $0.stringValue == "Hop" })!
        let original = title.frame
        title.setFrameOrigin(NSPoint(x: -5, y: original.minY))
        expect(!fitsHorizontally(title, root: root), "Negative probe still rejects real out-of-bounds content")
        title.frame = original

        let enable = button(root, title: "Enable keyboard & mouse sharing")
        enable.performClick(nil)
        expect(model.actions.last == "enable:true", "Checkbox delegates to memory action, not capture")
        expect(enable.state == .on, "Enable presentation refresh")
        enable.performClick(nil)
        expect(model.actions.last == "enable:false", "Disable delegates correctly")
        let direction = descendants(root).compactMap { $0 as? NSSegmentedControl }.first!
        direction.selectedSegment = 0
        _ = direction.sendAction(direction.action, to: direction.target)
        expect(model.actions.last == "direction:0", "Left direction action")
        button(root, title: "Pair a computer…").performClick(nil)
        panel.refresh()
        expect(model.actions.last == "pair", "Pair action without sockets")
        expect(!button(root, title: "Waiting for pairing request…").isEnabled, "Waiting disables repeated Pair")

        model.presentation = PopoverPresentation(status: "UI TEST FIXTURE · compare codes", code: 123456,
                                                peerName: "HarmonyOS PC (test)", peerConfirmed: true, pairingAllowed: true)
        try render(panel, name: "pairing", output: output)
        expect(!button(root, title: "Codes match — Confirm").isHidden, "Pairing exposes Confirm")
        expect(button(root, title: "Waiting for pairing request…").isHidden, "Challenge hides Pair")
        expect(descendants(root).compactMap({ $0 as? NSTextField }).contains(where: { $0.stringValue == "123456" && !$0.isHidden }), "Exact six-digit code rendered")
        button(root, title: "Codes match — Confirm").performClick(nil)
        panel.refresh()
        expect(model.actions.last == "confirm", "Confirmation only delegates")
        expect(!button(root, title: "Confirmed — waiting for peer").isEnabled, "Confirmed is idempotently disabled")
        try render(panel, name: "confirmed", output: output)
        button(root, title: "Cancel pairing").performClick(nil)
        panel.refresh()
        expect(model.actions.last == "reject", "Cancel action without transport")
        expect(button(root, title: "Confirmed — waiting for peer").isHidden, "Cancel hides confirmation")

        model.presentation = PopoverPresentation(status: "UI TEST FIXTURE · paired, disabled", pairedName: "HarmonyOS PC (test)",
                                                direction: 0, permissionGranted: true)
        try render(panel, name: "paired", output: output)
        expect(!button(root, title: "Unpair").isHidden, "Paired exposes Unpair")
        expect(button(root, title: "Pair a computer…").isHidden, "Paired hides Pair")
        button(root, title: "Unpair").performClick(nil)
        panel.refresh()
        expect(model.actions.last == "unpair", "Unpair only changes memory, no Keychain")

        model.presentation = PopoverPresentation(status: "UI TEST FIXTURE · " + String(repeating: "长状态 / long network error; ", count: 8),
                                                pairedName: "Long fixture device name 1234567")
        try render(panel, name: "long-status", output: output)
        expect(app.mainWindow == nil && app.windows.count == initialWindows, "All cases create no windows")
        expect(app.activationPolicy() == .accessory, "No regular/Dock activation policy")
        expect(!model.actions.contains("permissions"), "No permission request or settings action executed")
        let report: [String: Any] = ["timestamp": ISO8601DateFormatter().string(from: Date()), "checks": checks,
                                     "failures": failures, "passing": failures.isEmpty,
                                     "scope": "Offscreen actual production AppKit content; memory model only",
                                     "productAppLaunched": false, "visibleWindowsCreated": app.windows.contains(where: { $0.isVisible }),
                                     "runtimeInputVerified": false, "permissionQueriesOrPrompts": false]
        try JSONSerialization.data(withJSONObject: report, options: [.prettyPrinted, .sortedKeys])
            .write(to: output.appendingPathComponent("report.json"))
        if !failures.isEmpty {
            for failure in failures { fputs("FAIL: \(failure)\n", stderr) }
            exit(1)
        }
        print("PASS: \(checks) production AppKit popover layout/action checks; 5 offscreen fixture PNGs")
        print("No product AppController, capture/tap/cursor APIs, listeners, Keychain, permission probes/prompts, or visible windows were created.")
        print("Boundary: offscreen production content view only, not actual menu-bar popup interaction or two-device runtime proof.")
    }
}
