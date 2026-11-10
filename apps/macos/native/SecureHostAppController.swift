import AppKit
import ApplicationServices

/// Main-queue side of the encrypted host. Network commands never wait for the
/// worker: its restore callback may need to synchronously hand input back here.
final class SecureHostAppController: HopAppModel, V2InputRouterDelegate {
    let input: InputCapture
    var changed: () -> Void = {}
    private let keymap: HIDKeymap
    private lazy var router = V2InputRouter(keymap: keymap, delegate: self)
    private lazy var moves = MoveForwardingScheduler(
        hasPending: { [weak self] in self?.input.hasPendingMove == true },
        flush: { [weak self] in self?.input.flushMove() },
        rate: { [weak self] in self?.input.tuning.reportRateHz ?? 240 })
    private var host: V2HostCoordinator?
    private var hostGeneration = 0
    private var identity: V2LocalIdentity?
    private var trust: V2TrustStore?
    private var receiver: V2ReceiverCoordinator?
    private var injector: V2MacReceiverInjector?
    private var knownHosts: [V2VerifiedPeer] = []
    private var receivingMode = false
    private var receiverBusy = false
    private var transitioning = false
    private var hostStopping = false
    private var receiverGeneration = 0
    private var receiverComparison: V2ReceiverCoordinator.Comparison?
    private var receivingPeer: V2VerifiedPeer?
    private var receiverPort: UInt16 = 24891
    private var receiverAddress = ""
    private var localName = "Hop Mac"
    private var advertisement: NetService?
    private let discovery = V2Discovery()
    private var maintenance: Timer?
    private var started = false
    private var observers: [NSObjectProtocol] = []
    private var screenSignature = [V2MacReceiverInjector.Display]()
    private var quitting = false
    private var enabled = false
    private var clipboardEnabled = UserDefaults.standard.object(forKey: "HopV2ClipboardEnabled") as? Bool ?? true
    private var listening = false
    private var pairing = false
    private var comparison: V2HostCoordinator.Comparison?
    private var approved = false
    private var peer: V2VerifiedPeer?
    private var connectedPeers: [(V2VerifiedPeer, [V2SessionBridge.Screen])] = []
    private let layoutKey = "HopV2DesktopLayout"
    private var placements = V2DesktopLayout.load(UserDefaults.standard.data(forKey: "HopV2DesktopLayout"))
    private var topology: V2DesktopLayout?
    private var editingLayout = false
    private var pendingHandoff: (epoch: UInt64, route: V2DesktopLayout.Route, anchor: CGPoint, presses: UInt64)?
    private var returnOverride: CGPoint?
    private var usesLayout: Bool { topology?.displays.contains(where: { !$0.key.local }) == true }
    private var screens: [V2SessionBridge.Screen] = []
    private var target = 0
    private var status = HopLocale.text("Starting encrypted sharing…", "正在启动加密共享…")

    init(keymap: Keymap) throws {
        self.keymap = try HIDKeymap(keymap: keymap)
        input = InputCapture(keymap: keymap)
        input.ignoredEventSourceUserData = V2QuartzInputBackend.eventMarker
        input.requireNeutralEntry = true
        input.direction = UserDefaults.standard.string(forKey: "HopV2Direction") == "left" ? 0 : 1
        input.mayEnter = { [weak self] in
            guard let self else { return false }
            return !self.receivingMode && self.enabled && !self.quitting && !self.editingLayout && !self.pairing &&
                self.pendingHandoff == nil && (self.usesLayout || (self.peer != nil && self.router.selectedScreen != 0))
        }
        input.peerSize = { [weak self] in
            guard let self, self.screens.indices.contains(self.target) else { return (1, 1) }
            return (self.screens[self.target].width, self.screens[self.target].height)
        }
        input.send = { [weak self] in self?.router.captured($0) }
        input.movementAvailable = { [weak self] in self?.moves.movementAvailable() }
        input.changed = { [weak self] in DispatchQueue.main.async { [weak self] in self?.changed() } }
        input.fault = { [weak self] message, _ in
            guard let self else { return }
            let generation = self.input.generation
            DispatchQueue.main.async { [weak self] in
                guard let self, self.input.generation == generation else { return }
                self.suspend(message)
            }
        }
    }

