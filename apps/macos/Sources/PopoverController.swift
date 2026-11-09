import AppKit

/// Presentation-only boundary: native view tests can supply memory data/actions,
/// without constructing capture/network/storage controllers or querying permissions.
struct PopoverPresentation {
    var status = "Starting local server…"
    var pairedName: String? = nil
    var enabled = false
    var direction: UInt8 = 1
    var permissionGranted = false
    var code: UInt32? = nil
    var peerName = ""
    var peerConfirmed = false
    var localConfirmed = false
    var pairingAllowed = false
}
protocol PopoverModel: AnyObject {
    var popoverPresentation: PopoverPresentation { get }
    func setEnabled(_ value: Bool)
    func setDirection(_ direction: UInt8)
    func beginPairing()
    func unpair()
    func confirmPairing()
    func rejectPairing()
    func requestPermissions()
}
extension AppController: PopoverModel {
    var popoverPresentation: PopoverPresentation {
        let candidate = session.challenge
        return PopoverPresentation(status: stateText, pairedName: session.binding?.name, enabled: enabled,
                                   direction: session.direction, permissionGranted: InputCapture.permissionGranted,
                                   code: candidate?.code, peerName: candidate?.hello.name ?? "",
                                   peerConfirmed: session.peerConfirmed, localConfirmed: session.localConfirmed,
                                   pairingAllowed: session.pairingAllowed)
    }
}

