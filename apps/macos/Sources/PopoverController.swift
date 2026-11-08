import AppKit

final class PopoverController: NSViewController {
    private let controller: AppController
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
    init(controller: AppController) { self.controller = controller; super.init(nibName: nil, bundle: nil) }
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
        state.stringValue = controller.stateText
        paired.stringValue = controller.session.binding.map { "Paired device: \($0.name)" } ?? "No paired device"
        enable.state = controller.enabled ? .on : .off
        direction.selectedSegment = Int(controller.session.direction)
        permissions.stringValue = InputCapture.permissionGranted ? "Accessibility & Input Monitoring: granted" : "Allow Hop in Privacy & Security → Accessibility and Input Monitoring. Then enable sharing. macOS may require restarting Hop."
        let candidate = controller.session.challenge
        code.isHidden = candidate == nil; confirmation.isHidden = candidate == nil; confirm.isHidden = candidate == nil; reject.isHidden = candidate == nil
        if let candidate {
            code.stringValue = String(format: "%06u", candidate.code)
            confirmation.stringValue = "\(candidate.hello.name): verify the same code on BOTH computers.\nOther computer: \(controller.session.peerConfirmed ? "confirmed" : "waiting")"
        }
        confirm.isEnabled = !controller.session.localConfirmed
        confirm.title = controller.session.localConfirmed ? "Confirmed — waiting for peer" : "Codes match — Confirm"
        pair.isHidden = controller.session.binding != nil || candidate != nil
        pair.isEnabled = !controller.session.pairingAllowed
        pair.title = controller.session.pairingAllowed ? "Waiting for pairing request…" : "Pair a computer…"
        unpair.isHidden = controller.session.binding == nil
        view.layoutSubtreeIfNeeded()
        let size = NSSize(width: 360, height: max(360, stack.fittingSize.height + 36))
        preferredContentSize = size
        view.setFrameSize(size)
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