    func start() {
        guard !quitting, !started else { return }
        started = true
        let workspace = NSWorkspace.shared.notificationCenter
        for event in [NSWorkspace.willSleepNotification, NSWorkspace.screensDidSleepNotification, NSWorkspace.sessionDidResignActiveNotification] {
            observers.append(workspace.addObserver(forName: event, object: nil, queue: .main) { [weak self] _ in
                self?.suspend(HopLocale.text("Sharing paused while this Mac sleeps or locks.", "这台 Mac 休眠或锁屏时已暂停共享。"))
            })
        }
        screenSignature = Self.displayBounds()
        observers.append(NotificationCenter.default.addObserver(forName: NSApplication.didChangeScreenParametersNotification, object: nil, queue: .main) { [weak self] _ in
            guard let self else { return }
            let bounds = Self.displayBounds()
            guard bounds != self.screenSignature else { return }
            self.screenSignature = bounds
            if self.receivingMode || bounds.isEmpty {
                self.suspend(HopLocale.text("Displays changed. Reconnect when ready.", "显示器已变化，请重新连接。"))
            } else {
                self.pendingHandoff = nil; self.returnOverride = nil
                self.router.restore(y: nil); self.host?.release()
                self.rebuildTopology()
                self.changed()
            }
        })
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            do {
                let blobs = V2KeychainBlobStore()
                let store = V2LocalIdentityStore(blobs: blobs)
                let trust = V2TrustStore(blobs: blobs)
                let identity: V2LocalIdentity
                if let saved = try store.load() {
                    identity = saved
                } else {
                    guard try trust.peers().isEmpty else { throw WireError.invalid("Missing encrypted device identity") }
                    identity = try V2IdentityFactory.generate()
                    try store.saveNew(identity)
                }
                DispatchQueue.main.async { [weak self] in self?.useIdentity(identity, trust: trust) }
            } catch {
                DispatchQueue.main.async { [weak self] in
                    self?.setStatus("Cannot open the encrypted device identity.", "无法读取或创建加密设备身份。")
                }
            }
        }
    }
    private static func displayBounds() -> [V2MacReceiverInjector.Display] {
        ((try? V2MacReceiverInjector.localDisplays()) ?? []).sorted { $0.id < $1.id }
    }

    private func useIdentity(_ identity: V2LocalIdentity, trust: V2TrustStore) {
        guard !quitting else { return }
        self.identity = identity; self.trust = trust
        localName = Host.current().localizedName.flatMap { name in
            V2PairOffer(role: 1, nonce: Array(repeating: 1, count: 16), name: name, certificatePEM: identity.certificatePEM).valid ? name : nil
        } ?? "Hop Mac"
        receiver = V2ReceiverCoordinator(identity: identity, name: localName, trust: trust, clipboardBackend: { MacPasteboardBackend() }) { [weak self] in self?.handleReceiver($0) }
        receiver?.setClipboardEnabled(clipboardEnabled)
        listen()
    }

    private func listen() {
        guard !quitting, !receivingMode, let identity, let trust else { return }
        hostGeneration += 1
        let generation = hostGeneration
        do {
            let coordinator = try V2HostCoordinator(deviceName: localName, identity: identity, trustStore: trust, onEvent: { [weak self] event in
                guard let self, self.hostGeneration == generation else { return }
                self.handle(event)
            },
                restore: { [weak self] position in
                    let restore = { [weak self] in
                        guard let self else { return true }
                        self.router.restore(y: position?.y)
                        return self.input.localCursorRestored
                    }
                    return Thread.isMainThread ? restore() : DispatchQueue.main.sync(execute: restore)
                }, shouldRelease: { [weak self] fingerprint, screen, epoch, position in
                    let route = { [weak self] in self?.decideLeave(fingerprint: fingerprint, screen: screen, epoch: epoch, position: position) ?? true }
                    return Thread.isMainThread ? route() : DispatchQueue.main.sync(execute: route)
                }, clipboardBackend: { MacPasteboardBackend() })
            host = coordinator
            coordinator.setClipboardEnabled(clipboardEnabled)
            coordinator.start()
        } catch { setStatus("Cannot start encrypted sharing.", "无法启动加密共享。") }
    }

    private func handle(_ event: V2HostCoordinator.Event) {
        guard !quitting else { return }
        if receivingMode {
            if case .stopped = event { host = nil; hostStopping = false; changed() }
            return
        }
        switch event {
        case .listening(let port):
            listening = true
            let service = V2Discovery.advertisement(deviceName: localName, port: port)
            advertisement = service
            service.publish(options: .noAutoRename)
            setStatus("Add a computer to share your keyboard and mouse.", "添加电脑，即可共用一套键鼠。")
        case .stopped:
            listening = false
            advertisement?.stop(); advertisement = nil
        case .pairingWindowOpened:
            pairing = true
            setStatus("Select this Mac on the other device to pair.", "请在另一台设备上选择这台 Mac 进行配对。")
        case .comparison(let offer):
            comparison = offer; approved = false
        case .pairingWindowClosed:
            pairing = false; comparison = nil; approved = false
            if peer == nil { setStatus("Waiting for an encrypted connection.", "正在等待加密连接。") }
        case .connected(let peer, let screens):
            if let index = connectedPeers.firstIndex(where: { $0.0.fingerprint == peer.fingerprint }) {
                connectedPeers[index] = (peer, screens)
                if self.peer?.fingerprint == peer.fingerprint {
                    if self.screens != screens && input.mode != .local { router.restore(y: nil); host?.disconnect(); return }
                    setCurrentDevice(index)
                }
            } else {
                connectedPeers.append((peer, screens))
                if self.peer == nil { setCurrentDevice(connectedPeers.count - 1) }
            }
            rebuildTopology()
            setStatus("Computer connected", "电脑已连接")
        case .focused(let focus):
            guard enabled else { router.restore(y: nil); host?.release(); return }
            router.focused(epoch: focus.epoch, screen: focus.screen, x: focus.x, y: focus.y, captureEpoch: focus.captureEpoch)
            if router.isActive { host?.activateClipboard(focusEpoch: focus.epoch) }
        case .localFocus:
            break // The synchronous restore callback already returned input.
        case .focusReleased(let epoch):
            completeHandoff(epoch)
        case .disconnected(let fingerprint):
            connectedPeers.removeAll { $0.0.fingerprint == fingerprint }
            if pendingHandoff?.route.target.device == fingerprint || pendingHandoff?.route.source.device == fingerprint { pendingHandoff = nil }
            if peer?.fingerprint == fingerprint {
                router.restore(y: nil); router.selectedScreen = 0
                peer = nil; screens = []; target = 0
                if !connectedPeers.isEmpty { setCurrentDevice(0) }
                setStatus("Device disconnected.", "设备已断开。")
            }
            rebuildTopology()
        case .forgotten:
            setStatus("Device forgotten. Pair again to reconnect.", "已解除配对。再次连接时需要重新配对。")
        case .failed(let reason):
            hopLog("Secure host: \(reason)")
            setStatus("Encrypted connection could not be established or was closed.", "无法建立加密连接，或连接已关闭。")
        }
        changed()
    }

    func quit() {
        guard !quitting else { return }
        quitting = true; enabled = false
        router.restore(y: nil)
        input.stop(drain: false)
        moves.stop(); maintenance?.invalidate(); maintenance = nil
        discovery.stop()
        advertisement?.stop(); advertisement = nil
        host?.stop(); host = nil
        receiver?.drainOnQuit(); receiver = nil
        _ = injector?.restore()
        for observer in observers {
            NSWorkspace.shared.notificationCenter.removeObserver(observer)
            NotificationCenter.default.removeObserver(observer)
        }
        observers.removeAll()
    }

    private func setStatus(_ english: String, _ chinese: String) {
        status = HopLocale.text(english, chinese); changed()
    }
    private func refreshMaintenance() {
        let needed = enabled || !input.localCursorRestored
        if !needed { maintenance?.invalidate(); maintenance = nil; return }
        guard maintenance == nil else { return }
        let timer = Timer(timeInterval: 1 / 120.0, repeats: true) { [weak self] _ in
            guard let self else { return }
            self.input.tick()
            if !self.enabled && self.input.localCursorRestored { self.refreshMaintenance() }
        }
        maintenance = timer
        RunLoop.main.add(timer, forMode: .common)
    }
    private func suspend(_ message: String) {
        pendingHandoff = nil; returnOverride = nil
        discovery.stop()
        router.restore(y: nil); input.stop(); enabled = false
        host?.disconnect()
        if receivingMode { stopReceiver() }
        refreshMaintenance()
        status = message; changed()
    }
    func setEnabled(_ value: Bool) {
        guard !quitting, !receivingMode, !transitioning else { return }
        if value {
            do { try input.start(); enabled = true }
            catch { enabled = false; status = String(describing: error); PermissionDock.shared.show() }
        } else {
            pendingHandoff = nil; returnOverride = nil
            router.restore(y: nil); input.stop(); enabled = false; host?.release()
        }
        refreshMaintenance()
        changed()
    }
    func setDirection(_ value: UInt8) {
        guard value <= 1, value != input.direction else { return }
        if input.mode != .local { router.restore(y: nil); host?.disconnect() }
        input.direction = value
        UserDefaults.standard.set(value == 0 ? "left" : "right", forKey: "HopV2Direction")
        changed()
    }
    func selectScreen(_ index: Int) {
        guard screens.indices.contains(index), input.mode == .local else { return }
        target = index; router.selectedScreen = screens[index].id; changed()
    }
    func selectDevice(_ index: Int) {
        guard input.mode == .local else { return }
        setCurrentDevice(index); changed()
    }
    private func setCurrentDevice(_ index: Int) {
        guard connectedPeers.indices.contains(index) else { return }
        let (newPeer, newScreens) = connectedPeers[index]
        let oldID = peer?.fingerprint == newPeer.fingerprint ? router.selectedScreen : 0
        peer = newPeer; screens = newScreens
        target = newScreens.firstIndex(where: { $0.id == oldID }) ?? 0
        router.selectedScreen = newScreens.isEmpty ? 0 : newScreens[target].id
        applyTuning(MouseTuning.load(fingerprint: newPeer.fingerprint))
    }
    func beginPairing() { host?.openPairingWindow() }
    func confirmPairing() {
        if receivingMode {
            guard receiverComparison != nil, !approved else { return }
            approved = true
            receiver?.approveComparison()
            changed()
            return
        }
        guard let comparison, !approved else { return }
        approved = true
        host?.approve(comparison, peerOnLocal: comparison.peerFingerprint, localOnPeer: comparison.localFingerprint)
        changed()
    }
    func rejectPairing() { if receivingMode { disconnectReceiver() } else { host?.cancelPairing() } }
    func unpair() {
        if receivingMode {
            guard let receivingPeer, let trust else { return }
            stopReceiver()
            drainReceiver(operation: { try trust.revoke(fingerprint: receivingPeer.fingerprint) }) { [weak self] in
                self?.refreshKnownHosts()
            }
            return
        }
        guard let peer else { return }
        router.restore(y: nil)
        host?.forgetPeer(fingerprint: peer.fingerprint)
    }
    func requestPermissions() { InputCapture.requestPermission(); PermissionDock.shared.show(); changed() }
    func setMouseTuning(_ tuning: MouseTuning) {
        guard let peer, tuning.save(fingerprint: peer.fingerprint) else { return }
        applyTuning(tuning); changed()
    }
    func setClipboardEnabled(_ enabled: Bool) {
        clipboardEnabled = enabled
        UserDefaults.standard.set(enabled, forKey: "HopV2ClipboardEnabled")
        host?.setClipboardEnabled(enabled)
        receiver?.setClipboardEnabled(enabled)
        changed()
    }
    private func applyTuning(_ tuning: MouseTuning) {
        let oldRate = input.tuning.reportRateHz
        input.tuning = tuning
        if oldRate != tuning.reportRateHz { moves.rateChanged() }
    }

    private func rebuildTopology() {
        let local = screenSignature.enumerated().compactMap { index, display -> V2DesktopLayout.Display? in
            // Mirrored outputs share one physical rectangle.
            guard !screenSignature.prefix(index).contains(where: { $0.bounds == display.bounds }) else { return nil }
            return .init(key: .init(device: "local", screen: display.id), label: HopLocale.text("This Mac", "这台 Mac"), frame: display.bounds, physical: display.bounds)
        }
        let remotes = connectedPeers.map { peer, screens in
            screens.enumerated().compactMap { index, screen -> V2DesktopLayout.Display? in
                guard !screens.prefix(index).contains(where: { $0.x == screen.x && $0.y == screen.y && $0.width == screen.width && $0.height == screen.height }) else { return nil }
                let bounds = CGRect(x: Int(screen.x), y: Int(screen.y), width: Int(screen.width), height: Int(screen.height))
                return .init(key: .init(device: peer.fingerprint, screen: screen.id), label: peer.name, frame: bounds, physical: bounds)
            }
        }
        topology = try? V2DesktopLayout.arrange(local, remotes, placements: placements ?? [:])
        if usesLayout {
            input.routedEdge = { [weak self] point, margin in self?.topology?.sourceEdge(point, margin: margin) != nil }
            input.routedEntry = { [weak self] point, movement in
                guard let self, let route = self.topology?.sourceEdge(point, margin: 6, movement: movement) else { return nil }
                let frame = self.topology?.displays.first(where: { $0.key == route.source })?.physical
                let anchor = frame.map { route.entryEdge.opposite.inset(point, within: $0) } ?? point
                return self.plan(route, anchor: anchor) { [weak self] current, margin in
                    self?.topology?.sourceEdge(current, margin: margin, matching: route) != nil
                }
            }
        } else {
            input.routedEdge = nil; input.routedEntry = nil
        }
    }

    private func plan(_ route: V2DesktopLayout.Route, anchor: CGPoint, near: @escaping (CGPoint, CGFloat) -> Bool) -> InputCapture.RoutedEntry? {
        guard let device = connectedPeers.firstIndex(where: { $0.0.fingerprint == route.target.device }),
              let target = connectedPeers[device].1.firstIndex(where: { $0.id == route.target.screen }) else { return nil }
        setCurrentDevice(device)
        self.target = target; router.selectedScreen = route.target.screen
        let screen = screens[target]
        return InputCapture.RoutedEntry(width: screen.width, height: screen.height, x: route.x, y: route.y, returnPoint: anchor, stillNear: near)
    }

    private func decideLeave(fingerprint: String, screen: UInt64, epoch: UInt64, position: V2HostCoordinator.Position) -> Bool {
        guard enabled, !quitting else { return false }
        guard usesLayout else { return input.matchesRemoteEdge(x: position.x, y: position.y) }
        guard let route = topology?.remoteEdge(.init(device: fingerprint, screen: screen), x: position.x, y: position.y,
                                               accepts: { edge in
            input.matchesRemoteEdge(x: position.x, y: position.y, horizontalBoundary: edge.horizontalBoundary)
        }) else { return false }
        if route.target.local {
            returnOverride = route.localEntryPoint
            hopLog("crossing accepted: return to Mac on \(route.entryEdge)")
            return true
        }
        guard input.crossingKeysAreUp, let anchor = input.routingAnchor else { return false }
        pendingHandoff = (epoch, route, anchor, input.physicalPressSequence)
        return true
    }

    private func completeHandoff(_ epoch: UInt64) {
        guard let pending = pendingHandoff, pending.epoch == epoch else { return }
        pendingHandoff = nil
        guard enabled, !quitting, !receivingMode, !editingLayout, input.mode == .local, input.physicalPressSequence == pending.presses,
              let plan = plan(pending.route, anchor: pending.anchor, near: { p, limit in
                  max(abs(p.x - pending.anchor.x), abs(p.y - pending.anchor.y)) <= limit
              }) else { return }
        input.continueAt(plan)
    }

    func editLayout() {
        guard let topology, topology.displays.contains(where: { !$0.key.local }), input.mode == .local, pendingHandoff == nil, !receivingMode, !editingLayout else { return }
        let snapshot = topology.displays
        editingLayout = true; defer { editingLayout = false }
        let alert = NSAlert()
        alert.messageText = HopLocale.text("Arrange your computers", "排列你的电脑")
        alert.addButton(withTitle: HopLocale.text("Save layout", "保存布局"))
        alert.addButton(withTitle: HopLocale.text("Cancel", "取消"))
        let mainDisplay = screenSignature.first(where: { $0.nativeID == CGMainDisplayID() })
        let canvas = V2LayoutCanvas(layout: topology, anchor: mainDisplay.map { .init(device: "local", screen: $0.id) })
        alert.accessoryView = canvas
        guard alert.runModal() == .alertFirstButtonReturn, !quitting else { return }
        guard snapshot == self.topology?.displays else {
            setStatus("Displays changed. Reopen the layout to make changes.", "显示器已变化，请重新打开空间布局。")
            return
        }
        if let data = try? canvas.layout.encode(keeping: placements ?? [:]), let placed = V2DesktopLayout.load(data) {
            placements = placed; UserDefaults.standard.set(data, forKey: layoutKey); rebuildTopology(); changed()
        }
    }

    func setReceivingMode(_ value: Bool) {
        guard !quitting, !transitioning, !hostStopping, value != receivingMode, receiver != nil else { return }
        if value {
            router.restore(y: nil); router.selectedScreen = 0; input.stop(); enabled = false
            refreshMaintenance()
            hostStopping = host != nil; host?.stop(); advertisement?.stop(); advertisement = nil
            peer = nil; connectedPeers = []; screens = []; target = 0; listening = false; pairing = false; comparison = nil
            receivingMode = true
            refreshKnownHosts()
            setStatus("Choose a host to receive keyboard and mouse input.", "选择一台主机以接收键鼠输入。")
        } else {
            stopReceiver()
            drainReceiver { [weak self] in
                guard let self else { return }
                self.receivingMode = false; self.injector = nil
                self.setStatus("Starting encrypted sharing…", "正在启动加密共享…")
                self.listen()
            }
        }
    }

    func connectReceiver() {
        guard receivingMode, !receiverBusy, !transitioning, !hostStopping, let receiver else { return }
        guard InputCapture.permissionGranted, CGPreflightPostEventAccess() else { requestPermissions(); return }
        let generation = receiverGeneration
        let known = knownHosts
        let alert = NSAlert()
        alert.messageText = HopLocale.text("Connect to a keyboard and mouse host", "连接键鼠主机")
        alert.informativeText = HopLocale.text("Choose a nearby host or enter its local IP address and port. For a new pairing, open pairing on the host and confirm there first.", "选择附近的主机，或填写局域网 IP 地址和端口。首次配对时，请先在主机开启配对，并在主机端先确认指纹。")
        alert.addButton(withTitle: HopLocale.text("Connect", "连接"))
        alert.addButton(withTitle: HopLocale.text("Cancel", "取消"))
        let fields = V2HostConnectionFields(saved: known, host: receiverAddress, port: receiverPort)
        alert.accessoryView = fields
        discovery.onHosts = { [weak fields] in fields?.updateNearby($0) }
        discovery.onError = { [weak fields] _ in fields?.showDiscoveryError() }
        discovery.start()
        defer { discovery.onHosts = nil; discovery.onError = nil; discovery.stop() }
        guard alert.runModal() == .alertFirstButtonReturn, receivingMode, !quitting, generation == receiverGeneration else { return }
        guard let destination = fields.destination else {
            setStatus("Enter a valid local IP address and port.", "请填写有效的局域网 IP 地址和端口。")
            return
        }
        let index = fields.savedPeerIndex
        let fingerprint = known.indices.contains(index) ? known[index].fingerprint : nil
        receiverAddress = destination.host; receiverPort = destination.port
        drainReceiver { [weak self] in
            guard let self, self.receivingMode, !self.quitting else { return }
            do {
                guard let backend = V2QuartzInputBackend(localKeysAreUp: { [weak input = self.input] in
                    input?.receiverPhysicalInputIsNeutral == true
                }) else { throw WireError.invalid("Cannot create a Quartz event source") }
                let injector = try V2MacReceiverInjector(displays: V2MacReceiverInjector.localDisplays(), keymap: self.keymap, backend: backend,
                                                         doubleClickInterval: NSEvent.doubleClickInterval)
                try self.input.start() // Arms the physical emergency shortcut; mayEnter is false here.
                self.input.armReceiverSafety()
                self.injector = injector
                self.receiverBusy = receiver.connect(address: destination, fingerprint: fingerprint,
                    input: V2ReceiverCoordinator.Input(screenIDs: injector.screenIDs, screenAdvertisement: injector.screenAdvertisement,
                        enter: { injector.enter($0, epoch: $1) }, input: { injector.input($0, epoch: $1) },
                        restore: injector.restore, takeLeave: { injector.takeLeave().map { ($0.x, $0.y) } }))
                if !self.receiverBusy { self.input.stop() }
                self.changed()
            } catch {
                self.input.stop()
                self.setStatus("Could not prepare local input. Check permissions and displays.", "无法准备本机输入，请检查权限和显示器。")
            }
        }
    }

    func disconnectReceiver() {
        stopReceiver()
        setStatus("Disconnected from the host.", "已与键鼠主机断开连接。")
    }

    private func stopReceiver() {
        receiverGeneration += 1; transitioning = false
        receiver?.stop()
        input.stop()
        receiverBusy = false; receivingPeer = nil; receiverComparison = nil; approved = false
    }

    private func drainReceiver(operation: @escaping () throws -> Void = {}, _ completion: @escaping () -> Void) {
        guard let receiver else { return }
        receiverGeneration += 1
        let generation = receiverGeneration
        transitioning = true; changed()
        let injector = self.injector
        receiver.stop(after: {
            guard injector?.restore() ?? true else { return false }
            do { try operation(); return true } catch { return false }
        }) { [weak self] neutral in
            guard let self, !self.quitting, generation == self.receiverGeneration else { return }
            self.transitioning = false
            if neutral { completion(); self.changed() }
            else { self.setStatus("Could not safely complete this action. Check input permission and try again.", "无法安全完成操作，请检查输入权限后重试。") }
        }
    }

    private func refreshKnownHosts() {
        guard let trust else { return }
        DispatchQueue.global(qos: .utility).async { [weak self] in
            let peers = (try? trust.peers()) ?? []
            DispatchQueue.main.async { [weak self] in self?.knownHosts = peers; self?.changed() }
        }
    }

    private func handleReceiver(_ event: V2ReceiverCoordinator.Event) {
        guard receivingMode, !quitting else { return }
        switch event {
        case .connecting:
            receiverComparison = nil; approved = false
            setStatus("Connecting to an encrypted host…", "正在连接加密主机…")
        case .compare(let comparison):
            receiverComparison = comparison; approved = false
            setStatus("Compare the complete fingerprints and confirm on the host first.", "请核对完整指纹，并先在主机端确认。")
        case .ready(let peer):
            receivingPeer = peer; receiverComparison = nil; approved = false
            refreshKnownHosts()
            setStatus("Connected. Waiting for input from the host.", "已连接，正在等待主机键鼠输入。")
        case .receiving:
            setStatus("Receiving keyboard and mouse input.", "正在接收键鼠输入。")
        case .closed, .failed:
            stopReceiver()
            setStatus("The host disconnected or could not be verified.", "主机已断开连接，或无法验证主机身份。")
        }
        changed()
    }

    var popoverPresentation: PopoverPresentation {
        let headline = input.mode == .remote ? "Controlling remote computer" :
            input.mode == .pending ? "Waiting for remote permission / ENTER acknowledgement" : status
        var fingerprints: String?
        if let compared = receivingMode ? receiverComparison.map({ ($0.localFingerprint, $0.peerFingerprint, $0.peerName) }) :
            comparison.map({ ($0.localFingerprint, $0.peerFingerprint, $0.peerName) }) {
            let local = HopLocale.text("This Mac", "这台 Mac")
            let remote = HopLocale.text("Other device", "另一台设备")
            func rows(_ value: String) -> String {
                String(value.prefix(32)) + "\n" + String(value.suffix(32))
            }
            fingerprints = "\(local):\n\(rows(compared.0))\n\(remote) · \(compared.2):\n\(rows(compared.1))"
        }
        let labels = screens.enumerated().map { i, screen in
            HopLocale.text("Display \(i + 1) · \(screen.width)×\(screen.height)", "显示器 \(i + 1) · \(screen.width)×\(screen.height)")
        }
        return PopoverPresentation(status: headline, pairedName: receivingMode ? receivingPeer?.name : peer?.name, enabled: enabled, direction: input.direction,
                                   permissionGranted: InputCapture.permissionGranted, localConfirmed: approved,
                                   pairingAllowed: pairing, mouseTuning: input.tuning, encrypted: true,
                                   pairingAvailable: listening, fingerprints: fingerprints, screenNames: labels, selectedScreen: target,
                                   clipboardAvailable: true, clipboardEnabled: clipboardEnabled,
                                   receivingAvailable: receiver != nil, receivingMode: receivingMode, canConnectReceiver: !transitioning && !hostStopping,
                                   receiverBusy: receiverBusy,
                                   deviceNames: connectedPeers.map { "\($0.0.name) · \($0.0.fingerprint.prefix(8))" },
                                   selectedDevice: connectedPeers.firstIndex(where: { $0.0.fingerprint == peer?.fingerprint }) ?? 0,
                                   layoutAvailable: topology?.displays.contains(where: { !$0.key.local }) == true, usesLayout: usesLayout,
                                   arrangingAllowed: input.mode == .local && pendingHandoff == nil && !editingLayout,
                                   pairedFingerprint: receivingMode ? receivingPeer?.fingerprint : peer?.fingerprint)
    }

    func requestV2Entry(screen: UInt64, x: UInt16, y: UInt16, captureEpoch: UInt32) { host?.enter(peerFingerprint: peer?.fingerprint, screen: screen, x: x, y: y, captureEpoch: captureEpoch) }
    func submitV2Input(_ payloads: [[UInt8]], focusEpoch: UInt64) -> Bool {
        host?.submitInput(payloads, focusEpoch: focusEpoch) == true
    }
    func requestV2Release() { host?.release() }
    func acknowledgeV2Capture(_ message: Message) throws { try input.acknowledge(message) }
    func restoreV2Capture(y: UInt16?) {
        input.restore(y: usesLayout ? nil : y, point: returnOverride)
        returnOverride = nil
    }
    func abortV2Session() { host?.abortFocus() }
}