final class PopoverController: NSViewController {
    private let controller: PopoverModel
    private let stack = NSStackView()
    private let state = NSTextField(wrappingLabelWithString: "")
    private let paired = NSTextField(wrappingLabelWithString: "")
    private let permissions = NSTextField(wrappingLabelWithString: "")
    private let code = NSTextField(labelWithString: "")
    private let confirmation = NSTextField(wrappingLabelWithString: "")
    private let enable = NSButton(checkboxWithTitle: "Enable keyboard & mouse sharing", target: nil, action: nil)
    private let direction = NSSegmentedControl(labels: ["← Left", "Right →"], trackingMode: .selectOne, target: nil, action: nil)
    private let pair = NSButton(title: "Pair a computer…", target: nil, action: nil)
    private let unpair = NSButton(title: "Unpair", target: nil, action: nil)
    private let confirm = NSButton(title: "Codes match — Confirm", target: nil, action: nil)
    private let reject = NSButton(title: "Cancel pairing", target: nil, action: nil)
    init(controller: PopoverModel) { self.controller = controller; super.init(nibName: nil, bundle: nil) }
    required init?(coder: NSCoder) { fatalError("Programmatic controller") }
    override func loadView() {
        view = NSView(frame: NSRect(x: 0, y: 0, width: 360, height: 560))
        stack.orientation = .vertical; stack.alignment = .leading; stack.spacing = 10; stack.detachesHiddenViews = true
        stack.translatesAutoresizingMaskIntoConstraints = false; view.addSubview(stack)
        NSLayoutConstraint.activate([stack.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 20),
                                     stack.trailingAnchor.constraint(equalTo: view.trailingAnchor, constant: -20),
                                     stack.topAnchor.constraint(equalTo: view.topAnchor, constant: 18),
                                     stack.bottomAnchor.constraint(lessThanOrEqualTo: view.bottomAnchor, constant: -18)])
        let title = NSTextField(labelWithString: "Hop"); title.font = .systemFont(ofSize: 22, weight: .semibold)
        state.font = .systemFont(ofSize: 13, weight: .medium)
        paired.font = .systemFont(ofSize: 12)
        let layout = NSTextField(labelWithString: "Other computer is on this Mac’s:")
        code.font = .monospacedDigitSystemFont(ofSize: 30, weight: .semibold)
        let permissionButton = NSButton(title: "Grant permissions / Open settings…", target: self, action: #selector(grant))
        permissions.font = .systemFont(ofSize: 11); permissions.textColor = .secondaryLabelColor
        let emergency = NSTextField(wrappingLabelWithString: "Emergency stop: ⌃⌥⌘Esc\nOnly use on a trusted LAN. Traffic and pairing token are not encrypted.")
        emergency.font = .systemFont(ofSize: 11); emergency.textColor = .secondaryLabelColor
        let quit = NSButton(title: "Quit Hop", target: self, action: #selector(quitApp))
        for subview in [title, state, paired, enable, layout, direction, permissionButton, permissions, pair,
                        code, confirmation, confirm, reject, unpair, emergency, quit] { stack.addArrangedSubview(subview) }
        for label in [state, paired, permissions, confirmation, emergency] { label.widthAnchor.constraint(lessThanOrEqualToConstant: 320).isActive = true }
        enable.target = self; enable.action = #selector(toggleEnabled)
        direction.target = self; direction.action = #selector(changeDirection)
        pair.target = self; pair.action = #selector(startPairing)
        unpair.target = self; unpair.action = #selector(removePairing)
        confirm.target = self; confirm.action = #selector(confirmCode)
        reject.target = self; reject.action = #selector(cancelPairing)
        refresh()
    }
    func refresh() {
        guard isViewLoaded else { return }
        let model = controller.popoverPresentation
        state.stringValue = model.status
        paired.stringValue = model.pairedName.map { "Paired device: \($0)" } ?? "No paired device"
        enable.state = model.enabled ? .on : .off
        direction.selectedSegment = Int(model.direction)
        permissions.stringValue = model.permissionGranted ? "Accessibility & Input Monitoring: granted" : "Allow Hop in Privacy & Security → Accessibility and Input Monitoring. Then enable sharing. macOS may require restarting Hop."
        code.isHidden = model.code == nil; confirmation.isHidden = model.code == nil; confirm.isHidden = model.code == nil; reject.isHidden = model.code == nil
        if let pairingCode = model.code {
            code.stringValue = String(format: "%06u", pairingCode)
            confirmation.stringValue = "\(model.peerName): verify the same code on BOTH computers.\nOther computer: \(model.peerConfirmed ? "confirmed" : "waiting")"
        }
        confirm.isEnabled = !model.localConfirmed
        confirm.title = model.localConfirmed ? "Confirmed — waiting for peer" : "Codes match — Confirm"
        pair.isHidden = model.pairedName != nil || model.code != nil
        pair.isEnabled = !model.pairingAllowed
        pair.title = model.pairingAllowed ? "Waiting for pairing request…" : "Pair a computer…"
        unpair.isHidden = model.pairedName == nil
        let size = contentSize()
        guard preferredContentSize != size else { return }
        preferredContentSize = size
        view.setFrameSize(size)
        view.window?.setContentSize(size)
    }
    /// Height comes from each row's own text, never from the window's current height.
    /// Measuring the live stack made every refresh add another empty band at the top.
    private func contentSize() -> NSSize {
        let width: CGFloat = 360
        var total: CGFloat = 36
        let visible = stack.arrangedSubviews.filter { !$0.isHidden }
        for (index, item) in visible.enumerated() {
            if let label = item as? NSTextField, label.cell?.wraps == true {
                label.preferredMaxLayoutWidth = 320
            }
            var row = item.intrinsicContentSize.height
            if !row.isFinite || row <= 0 || row > 160 { row = min(max(item.fittingSize.height, 16), 160) }
            total += row
            if index + 1 < visible.count { total += stack.spacing }
        }
        return NSSize(width: width, height: min(560, max(360, ceil(total))))
    }
    @objc private func toggleEnabled() { controller.setEnabled(enable.state == .on); refresh() }
    @objc private func changeDirection() { controller.setDirection(UInt8(direction.selectedSegment)) }
    @objc private func startPairing() { controller.beginPairing() }
    @objc private func removePairing() { controller.unpair() }
    @objc private func confirmCode() { controller.confirmPairing() }
    @objc private func cancelPairing() { controller.rejectPairing() }
    @objc private func grant() {
        controller.requestPermissions()
        if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility") { NSWorkspace.shared.open(url) }
    }
    @objc private func quitApp() { NSApplication.shared.terminate(nil) }
}
