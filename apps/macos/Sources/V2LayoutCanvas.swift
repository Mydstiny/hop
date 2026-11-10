import AppKit

/// Native, flipped layout canvas. Drag a computer as a unit, or expand it
/// into displays. Coordinates are snapped in the same space used by routing.
final class V2LayoutCanvas: NSView {
    private struct Card {
        let name: String
        let indices: [Int]
        let frame: CGRect
        let local: Bool
    }

    private var saved: V2DesktopLayout
    private let anchor: V2DesktopLayout.Key?
    private var preview: [V2DesktopLayout.Display]?
    private var selectedKey: V2DesktopLayout.Key?
    private var drag: (indices: [Int], start: CGPoint, scale: CGFloat)?
    private var transform: (origin: CGPoint, scale: CGFloat) = (.zero, 1)
    private var draggingTransform: (origin: CGPoint, scale: CGFloat)?
    private var invalid = false
    private var feedback: String?
    private let expand = NSButton(checkboxWithTitle: HopLocale.text("Show individual displays", "展开各台电脑的显示器"), target: nil, action: nil)
    private let viewAngle = NSSegmentedControl(labels: [HopLocale.text("Space", "空间"), HopLocale.text("Front", "正视")],
                                               trackingMode: .selectOne, target: nil, action: nil)
    private let hint = NSTextField(wrappingLabelWithString: "")
    private let device = NSPopUpButton()
    private let controls = NSBox()
    private var spatial: Bool { viewAngle.selectedSegment == 0 }
    private var boardRect: CGRect { CGRect(x: 14, y: 42, width: bounds.width - 188, height: bounds.height - 89) }

    init(layout: V2DesktopLayout, anchor: V2DesktopLayout.Key? = nil) {
        saved = layout
        self.anchor = layout.displays.contains(where: { $0.key == anchor }) ? anchor : layout.displays.first(where: { $0.key.local })?.key
        super.init(frame: CGRect(x: 0, y: 0, width: 640, height: 386))
        expand.frame = CGRect(x: 17, y: 8, width: 290, height: 24)
        expand.target = self; expand.action = #selector(updateGrouping)
        viewAngle.frame = CGRect(x: 472, y: 8, width: 154, height: 24)
        viewAngle.selectedSegment = 0; viewAngle.target = self; viewAngle.action = #selector(updateGrouping)
        viewAngle.setAccessibilityLabel(HopLocale.text("Layout view angle", "布局视角"))
        hint.frame = CGRect(x: 17, y: 350, width: 609, height: 34)
        hint.font = .systemFont(ofSize: 12)
        hint.textColor = .secondaryLabelColor
        addSubview(expand); addSubview(viewAngle); addSubview(hint)
        setupControls()
        updateDeviceMenu()
        updateHint()
        setAccessibilityLabel(HopLocale.text("Device arrangement", "设备空间布局"))
    }
    required init?(coder: NSCoder) { fatalError("Programmatic layout canvas") }
    override var isFlipped: Bool { true }
    var layout: V2DesktopLayout { saved }

