import AppKit

enum HopLocale {
    // Tests can exercise both languages without changing the user's preferences.
    static var forceChinese: Bool?
    static var chinese: Bool { forceChinese ?? (Locale.preferredLanguages.first?.lowercased().hasPrefix("zh") == true) }
    static func text(_ english: String, _ chinese: String) -> String { self.chinese ? chinese : english }
    static func status(_ value: String) -> String {
        guard chinese else { return value }
        let translations: [String: String] = [
            "Starting local server…": "正在启动本地服务…", "Connecting…": "正在连接…",
            "Controlling remote computer": "正在控制另一台电脑", "Waiting for remote permission / ENTER acknowledgement": "正在等待对方确认接管",
            "Connected — local input": "已连接 · 当前控制本机", "Connected — sharing disabled": "已连接 · 共享已关闭",
            "Connected — sharing enabled": "已连接 · 共享已开启", "Sharing enabled — waiting for paired device": "共享已开启 · 等待已配对设备",
            "Sharing disabled; local input restored": "共享已关闭 · 已恢复本机输入", "Peer disconnected": "对方已断开连接",
            "Peer unpaired": "对方已解除配对", "Pairing cancelled": "已取消配对", "Unpaired — select Pair to create a new binding": "已解除配对 · 可重新配对",
            "Select this Mac on the other computer within 60 seconds": "请在 60 秒内从另一台电脑选择此 Mac",
            "Direction changed — paired device may reconnect": "方向已更改 · 已配对设备可重新连接",
            "Sleep/session lock — local input restored; enable Hop to resume": "休眠或锁屏 · 已恢复本机输入，请重新开启共享",
            "Display configuration changed — local input restored": "显示器配置已更改 · 已恢复本机输入",
            "Emergency stop — local input restored": "已紧急停止 · 已恢复本机输入",
            "Grant Accessibility and Input Monitoring permissions, then enable Hop": "请先授予辅助功能和输入监控权限，再开启共享",
            "Cannot create active CGEventTap; check permissions": "无法监听键鼠输入，请检查系统权限"
        ]
        if let translated = translations[value] { return translated }
        if value.hasPrefix("Listening on TCP ") { return "正在监听端口 " + value.dropFirst("Listening on TCP ".count) }
        if value.hasPrefix("Connection rejected:") { return "连接被拒绝：" + value.dropFirst("Connection rejected:".count) }
        return value
    }
}
private func tr(_ english: String, _ chinese: String) -> String { HopLocale.text(english, chinese) }

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
    var mouseTuning = MouseTuning()
    var encrypted = false
    var pairingAvailable = true
    var fingerprints: String? = nil
    var screenNames: [String] = []
    var selectedScreen = 0
    var clipboardAvailable = false
    var clipboardEnabled = false
    var receivingAvailable = false
    var receivingMode = false
    var canConnectReceiver = false
    var receiverBusy = false
    var deviceNames: [String] = []
    var selectedDevice = 0
    var layoutAvailable = false
    var usesLayout = false
    var arrangingAllowed = true
    var pairedFingerprint: String? = nil
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
    func setMouseTuning(_ tuning: MouseTuning)
    func selectScreen(_ index: Int)
    func setClipboardEnabled(_ enabled: Bool)
    func setReceivingMode(_ enabled: Bool)
    func connectReceiver()
    func disconnectReceiver()
    func selectDevice(_ index: Int)
    func editLayout()
}
extension PopoverModel {
    func selectScreen(_ index: Int) {}
    func setClipboardEnabled(_ enabled: Bool) {}
    func setReceivingMode(_ enabled: Bool) {}
    func connectReceiver() {}
    func disconnectReceiver() {}
    func selectDevice(_ index: Int) {}
    func editLayout() {}
}
protocol HopAppModel: PopoverModel {
    var changed: () -> Void { get set }
    var input: InputCapture { get }
    func start()
    func quit()
}
extension AppController: HopAppModel {
    var popoverPresentation: PopoverPresentation {
        let candidate = session.challenge
        return PopoverPresentation(status: stateText, pairedName: session.binding?.name, enabled: enabled,
                                   direction: session.direction, permissionGranted: InputCapture.permissionGranted,
                                   code: candidate?.code, peerName: candidate?.hello.name ?? "",
                                   peerConfirmed: session.peerConfirmed, localConfirmed: session.localConfirmed,
                                   pairingAllowed: session.pairingAllowed, mouseTuning: input.tuning)
    }
}

