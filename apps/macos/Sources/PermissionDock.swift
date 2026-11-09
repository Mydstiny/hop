import AppKit
import CoreGraphics

/// Floater parked just below the System Settings window, with an arrow pointing up
/// into its list. Only the icon starts a file drag; the window itself does not move.
final class PermissionDock {
    static let shared = PermissionDock()
    private var panel: NSPanel?
    private var detail: NSTextField?
    private var timer: Timer?
    private init() {}

    func showIfNeeded() {
        if InputCapture.permissionGranted { hide(); return }
        show()
    }

    func closeIfGranted() {
        if InputCapture.permissionGranted { hide() }
    }

    func show() {
        if InputCapture.permissionGranted { hide(); return }
        let panel = ensurePanel()
        place(panel)
        panel.orderFrontRegardless()
        if settingsWindowFrame() == nil { openPrivacy("Privacy_ListenEvent") }
        if timer == nil {
            let timer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
                self?.followSettings()
            }
            RunLoop.main.add(timer, forMode: .common)
            self.timer = timer
        }
    }

    private func followSettings() {
        detail?.stringValue = InputCapture.permissionReport + "。可以创建后浮窗会消失。"
        if InputCapture.permissionGranted { hide(); return }
        if let panel, panel.isVisible { place(panel) }
    }

    private func hide() {
        timer?.invalidate(); timer = nil
        panel?.orderOut(nil)
    }

    private func ensurePanel() -> NSPanel {
        if let panel { return panel }
        let width: CGFloat = 520
        let cardHeight: CGFloat = 104
        let arrowHeight: CGFloat = 22
        let size = NSSize(width: width, height: cardHeight + arrowHeight)
        let panel = NSPanel(contentRect: NSRect(origin: .zero, size: size),
                            styleMask: [.borderless, .nonactivatingPanel, .fullSizeContentView],
                            backing: .buffered, defer: false)
        panel.level = .statusBar
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary]
        panel.isFloatingPanel = true
        panel.hidesOnDeactivate = false
        panel.isMovable = false
        panel.isMovableByWindowBackground = false
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = false
        panel.title = "Hop permissions"
        let root = PassThroughView(frame: NSRect(origin: .zero, size: size))
        let card = FixedContentView(frame: NSRect(x: 0, y: 0, width: width, height: cardHeight))
        card.material = .hudWindow
        card.state = .active
        card.wantsLayer = true
        card.layer?.cornerRadius = 16
        card.layer?.masksToBounds = true
        let shadow = NSShadow()
        shadow.shadowColor = NSColor.black.withAlphaComponent(0.35)
        shadow.shadowOffset = NSSize(width: 0, height: -3)
        shadow.shadowBlurRadius = 14
        card.shadow = shadow
        root.addSubview(card)
        let arrow = UpArrowView(frame: NSRect(x: width / 2 - 18, y: cardHeight - 2, width: 36, height: arrowHeight + 2))
        root.addSubview(arrow)
        let icon = AppIconDragView(frame: NSRect(x: 16, y: 16, width: 72, height: 72))
        icon.fileURL = Bundle.main.bundleURL
        card.addSubview(icon)
        let title = label("按住绿色图标，沿箭头向上拖进列表", size: 14, weight: .semibold, color: .labelColor)
        title.frame = NSRect(x: 104, y: 68, width: 280, height: 20)
        card.addSubview(title)
        let detail = NSTextField(wrappingLabelWithString: "正在检查两项权限。")
        self.detail = detail
        detail.font = .systemFont(ofSize: 11)
        detail.textColor = .secondaryLabelColor
        detail.frame = NSRect(x: 104, y: 14, width: 280, height: 52)
        detail.isEditable = false
        detail.isSelectable = false
        detail.drawsBackground = false
        card.addSubview(detail)
        let access = NSButton(title: "设备控制", target: self, action: #selector(openAccessibility))
        access.bezelStyle = .rounded
        access.frame = NSRect(x: 392, y: 54, width: 112, height: 26)
        card.addSubview(access)
        let listen = NSButton(title: "输入监控", target: self, action: #selector(openInputMonitoring))
        listen.bezelStyle = .rounded
        listen.frame = NSRect(x: 392, y: 20, width: 112, height: 26)
        card.addSubview(listen)
        panel.contentView = root
        self.panel = panel
        return panel
    }

    private func label(_ text: String, size: CGFloat, weight: NSFont.Weight, color: NSColor) -> NSTextField {
        let field = NSTextField(labelWithString: text)
        field.font = .systemFont(ofSize: size, weight: weight)
        field.textColor = color
        return field
    }

    private func place(_ panel: NSPanel) {
        let size = panel.frame.size
        let settings = settingsWindowFrame()
        let screen = settings.flatMap { frame in NSScreen.screens.first { $0.frame.intersects(frame) } } ?? NSScreen.main
        let visible = screen?.visibleFrame ?? NSRect(x: 0, y: 0, width: 1280, height: 800)
        let host = settings ?? visible
        // Below the settings window when it fits. Otherwise sit on the bottom edge,
        // still fully on screen. Using the visible frame as the host used to push it off.
        var origin = NSPoint(x: host.midX - size.width / 2, y: host.minY - size.height - 36)
        if origin.y < visible.minY + 8 { origin.y = visible.minY + 12 }
        let minX = max(visible.minX + 8, host.minX)
        let maxX = min(visible.maxX - 8, host.maxX)
        if maxX - minX >= size.width {
            origin.x = min(max(origin.x, minX), maxX - size.width)
        }
        panel.setFrameOrigin(origin)
    }

    /// CG window bounds use a top-left origin on the primary display. Convert to AppKit points.
    private func settingsWindowFrame() -> NSRect? {
        guard let info = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID) as? [[String: Any]] else { return nil }
        let primary = NSScreen.screens.first { $0.frame.origin == .zero } ?? NSScreen.screens.first
        let primaryHeight = primary?.frame.height ?? 0
        var best: NSRect?
        var bestArea: CGFloat = 0
        for window in info {
            let owner = window[kCGWindowOwnerName as String] as? String ?? ""
            guard owner == "System Settings" || owner == "系统设置" else { continue }
            guard (window[kCGWindowLayer as String] as? Int ?? 0) == 0 else { continue }
            guard let bounds = window[kCGWindowBounds as String] as? [String: Any],
                  let rawX = (bounds["X"] as? NSNumber)?.doubleValue,
                  let rawY = (bounds["Y"] as? NSNumber)?.doubleValue,
                  let rawWidth = (bounds["Width"] as? NSNumber)?.doubleValue,
                  let rawHeight = (bounds["Height"] as? NSNumber)?.doubleValue,
                  rawWidth > 200, rawHeight > 200 else { continue }
            let width = CGFloat(rawWidth), height = CGFloat(rawHeight)
            let rect = NSRect(x: CGFloat(rawX), y: primaryHeight - CGFloat(rawY) - height, width: width, height: height)
            let area = width * height
            if area > bestArea { best = rect; bestArea = area }
        }
        return best
    }

    @objc private func openAccessibility() { openPrivacy("Privacy_Accessibility"); if let panel { place(panel) } }
    @objc private func openInputMonitoring() { openPrivacy("Privacy_ListenEvent"); if let panel { place(panel) } }

    private func openPrivacy(_ anchor: String) {
        guard let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?\(anchor)") else { return }
        NSWorkspace.shared.open(url)
    }
}