    private func setupControls() {
        controls.boxType = .custom; controls.borderWidth = 0; controls.cornerRadius = 14
        controls.fillColor = .controlBackgroundColor
        controls.contentViewMargins = .zero
        controls.frame = CGRect(x: 479, y: 42, width: 147, height: 210)
        let panel = NSView(frame: controls.bounds)
        controls.contentView = panel
        addSubview(controls)
        let title = NSTextField(labelWithString: HopLocale.text("Place next to this Mac", "放到本机旁边"))
        title.font = .systemFont(ofSize: 12, weight: .semibold)
        title.frame = CGRect(x: 12, y: 178, width: 123, height: 17)
        panel.addSubview(title)
        device.frame = CGRect(x: 9, y: 141, width: 128, height: 27)
        device.target = self; device.action = #selector(selectDevice)
        device.setAccessibilityLabel(HopLocale.text("Display to move", "要移动的设备或显示器"))
        panel.addSubview(device)
        let labels: [(V2LayoutEdge, String, String)] = [
            (.top, "↑ Above", "↑ 上方"), (.bottom, "↓ Below", "↓ 下方"),
            (.left, "← Left", "← 左侧"), (.right, "→ Right", "→ 右侧")
        ]
        for (index, item) in labels.enumerated() {
            let button = NSButton(title: HopLocale.text(item.1, item.2), target: self, action: #selector(place(_:)))
            button.tag = item.0.rawValue
            button.controlSize = .small
            button.frame = CGRect(x: 9 + index % 2 * 65, y: 105 - index / 2 * 34, width: 65, height: 26)
            panel.addSubview(button)
        }
        let caption = NSTextField(wrappingLabelWithString: HopLocale.text("Or drag a display on the canvas.", "也可以直接拖动左侧的设备。"))
        caption.font = .systemFont(ofSize: 11); caption.textColor = .secondaryLabelColor
        caption.frame = CGRect(x: 12, y: 19, width: 123, height: 38)
        panel.addSubview(caption)
    }

    private var remoteCards: [Card] { cards(saved.displays).filter { !$0.local } }

    private func updateDeviceMenu() {
        let choices = remoteCards
        device.removeAllItems()
        for card in choices { device.menu?.addItem(withTitle: card.name, action: nil, keyEquivalent: "") }
        let index = choices.firstIndex { card in card.indices.contains { saved.displays[$0].key == selectedKey } } ?? 0
        if choices.indices.contains(index) {
            device.selectItem(at: index)
            selectedKey = saved.displays[choices[index].indices[0]].key
        }
    }

    @objc private func selectDevice() {
        guard remoteCards.indices.contains(device.indexOfSelectedItem) else { return }
        let card = remoteCards[device.indexOfSelectedItem]
        selectedKey = saved.displays[card.indices[0]].key
        feedback = nil; invalid = false
        updateHint(); needsDisplay = true
    }

    @objc private func place(_ button: NSButton) {
        let choices = remoteCards
        guard let anchor, let edge = V2LayoutEdge(rawValue: button.tag), choices.indices.contains(device.indexOfSelectedItem) else { return }
        let keys = Set(choices[device.indexOfSelectedItem].indices.map { saved.displays[$0].key })
        if let next = saved.placing(keys, by: anchor, on: edge) {
            saved = next
            feedback = nil; invalid = false
        } else {
            feedback = HopLocale.text("That side is occupied. Try a different side or drag to a free edge.", "这个方向被其他屏幕占用，可换个方向或拖到空余边缘。")
        }
        updateHint(); needsDisplay = true
    }

    private func cards(_ displays: [V2DesktopLayout.Display]) -> [Card] {
        if expand.state == .on {
            return displays.enumerated().map { index, display in
                let peers = displays.filter { $0.key.device == display.key.device }
                let nth = (peers.firstIndex(where: { $0.key == display.key }) ?? 0) + 1
                return Card(name: "\(display.label) · \(nth)", indices: [index], frame: display.frame, local: display.key.local)
            }
        }
        var order = [String](), groups = [String: [Int]]()
        for (i, display) in displays.enumerated() {
            if groups[display.key.device] == nil { order.append(display.key.device) }
            groups[display.key.device, default: []].append(i)
        }
        return order.compactMap { id in
            guard let indices = groups[id], let first = indices.first else { return nil }
            let screen = displays[first]
            let box = indices.reduce(CGRect.null) { $0.union(displays[$1].frame) }
            return Card(name: screen.label, indices: indices, frame: box, local: screen.key.local)
        }
    }

    private func fit(_ cards: [Card]) -> (CGPoint, CGFloat) {
        let box = cards.reduce(CGRect.null) { rect, card in
            rect.union(corners(card.frame).map(project).reduce(CGRect.null) { $0.union(CGRect(origin: $1, size: .zero)) })
        }
        guard !box.isNull else { return (CGPoint(x: bounds.midX, y: bounds.midY), 1) }
        let scale = min((boardRect.width - 54) / max(1, box.width), (boardRect.height - 66) / max(1, box.height), 1)
        return (CGPoint(x: boardRect.midX - box.midX * scale, y: boardRect.midY - 12 - box.midY * scale), scale)
    }

    private func project(_ p: CGPoint) -> CGPoint {
        spatial ? CGPoint(x: p.x + p.y * 0.24, y: p.y * 0.82) : p
    }
    private func unproject(_ p: CGPoint) -> CGPoint {
        guard spatial else { return p }
        let y = p.y / 0.82
        return CGPoint(x: p.x - y * 0.24, y: y)
    }
    private func corners(_ rect: CGRect) -> [CGPoint] {
        [CGPoint(x: rect.minX, y: rect.minY), CGPoint(x: rect.maxX, y: rect.minY),
         CGPoint(x: rect.maxX, y: rect.maxY), CGPoint(x: rect.minX, y: rect.maxY)]
    }
    private func viewPoint(_ point: CGPoint) -> CGPoint {
        let (origin, scale) = draggingTransform ?? transform
        let p = project(point)
        return CGPoint(x: origin.x + p.x * scale, y: origin.y + p.y * scale)
    }
    private func shape(_ points: [CGPoint]) -> NSBezierPath {
        let path = NSBezierPath()
        guard let first = points.first else { return path }
        path.move(to: first)
        for point in points.dropFirst() { path.line(to: point) }
        path.close()
        return path
    }

    override func draw(_ dirtyRect: NSRect) {
        let board = NSBezierPath(roundedRect: boardRect, xRadius: 16, yRadius: 16)
        NSColor.controlBackgroundColor.setFill(); board.fill()
        NSColor.systemIndigo.withAlphaComponent(0.045).setFill(); board.fill()
        NSColor.separatorColor.setStroke(); board.stroke()
        NSGraphicsContext.saveGraphicsState()
        board.addClip()
        let displays = preview ?? saved.displays
        let cards = cards(displays)
        if draggingTransform == nil { transform = fit(cards) }
        NSColor.systemIndigo.withAlphaComponent(0.13).setFill()
        for y in stride(from: 54, through: Int(boardRect.maxY), by: 19) {
            for x in stride(from: -65, through: Int(boardRect.maxX) + 20, by: 19) {
                let gridX = CGFloat(x) + (spatial ? CGFloat(y - 54) * 0.29 : 0)
                NSBezierPath(ovalIn: CGRect(x: gridX, y: CGFloat(y), width: 1.8, height: 1.8)).fill()
            }
        }
        for card in cards {
            let active = drag?.indices == card.indices
            let selected = card.indices.contains { displays[$0].key == selectedKey }
            let color: NSColor = card.local ? .systemIndigo : (active && invalid ? .systemRed : .systemBlue)
            let width = card.frame.width * transform.scale
            let height = card.frame.height * transform.scale
            let points = corners(card.frame).map(viewPoint)
            let face = shape(points)
            let bottom = viewPoint(CGPoint(x: card.frame.midX, y: card.frame.maxY))
            let stand = min(14, height * 0.16)
            if spatial && width >= 30 && height >= 20 {
                let back = points.map { CGPoint(x: $0.x + 7, y: $0.y - 6) }
                color.withAlphaComponent(0.24).setFill(); shape(back).fill()
                color.withAlphaComponent(0.46).setFill()
                shape([points[0], points[1], back[1], back[0]]).fill()
                shape([points[1], points[2], back[2], back[1]]).fill()
                color.withAlphaComponent(0.72).setFill()
                NSBezierPath(roundedRect: CGRect(x: bottom.x - 2, y: bottom.y, width: 4, height: stand), xRadius: 2, yRadius: 2).fill()
                NSBezierPath(ovalIn: CGRect(x: bottom.x - stand, y: bottom.y + stand - 2, width: stand * 2, height: 5)).fill()
            }
            NSColor.controlBackgroundColor.setFill(); face.fill()
            color.withAlphaComponent(active || selected ? 0.21 : 0.10).setFill(); face.fill()
            color.withAlphaComponent(selected ? 0.78 : 0.43).setStroke(); face.lineWidth = active || selected ? 2 : 1; face.stroke()
            guard width >= 34, height >= 24 else { continue }
            let mid = viewPoint(CGPoint(x: card.frame.midX, y: card.frame.midY))
            let paragraph = NSMutableParagraphStyle()
            paragraph.lineBreakMode = .byTruncatingTail; paragraph.alignment = .center
            (card.name as NSString).draw(in: CGRect(x: mid.x - width / 2 + 6, y: mid.y - 9, width: width - 12, height: 18),
                withAttributes: [.font: NSFont.systemFont(ofSize: 12, weight: .semibold), .foregroundColor: NSColor.labelColor, .paragraphStyle: paragraph])
        }
        let layout = preview == nil ? saved : (try? V2DesktopLayout(displays))
        for seam in layout?.seams ?? [] where seam.source.device != seam.target.device || expand.state == .on && !seam.source.local {
            let (a, b) = seam.endpoints
            let path = NSBezierPath()
            path.move(to: viewPoint(a)); path.line(to: viewPoint(b))
            path.lineCapStyle = .round
            NSColor.controlBackgroundColor.setStroke(); path.lineWidth = 7; path.stroke()
            NSColor.systemGreen.setStroke(); path.lineWidth = 4; path.stroke()
        }
        NSGraphicsContext.restoreGraphicsState()
    }

    override func mouseDown(with event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        let point = unproject(CGPoint(x: (p.x - transform.origin.x) / transform.scale, y: (p.y - transform.origin.y) / transform.scale))
        guard boardRect.contains(p), let card = cards(saved.displays).last(where: { $0.frame.contains(point) }), !card.local else { return }
        selectedKey = saved.displays[card.indices[0]].key
        updateDeviceMenu()
        drag = (card.indices, p, transform.scale)
        draggingTransform = transform
        invalid = false; feedback = nil
        updateHint(); needsDisplay = true
    }

    override func mouseDragged(with event: NSEvent) {
        guard let drag, drag.scale > 0 else { return }
        let p = convert(event.locationInWindow, from: nil)
        let offset = unproject(CGPoint(x: (p.x - drag.start.x) / drag.scale, y: (p.y - drag.start.y) / drag.scale))
        let dx = offset.x.rounded(), dy = offset.y.rounded()
        let moving = Set(drag.indices)
        var snaps: [CGPoint] = []
        for i in drag.indices {
            let a = saved.displays[i].frame.offsetBy(dx: dx, dy: dy)
            for (j, other) in saved.displays.enumerated() where !moving.contains(j) {
                let b = other.frame
                if max(a.minY, b.minY) < min(a.maxY, b.maxY) {
                    for delta in [b.maxX - a.minX, b.minX - a.maxX] where abs(delta) * drag.scale <= 14 {
                        snaps.append(CGPoint(x: delta, y: 0))
                    }
                }
                if max(a.minX, b.minX) < min(a.maxX, b.maxX) {
                    for delta in [b.maxY - a.minY, b.minY - a.maxY] where abs(delta) * drag.scale <= 14 {
                        snaps.append(CGPoint(x: 0, y: delta))
                    }
                }
            }
        }
        func shifted(by correction: CGPoint) -> [V2DesktopLayout.Display] {
            var next = saved.displays
            for i in drag.indices {
                next[i].frame = next[i].frame.offsetBy(dx: dx + correction.x, dy: dy + correction.y)
            }
            return next
        }
        let ordered = snaps.sorted { abs($0.x) + abs($0.y) < abs($1.x) + abs($1.y) }
        preview = ordered.lazy.map(shifted).first { (try? V2DesktopLayout($0)) != nil } ?? shifted(by: .zero)
        invalid = preview.flatMap { try? V2DesktopLayout($0) } == nil
        updateHint(); needsDisplay = true
    }

    override func mouseUp(with event: NSEvent) {
        if let preview {
            if let next = try? V2DesktopLayout(preview) { saved = next }
            else { feedback = HopLocale.text("Overlapping screens. Kept the previous positions.", "屏幕重叠，已保留原位置。") }
        }
        preview = nil; drag = nil; draggingTransform = nil
        invalid = false
        updateHint(); needsDisplay = true
    }

    private func updateHint() {
        if invalid {
            hint.stringValue = HopLocale.text("Displays overlap. Release to keep their previous positions.", "屏幕重叠，松手后将恢复原位置。")
        } else if let feedback {
            hint.stringValue = feedback
        } else {
            let layout = preview.flatMap { try? V2DesktopLayout($0) } ?? saved
            if layout.unreachable(using: layout.seams).isEmpty {
                hint.stringValue = HopLocale.text("Green edges allow crossing in both directions.", "绿色边段可双向穿越。")
            } else {
                hint.stringValue = HopLocale.text("Some screens are out of reach. Touch an edge to connect them.", "有屏幕尚未接通。让屏幕边缘相贴，直到出现绿线。")
            }
        }
        hint.textColor = invalid ? .systemRed : .secondaryLabelColor
    }
    @objc private func updateGrouping() {
        updateDeviceMenu()
        needsDisplay = true
        guard !NSWorkspace.shared.accessibilityDisplayShouldReduceMotion else { return }
        alphaValue = 0.78
        NSAnimationContext.runAnimationGroup { context in
            context.duration = 0.16
            animator().alphaValue = 1
        }
    }
}