private final class V2HostConnectionFields: NSView {
    private let nearby = NSPopUpButton(frame: NSRect(x: 0, y: 81, width: 318, height: 26), pullsDown: false)
    private let saved = NSPopUpButton(frame: NSRect(x: 0, y: 49, width: 318, height: 26), pullsDown: false)
    private let address = NSTextField(frame: NSRect(x: 0, y: 16, width: 233, height: 25))
    private let port = NSTextField(frame: NSRect(x: 243, y: 16, width: 75, height: 25))
    private var hosts: [V2DiscoveredHost] = []

    init(saved peers: [V2VerifiedPeer], host: String, port: UInt16) {
        super.init(frame: NSRect(x: 0, y: 0, width: 325, height: 112))
        address.placeholderString = "192.168.1.10"; address.stringValue = host
        address.setAccessibilityLabel(HopLocale.text("Host IP address", "主机 IP 地址"))
        self.port.stringValue = String(port)
        self.port.setAccessibilityLabel(HopLocale.text("Host port", "主机端口"))
        saved.addItems(withTitles: [HopLocale.text("Pair a new host", "配对新主机")] + peers.map { "\($0.name) · \($0.fingerprint.prefix(8))" })
        saved.setAccessibilityLabel(HopLocale.text("Trusted host", "已配对的主机"))
        nearby.setAccessibilityLabel(HopLocale.text("Nearby hosts", "附近的主机"))
        nearby.target = self; nearby.action = #selector(chooseNearby)
        for field in [nearby, saved, address, self.port] { addSubview(field) }
        updateNearby([])
    }
    required init?(coder: NSCoder) { fatalError("Programmatic connection fields") }
    var savedPeerIndex: Int { saved.indexOfSelectedItem - 1 }
    var destination: V2ReceiverAddress? {
        guard let number = UInt16(port.stringValue.trimmingCharacters(in: .whitespaces)) else { return nil }
        return try? V2ReceiverAddress(host: address.stringValue, port: number)
    }
    func updateNearby(_ values: [V2DiscoveredHost]) {
        hosts = values
        nearby.removeAllItems()
        nearby.addItems(withTitles: [HopLocale.text("Choose a nearby host…", "选择附近的主机…")] + values.map { "\($0.name) · \($0.host):\($0.port)" })
        nearby.isEnabled = !values.isEmpty
    }
    func showDiscoveryError() {
        updateNearby([])
        nearby.item(at: 0)?.title = HopLocale.text("Discovery unavailable; enter an address", "无法自动发现，可手动填写地址")
    }
    @objc private func chooseNearby() {
        let index = nearby.indexOfSelectedItem - 1
        guard hosts.indices.contains(index) else { return }
        let host = hosts[index]
        address.stringValue = host.host; port.stringValue = String(host.port)
    }
}
