import AppKit

final class AppDelegate: NSObject, NSApplicationDelegate {
    private var item: NSStatusItem?
    private let popover = NSPopover()
    private var controller: HopAppModel?
    private var panel: PopoverController?
    func applicationDidFinishLaunching(_ notification: Notification) {
        let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        self.item = item
        item.button?.image = NSImage(systemSymbolName: "computermouse", accessibilityDescription: "Hop keyboard and mouse sharing")
        item.button?.target = self; item.button?.action = #selector(togglePopover)
        popover.behavior = .semitransient
        do {
            guard let url = Bundle.main.url(forResource: "keymap", withExtension: "json") else { throw WireError.invalid("Shared keymap resource missing; rebuild Hop") }
            let keymap = try Keymap.load(url: url)
            let controller: HopAppModel
            if CommandLine.arguments.contains("--legacy-transport") {
                controller = try AppController(keymap: keymap)
            } else {
                controller = try SecureHostAppController(keymap: keymap)
            }
            let panel = PopoverController(controller: controller)
            self.controller = controller; self.panel = panel
            popover.contentViewController = panel
            panel.resizePopover = { [weak self] size in self?.popover.contentSize = size }
            controller.changed = { [weak panel, weak item, weak controller] in
                panel?.refresh()
                item?.button?.image = NSImage(systemSymbolName: controller?.input.mode == .remote ? "computermouse.fill" : "computermouse", accessibilityDescription: "Hop")
            }
            controller.start()
        } catch {
            try? "Hop could not start: \(error)".write(toFile: "/tmp/hop-start-error.txt", atomically: true, encoding: .utf8)
            hopLog("start failed \(error)")
            let view = NSViewController(); view.view = NSView(frame: NSRect(x: 0, y: 0, width: 350, height: 150))
            let label = NSTextField(wrappingLabelWithString: "Hop could not start:\n\(error)")
            label.frame = NSRect(x: 20, y: 55, width: 310, height: 80); view.view.addSubview(label)
            let quit = NSButton(title: "Quit Hop", target: self, action: #selector(quit)); quit.frame = NSRect(x: 20, y: 15, width: 100, height: 30); view.view.addSubview(quit)
            popover.contentViewController = view
        }
    }
    @objc private func togglePopover() {
        if popover.isShown { popover.performClose(nil) } else { showPopover() }
    }
    private func showPopover() {
        guard let button = item?.button, !popover.isShown else { return }
        if let panel {
            _ = panel.view
            panel.refresh()
            popover.contentSize = panel.preferredContentSize
        }
        NSApp.activate(ignoringOtherApps: true)
        popover.show(relativeTo: button.bounds, of: button, preferredEdge: .minY)
    }
    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        showPopover()
        return false
    }
    @objc private func quit() { NSApplication.shared.terminate(nil) }
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply { controller?.quit(); return .terminateNow }
    func applicationWillTerminate(_ notification: Notification) { controller?.quit() }
}
let application = NSApplication.shared
let delegate = AppDelegate()
application.delegate = delegate
application.setActivationPolicy(.accessory)
application.run()