final class PopoverController: NSViewController {
    private let controller: PopoverModel
    private let stack = NSStackView()
    private let card = NSStackView()
    private let header = NSStackView()
    private let homeActions = NSStackView()
    private let settingsActions = NSStackView()
    private let section = NSTextField(labelWithString: "")
    private let settingsTitle = NSTextField(labelWithString: tr("Settings", "设置"))
    private let settingsRole = NSTextField(labelWithString: tr("This Mac", "这台 Mac"))
    private let state = NSTextField(wrappingLabelWithString: "")
    private let paired = NSTextField(wrappingLabelWithString: "")
    private let permissions = NSTextField(wrappingLabelWithString: "")
    private let code = NSTextField(labelWithString: "")
    private let confirmation = NSTextField(wrappingLabelWithString: "")
    private let fingerprints = NSTextField(wrappingLabelWithString: "")
    private let screens = NSPopUpButton()
    private let devices = NSPopUpButton()
    private let role = NSSegmentedControl(labels: [tr("Share input", "共享键鼠"), tr("Receive input", "接收键鼠")], trackingMode: .selectOne, target: nil, action: nil)
    private let connect = NSButton(title: tr("Connect to a host…", "连接键鼠主机…"), target: nil, action: nil)
    private let disconnect = NSButton(title: tr("Disconnect", "断开连接"), target: nil, action: nil)
    private let arrange = NSButton(title: tr("Arrange devices…", "空间布局…"), target: nil, action: nil)
    private let layout = NSTextField(labelWithString: tr("Other computer is on this Mac’s:", "另一台电脑位于这台 Mac 的："))
    private let emergency = NSTextField(wrappingLabelWithString: "")
    private let enable = NSButton(checkboxWithTitle: tr("Enable keyboard & mouse sharing", "启用键鼠共享"), target: nil, action: nil)
    private let clipboard = NSButton(checkboxWithTitle: tr("Sync text and images", "同步文本和图片剪贴板"), target: nil, action: nil)
    private let direction = NSSegmentedControl(labels: [tr("← Left", "← 左侧"), tr("Right →", "右侧 →")], trackingMode: .selectOne, target: nil, action: nil)
    private let pair = NSButton(title: tr("Pair a computer…", "配对电脑…"), target: nil, action: nil)
    private let unpair = NSButton(title: tr("Unpair", "解除配对"), target: nil, action: nil)
    private let mouseSettings = NSButton(title: tr("Mouse settings…", "鼠标设置…"), target: nil, action: nil)
    private let confirm = NSButton(title: tr("Codes match — Confirm", "配对码一致，确认"), target: nil, action: nil)
    private let reject = NSButton(title: tr("Cancel pairing", "取消配对"), target: nil, action: nil)
    private let options = NSButton(title: tr("Settings…", "设置…"), target: nil, action: nil)
    private let share = NSButton(title: tr("Start sharing", "开始共享"), target: nil, action: nil)
    private let permissionsButton = NSButton(title: tr("Grant permissions / Open settings…", "授权 / 打开系统设置…"), target: nil, action: nil)
    private var quitButton: NSButton!
    private var showOptions = false
    private var comparisonVisible = false
    var resizePopover: ((NSSize) -> Void)?
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
        let title = NSTextField(labelWithString: "Hop"); title.font = .systemFont(ofSize: 24, weight: .bold)
        let brand = NSStackView()
        brand.orientation = .horizontal; brand.alignment = .centerY; brand.spacing = 9
        if let mark = NSImage(systemSymbolName: "rectangle.on.rectangle.angled", accessibilityDescription: nil) {
            let icon = NSImageView(image: mark)
            icon.symbolConfiguration = .init(pointSize: 18, weight: .medium)
            icon.contentTintColor = .systemIndigo
            brand.addArrangedSubview(icon)
        }
        brand.addArrangedSubview(title)
        for row in [header, homeActions, settingsActions] {
            row.orientation = .horizontal; row.alignment = .centerY; row.spacing = 8; row.detachesHiddenViews = true
        }
        let spacer = NSView()
        spacer.setContentHuggingPriority(.defaultLow, for: .horizontal)
        header.addArrangedSubview(brand); header.addArrangedSubview(spacer); header.addArrangedSubview(options)
        for row in [homeActions, settingsActions] { row.distribution = .fillEqually }
        homeActions.addArrangedSubview(arrange); homeActions.addArrangedSubview(pair)
        state.font = .systemFont(ofSize: 18, weight: .semibold)
        section.font = .systemFont(ofSize: 11, weight: .semibold); section.textColor = .secondaryLabelColor
        settingsTitle.font = .systemFont(ofSize: 20, weight: .semibold)
        settingsRole.font = .systemFont(ofSize: 12, weight: .medium); settingsRole.textColor = .secondaryLabelColor
        paired.font = .systemFont(ofSize: 12); paired.textColor = .secondaryLabelColor
        code.font = .monospacedDigitSystemFont(ofSize: 30, weight: .semibold)
        permissionsButton.target = self; permissionsButton.action = #selector(grant)
        permissions.font = .systemFont(ofSize: 11); permissions.textColor = .secondaryLabelColor
        fingerprints.isSelectable = true
        fingerprints.font = .monospacedSystemFont(ofSize: 12, weight: .regular)
        emergency.font = .systemFont(ofSize: 12); emergency.textColor = .secondaryLabelColor
        quitButton = NSButton(title: tr("Quit Hop", "退出 Hop"), target: self, action: #selector(quitApp))
        card.orientation = .vertical; card.alignment = .leading; card.spacing = 6; card.detachesHiddenViews = true
        card.edgeInsets = NSEdgeInsets(top: 14, left: 14, bottom: 14, right: 14)
        card.wantsLayer = true
        card.layer?.backgroundColor = NSColor.systemIndigo.withAlphaComponent(0.11).cgColor
        card.layer?.cornerRadius = 14
        card.addArrangedSubview(section); card.addArrangedSubview(state); card.addArrangedSubview(paired)
        settingsActions.addArrangedSubview(unpair); settingsActions.addArrangedSubview(quitButton)
        for subview in [header, card, code, confirmation, fingerprints, confirm, reject, permissionsButton, share, enable, connect, disconnect, homeActions,
                        settingsTitle, settingsRole, role, clipboard, layout, direction, devices, screens, permissions, mouseSettings, emergency, settingsActions] { stack.addArrangedSubview(subview) }
        for fullWidth in [header, card, homeActions, share, connect, disconnect, confirm, settingsActions] {
            let width = fullWidth.widthAnchor.constraint(equalTo: stack.widthAnchor)
            width.priority = .defaultHigh
            width.isActive = true
        }
        for button in [arrange, pair, share, connect, disconnect, confirm] { button.controlSize = .large }
        for label in [state, paired, permissions, confirmation, fingerprints, emergency] {
            label.widthAnchor.constraint(lessThanOrEqualToConstant: label === state || label === paired ? 292 : 320).isActive = true
        }
        options.target = self; options.action = #selector(toggleOptions)
        share.target = self; share.action = #selector(toggleSharing)
        share.bezelColor = .systemIndigo
        connect.bezelColor = .systemIndigo
        confirm.bezelColor = .systemIndigo
        enable.target = self; enable.action = #selector(toggleEnabled)
        clipboard.target = self; clipboard.action = #selector(toggleClipboard)
        clipboard.toolTip = tr("Sync only while controlling the paired device. Changes take effect on the next crossing.", "仅在控制已配对设备时同步。开启后在下次穿越时生效。")
        role.target = self; role.action = #selector(changeRole)
        connect.target = self; connect.action = #selector(connectToHost)
        disconnect.target = self; disconnect.action = #selector(disconnectFromHost)
        arrange.target = self; arrange.action = #selector(arrangeDevices)
        direction.target = self; direction.action = #selector(changeDirection)
        screens.target = self; screens.action = #selector(changeScreen)
        devices.target = self; devices.action = #selector(changeDevice)
        pair.target = self; pair.action = #selector(startPairing)
        unpair.target = self; unpair.action = #selector(removePairing)
        mouseSettings.target = self; mouseSettings.action = #selector(editMouseSettings)
        confirm.target = self; confirm.action = #selector(confirmCode)
        reject.target = self; reject.action = #selector(cancelPairing)
        refresh()
    }
    func refresh() {
        guard isViewLoaded else { return }
        let model = controller.popoverPresentation
        state.stringValue = HopLocale.status(model.status)
        paired.stringValue = model.pairedName.map { tr("Paired device: ", "已配对设备：") + $0 } ?? tr("No paired device", "尚未配对设备")
        enable.state = model.enabled ? .on : .off
        role.isHidden = !model.receivingAvailable
        role.selectedSegment = model.receivingMode ? 1 : 0
        enable.isHidden = model.receivingMode
        layout.isHidden = model.receivingMode || model.usesLayout
        direction.isHidden = model.receivingMode || model.usesLayout
        arrange.isHidden = model.receivingMode || !model.layoutAvailable
        connect.isHidden = !model.receivingMode || model.receiverBusy
        connect.isEnabled = model.canConnectReceiver
        disconnect.isHidden = !model.receivingMode || !model.receiverBusy
        clipboard.isHidden = !model.clipboardAvailable
        clipboard.state = model.clipboardEnabled ? .on : .off
        direction.selectedSegment = Int(model.direction)
        permissions.stringValue = model.permissionGranted ? tr("Accessibility & Input Monitoring: granted", "辅助功能和输入监控：已授权") : tr("Allow Hop in Privacy & Security → Accessibility and Input Monitoring. Then enable sharing. macOS may require restarting Hop.", "请在“隐私与安全性”中允许 Hop 使用辅助功能和输入监控，然后开启共享。macOS 可能要求重启 Hop。")
        let comparing = model.code != nil || model.fingerprints != nil
        let enteredComparison = comparing && !comparisonVisible
        comparisonVisible = comparing
        code.isHidden = model.code == nil; confirmation.isHidden = !comparing; confirm.isHidden = !comparing
        reject.isHidden = !comparing && !(model.encrypted && model.pairingAllowed)
        fingerprints.isHidden = model.fingerprints == nil
        fingerprints.stringValue = model.fingerprints ?? ""
        screens.isHidden = model.receivingMode || model.screenNames.count < 2
        devices.isHidden = model.receivingMode || model.deviceNames.count < 2
        if devices.itemTitles != model.deviceNames { devices.removeAllItems(); devices.addItems(withTitles: model.deviceNames) }
        if devices.numberOfItems > model.selectedDevice { devices.selectItem(at: model.selectedDevice) }
        if screens.itemTitles != model.screenNames {
            screens.removeAllItems(); screens.addItems(withTitles: model.screenNames)
        }
        if screens.numberOfItems > model.selectedScreen { screens.selectItem(at: model.selectedScreen) }
        emergency.stringValue = model.encrypted ? tr("Emergency stop: ⌃⌥⌘Esc", "紧急停止：⌃⌥⌘Esc") :
            tr("Emergency stop: ⌃⌥⌘Esc\nOnly use on a trusted LAN. Traffic and pairing token are not encrypted.", "紧急停止：⌃⌥⌘Esc\n仅限可信局域网使用；当前连接和配对令牌未加密。")
        if let pairingCode = model.code {
            code.stringValue = String(format: "%06u", pairingCode)
            confirmation.stringValue = HopLocale.chinese ? "\(model.peerName)：请在两台电脑核对相同的配对码。\n对方：\(model.peerConfirmed ? "已确认" : "等待确认")" : "\(model.peerName): verify the same code on BOTH computers.\nOther computer: \(model.peerConfirmed ? "confirmed" : "waiting")"
        }
        if model.fingerprints != nil {
            confirmation.stringValue = tr("Verify both complete fingerprints on the two devices before confirming.", "请在两台设备上分别核对双方的完整指纹，再确认配对。")
        }
        confirm.isEnabled = !model.localConfirmed
        confirm.title = model.localConfirmed ? tr("Confirmed — waiting for peer", "已确认 · 等待对方") : tr("Codes match — Confirm", "配对码一致，确认")
        if model.fingerprints != nil && !model.localConfirmed { confirm.title = tr("Both fingerprints match", "双方指纹一致，确认") }
        pair.isHidden = model.receivingMode || comparing || (!model.encrypted && model.pairedName != nil)
        pair.isEnabled = model.pairingAvailable && !model.pairingAllowed
        pair.title = model.pairingAllowed ? tr("Waiting for pairing request…", "正在等待配对请求…") : tr("Pair a computer…", "配对电脑…")
        unpair.isHidden = model.pairedName == nil
        mouseSettings.isHidden = model.receivingMode || model.pairedName == nil
        share.isHidden = true
        section.isHidden = true
        card.isHidden = false
        settingsTitle.isHidden = true; settingsRole.isHidden = true
        options.isHidden = true
        emergency.isHidden = false
        quitButton.isHidden = false
        permissionsButton.isHidden = false
        permissions.isHidden = false
        if model.encrypted {
            let details = showOptions && !comparing
            card.isHidden = details
            section.isHidden = false
            section.stringValue = comparing ? tr("Pairing", "设备配对") :
                (model.receivingMode ? tr("Receiving input", "接收键鼠") : tr("Sharing input", "共享键鼠"))
            settingsTitle.isHidden = !details
            settingsRole.isHidden = !details || !model.receivingAvailable
            if let name = model.pairedName, !comparing {
                state.stringValue = name
                paired.stringValue = HopLocale.status(model.status)
            }
            role.isHidden = !details || !model.receivingAvailable
            role.isEnabled = model.arrangingAllowed
            clipboard.isHidden = !details || !model.clipboardAvailable
            devices.isHidden = !details || model.receivingMode || model.deviceNames.count < 2
            screens.isHidden = true // individual displays are arranged directly on the canvas
            layout.isHidden = true; direction.isHidden = true
            mouseSettings.isHidden = !details || model.receivingMode || model.pairedName == nil
            unpair.isHidden = !details || model.pairedName == nil
            quitButton.isHidden = !details
            emergency.isHidden = !model.enabled
            options.isHidden = comparing
            options.title = showOptions ? tr("Back", "返回") : tr("Settings…", "设置…")
            paired.isHidden = comparing || model.pairedName == nil
            enable.isHidden = true
            share.isHidden = details || comparing || model.receivingMode || !(model.enabled || (model.permissionGranted && model.layoutAvailable))
            share.title = model.enabled ? tr("Pause sharing", "暂停共享") : tr("Start sharing", "开始共享")
            share.bezelColor = model.enabled ? nil : .systemIndigo
            connect.isHidden = details || comparing || !model.receivingMode || model.receiverBusy
            disconnect.isHidden = details || comparing || !model.receivingMode || !model.receiverBusy
            arrange.isHidden = details || comparing || model.receivingMode || !model.layoutAvailable
            arrange.isEnabled = model.arrangingAllowed
            arrange.toolTip = model.arrangingAllowed ? nil : tr("Move the pointer back to this Mac to arrange screens.", "先把鼠标移回这台 Mac，再调整屏幕。")
            permissionsButton.isHidden = comparing || model.permissionGranted || (!model.layoutAvailable && !details)
            permissions.isHidden = !details || model.permissionGranted
            pair.isHidden = details || pair.isHidden
            reject.isHidden = details || reject.isHidden
            pair.title = model.pairingAllowed ? tr("Waiting for the other computer…", "等待另一台电脑加入…") : tr("Add computer…", "添加电脑…")
            if enteredComparison && view.window != nil { reveal([confirmation, fingerprints, confirm, reject]) }
        }
        homeActions.isHidden = arrange.isHidden && pair.isHidden
        settingsActions.isHidden = unpair.isHidden && quitButton.isHidden
        let size = contentSize()
        guard preferredContentSize != size else { return }
        preferredContentSize = size
        if view.window == nil { view.setFrameSize(size) }
        resizePopover?(size)
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
            if item === card {
                let labels = [section, state, paired].filter { !$0.isHidden }
                for label in labels { label.preferredMaxLayoutWidth = 292 }
                total += 28 + labels.reduce(0) { $0 + $1.fittingSize.height } + CGFloat(labels.count - 1) * card.spacing
                if index + 1 < visible.count { total += stack.spacing }
                continue
            }
            var row = item.intrinsicContentSize.height
            if !row.isFinite || row <= 0 || row > 160 { row = min(max(item.fittingSize.height, 16), 160) }
            total += row
            if index + 1 < visible.count { total += stack.spacing }
        }
        return NSSize(width: width, height: min(620, max(controller.popoverPresentation.encrypted ? 185 : 360, ceil(total))))
    }
    @objc private func toggleOptions() {
        showOptions.toggle(); refresh()
        if showOptions { reveal([settingsTitle, settingsRole, role, clipboard, devices, mouseSettings, settingsActions]) }
        else { reveal([card, share, homeActions, connect, disconnect]) }
    }
    private func reveal(_ views: [NSView]) {
        guard !NSWorkspace.shared.accessibilityDisplayShouldReduceMotion else { return }
        let visible = views.filter { !$0.isHidden }
        visible.forEach { $0.alphaValue = 0 }
        NSAnimationContext.runAnimationGroup { animation in
            animation.duration = 0.16
            visible.forEach { $0.animator().alphaValue = 1 }
        }
    }
    @objc private func toggleSharing() { controller.setEnabled(!controller.popoverPresentation.enabled); refresh() }
    @objc private func toggleEnabled() { controller.setEnabled(enable.state == .on); refresh() }
    @objc private func toggleClipboard() { controller.setClipboardEnabled(clipboard.state == .on); refresh() }
    @objc private func changeRole() { controller.setReceivingMode(role.selectedSegment == 1); refresh() }
    @objc private func connectToHost() { controller.connectReceiver(); refresh() }
    @objc private func disconnectFromHost() { controller.disconnectReceiver(); refresh() }
    @objc private func changeDirection() { controller.setDirection(UInt8(direction.selectedSegment)) }
    @objc private func changeScreen() { controller.selectScreen(screens.indexOfSelectedItem) }
    @objc private func changeDevice() { controller.selectDevice(devices.indexOfSelectedItem); refresh() }
    @objc private func arrangeDevices() { controller.editLayout(); refresh() }
    @objc private func startPairing() { controller.beginPairing() }
    @objc private func removePairing() {
        let model = controller.popoverPresentation
        if model.encrypted {
            guard let name = model.pairedName, let fingerprint = model.pairedFingerprint else { return }
            let alert = NSAlert()
            alert.messageText = tr("Unpair this computer?", "解除这台电脑的配对？")
            alert.informativeText = name
            alert.addButton(withTitle: tr("Cancel", "取消"))
            alert.addButton(withTitle: tr("Unpair", "解除配对"))
            guard alert.runModal() == .alertSecondButtonReturn,
                  controller.popoverPresentation.pairedFingerprint == fingerprint else { return }
        }
        controller.unpair()
    }
    @objc private func editMouseSettings() {
        let current = controller.popoverPresentation.mouseTuning
        let alert = NSAlert()
        alert.messageText = tr("Mouse settings", "鼠标设置")
        alert.informativeText = tr("These settings apply to the paired computer. Pointer sensitivity adjusts movement in software; it does not change the mouse's hardware DPI.", "这些设置仅对当前配对的电脑生效。指针灵敏度通过软件调整移动速度，不会改变鼠标硬件 DPI。")
        alert.addButton(withTitle: tr("Save", "保存"))
        alert.addButton(withTitle: tr("Cancel", "取消"))
        let content = MouseSettingsView(current: current)
        alert.accessoryView = content
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        controller.setMouseTuning(content.value)
    }
    @objc private func confirmCode() { controller.confirmPairing() }
    @objc private func cancelPairing() { controller.rejectPairing() }
    @objc private func grant() {
        controller.requestPermissions()
        if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility") { NSWorkspace.shared.open(url) }
    }
    @objc private func quitApp() { NSApplication.shared.terminate(nil) }
}