private final class PassThroughView: NSView {
    override var mouseDownCanMoveWindow: Bool { false }
}

private final class FixedContentView: NSVisualEffectView {
    override var mouseDownCanMoveWindow: Bool { false }
}

private final class UpArrowView: NSView {
    override var mouseDownCanMoveWindow: Bool { false }
    override var isOpaque: Bool { false }
    override func draw(_ dirtyRect: NSRect) {
        let path = NSBezierPath()
        path.move(to: NSPoint(x: bounds.midX, y: bounds.maxY - 1))
        path.line(to: NSPoint(x: bounds.minX + 1, y: bounds.minY))
        path.line(to: NSPoint(x: bounds.maxX - 1, y: bounds.minY))
        path.close()
        NSColor(srgbRed: 0.09, green: 0.45, blue: 0.32, alpha: 1).setFill()
        path.fill()
    }
}

private final class AppIconDragView: NSView, NSDraggingSource {
    var fileURL: URL?
    private var dragging = false
    override var mouseDownCanMoveWindow: Bool { false }
    override func draw(_ dirtyRect: NSRect) {
        let icon = HopMark.image(side: bounds.width)
        icon.draw(in: bounds, from: .zero, operation: .sourceOver, fraction: 1)
    }
    override func mouseDown(with event: NSEvent) { dragging = false }
    override func mouseDragged(with event: NSEvent) {
        guard !dragging, let fileURL else { return }
        dragging = true
        let writer = AppFileWriter(url: fileURL)
        let item = NSDraggingItem(pasteboardWriter: writer)
        item.setDraggingFrame(bounds, contents: HopMark.image(side: 128))
        beginDraggingSession(with: [item], event: event, source: self)
    }
    override func mouseUp(with event: NSEvent) { dragging = false }
    func draggingSession(_ session: NSDraggingSession, sourceOperationMaskFor context: NSDraggingContext) -> NSDragOperation {
        context == .outsideApplication ? [.copy, .link] : .copy
    }
}

private final class AppFileWriter: NSObject, NSPasteboardWriting {
    let url: URL
    init(url: URL) { self.url = url }
    func writableTypes(for pasteboard: NSPasteboard) -> [NSPasteboard.PasteboardType] {
        [.fileURL, NSPasteboard.PasteboardType("NSFilenamesPboardType")]
    }
    func pasteboardPropertyList(forType type: NSPasteboard.PasteboardType) -> Any? {
        if type == .fileURL { return url.absoluteString }
        if type.rawValue == "NSFilenamesPboardType" { return [url.path] }
        return nil
    }
}

enum HopMark {
    static func image(side: CGFloat) -> NSImage {
        let image = NSImage(size: NSSize(width: side, height: side), flipped: false) { rect in
            let inset = rect.insetBy(dx: side * 0.04, dy: side * 0.04)
            let plate = NSBezierPath(roundedRect: inset, xRadius: side * 0.22, yRadius: side * 0.22)
            NSColor(srgbRed: 0.09, green: 0.45, blue: 0.32, alpha: 1).setFill()
            plate.fill()
            let stem = side * 0.16
            let left = rect.midX - side * 0.28
            let right = rect.midX + side * 0.12
            let bottom = rect.midY - side * 0.30
            let top = rect.midY + side * 0.30
            let bar = NSBezierPath()
            bar.appendRect(NSRect(x: left, y: bottom, width: stem, height: top - bottom))
            bar.appendRect(NSRect(x: right, y: bottom, width: stem, height: top - bottom))
            bar.appendRect(NSRect(x: left, y: rect.midY - stem / 2, width: right + stem - left, height: stem))
            NSColor.white.setFill()
            bar.fill()
            return true
        }
        image.isTemplate = false
        return image
    }
}