/// A separate content view permits visual and behavior checks without showing a modal window.
final class MouseSettingsView: NSView {
    private let values: [Double] = [0.25, 0.5, 0.75, 1, 1.25, 1.5, 2, 3, 4]
    private let rates = [60, 120, 240, 500, 1000]
    private(set) var movement = NSPopUpButton()
    private(set) var horizontal = NSPopUpButton()
    private(set) var vertical = NSPopUpButton()
    private(set) var reportRate = NSPopUpButton()
    private(set) var reverseX = NSButton(checkboxWithTitle: tr("Reverse horizontal scroll", "反转水平滚动方向"), target: nil, action: nil)
    private(set) var reverseY = NSButton(checkboxWithTitle: tr("Reverse vertical scroll", "反转垂直滚动方向"), target: nil, action: nil)

    init(current: MouseTuning) {
        super.init(frame: NSRect(x: 0, y: 0, width: 350, height: 192))
        func row(_ label: String, _ selected: Double, _ y: CGFloat) -> NSPopUpButton {
            let title = NSTextField(labelWithString: label)
            title.frame = NSRect(x: 0, y: y + 3, width: 180, height: 20)
            addSubview(title)
            let popup = NSPopUpButton(frame: NSRect(x: 185, y: y, width: 140, height: 26), pullsDown: false)
            popup.addItems(withTitles: values.map { String(format: "%g×", $0) })
            let index = values.indices.min(by: { abs(values[$0] - selected) < abs(values[$1] - selected) }) ?? 3
            popup.selectItem(at: index)
            addSubview(popup)
            return popup
        }
        movement = row(tr("Pointer sensitivity", "指针灵敏度"), current.movement, 160)
        movement.toolTip = tr("Adjusts movement on this device; it does not change the mouse hardware DPI.", "调整此设备上的指针移动倍率，不会改变鼠标硬件 DPI。")
        horizontal = row(tr("Horizontal scroll speed", "水平滚动速度"), current.horizontal, 128)
        vertical = row(tr("Vertical scroll speed", "垂直滚动速度"), current.vertical, 96)
        let rateLabel = NSTextField(labelWithString: tr("Maximum forwarding rate", "转发频率上限"))
        rateLabel.frame = NSRect(x: 0, y: 67, width: 180, height: 20)
        addSubview(rateLabel)
        reportRate = NSPopUpButton(frame: NSRect(x: 185, y: 64, width: 140, height: 26), pullsDown: false)
        reportRate.addItems(withTitles: rates.map { "\($0) Hz" })
        reportRate.selectItem(at: rates.firstIndex(of: current.reportRateHz) ?? 2)
        reportRate.toolTip = tr("The actual rate depends on the mouse, system and network.", "实际频率取决于鼠标、系统和网络。")
        addSubview(reportRate)
        reverseX.frame = NSRect(x: 0, y: 34, width: 300, height: 22)
        reverseX.state = current.reverseHorizontal ? .on : .off
        addSubview(reverseX)
        reverseY.frame = NSRect(x: 0, y: 8, width: 300, height: 22)
        reverseY.state = current.reverseVertical ? .on : .off
        addSubview(reverseY)
    }
    required init?(coder: NSCoder) { fatalError("Programmatic view") }
    var value: MouseTuning {
        MouseTuning(movement: values[movement.indexOfSelectedItem], horizontal: values[horizontal.indexOfSelectedItem],
                    vertical: values[vertical.indexOfSelectedItem], reverseHorizontal: reverseX.state == .on,
                    reverseVertical: reverseY.state == .on, reportRateHz: rates[reportRate.indexOfSelectedItem])
    }
}
