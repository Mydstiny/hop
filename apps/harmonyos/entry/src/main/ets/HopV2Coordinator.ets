import { abilityAccessCtrl, common, Permissions } from '@kit.AbilityKit';
import { util } from '@kit.ArkTS';
import { display } from '@kit.ArkUI';
import { preferences } from '@kit.ArkData';
import { hilog } from '@kit.PerformanceAnalysisKit';
import hopv2, { V2Event, OfferConfig, ReceiverConfig, ReceiverDisplay, ClipboardStatus } from 'libhopv2.so';
import { HopV2IdentityManager } from './HopV2Identity';
import { V2TrustedPeer } from './HopV2Vault';
import { HopV2Discovery, V2NearbyPeer } from './HopV2Discovery';
import { ui } from './HopI18n';
import { ClipboardPacket, HopClipboard } from './HopClipboard';

@Observed
export class HopV2ViewState {
  state: string = 'disconnected';
  message: string = '';
  localFingerprint: string = '';
  peerFingerprint: string = '';
  peerName: string = '';
  peers: V2NearbyPeer[] = [];
  discoveryFailed: boolean = false;
  trusted: V2TrustedPeer[] = [];
  clipboardEnabled: boolean = true;
  clipboardAuthorized: boolean = false;
  clipboardMessage: string = '';
  displayCount: number = 1;
}
export class HopV2Coordinator {
  static readonly instance: HopV2Coordinator = new HopV2Coordinator();
  readonly view: HopV2ViewState = new HopV2ViewState();
  private manager: HopV2IdentityManager = new HopV2IdentityManager();
  private discovery: HopV2Discovery = new HopV2Discovery();
  private initialized: boolean = false;
  private generation: number = 0;
  private host: string = '';
  private port: number = 24891;
  private pending?: V2TrustedPeer;
  private approved: boolean = false;
  private saving: boolean = false;
  private clipboard?: HopClipboard;
  private clipboardTimer: number = -1;
  private clipboardEpoch: string = '';
  private clipboardBusy: boolean = false;
  private clipboardLinkEnabled: boolean = false;
  private clipboardPermissionPending: boolean = false;
  private readonly clipboardPermission: Permissions = 'ohos.permission.READ_PASTEBOARD';
  private settings?: preferences.Preferences;
  private context?: common.Context;
  private watchingDisplays: boolean = false;
  private displayLease: number = 0;
  private displaySignature: string = '';
  private displayTimer: number = -1;
  private retryTimer: number = -1;
  private retryCount: number = 0;
  private readySince: number = 0;
  private trustedConnection: boolean = false;
  private recoverableClose: boolean = false;
  private lastPeer?: V2TrustedPeer;
  private lastHost: string = '';
  private lastPort: number = 24891;
  private readonly displayChanged = (_id: number): void => {
    if (this.displayLease === 0 || this.displayLease !== this.generation) { return; }
    if (this.displayTimer >= 0) { clearTimeout(this.displayTimer); }
    const generation = this.generation;
    this.displayTimer = setTimeout(() => { this.displayTimer = -1; this.checkDisplays(generation); }, 250);
  };

  private signature(screens: ReceiverDisplay[]): string {
    return screens.map((s: ReceiverDisplay) => `${s.displayId}:${s.x}:${s.y}:${s.width}:${s.height}`).sort().join('|');
  }

  private async checkDisplays(generation: number): Promise<void> {
    if (generation !== this.generation || this.displayLease !== generation || !this.displaySignature) { return; }
    try {
      const screens = await this.receiverDisplays();
      if (generation !== this.generation || this.displayLease !== generation || this.signature(screens) === this.displaySignature) { return; }
      const peer = this.trustedConnection ? this.pending : undefined;
      this.disconnect();
      if (peer) { this.pending = peer; this.trustedConnection = true; this.scheduleRetry(); }
      else { this.view.message = ui('Displays changed. Connect again to refresh the layout.', '显示器配置已变化，请重新连接。'); }
    } catch (_) { hilog.warn(0, 'Hop', 'Could not refresh display geometry'); }
  }

  canReconnect(peer: V2TrustedPeer): boolean { return this.lastPeer?.fingerprint === peer.fingerprint && this.lastHost.length > 0; }
  reconnect(peer: V2TrustedPeer, legacyDisconnected: boolean, authorized: boolean, background: boolean): void {
    if (this.canReconnect(peer)) { this.connectTrusted(peer, this.lastHost, this.lastPort, legacyDisconnected, authorized, background); }
  }

  private scheduleRetry(): void {
    const peer = this.pending;
    if (!this.trustedConnection || !peer || this.retryTimer >= 0 || this.retryCount >= 3) { return; }
    const generation = this.generation;
    const delay = 700 * Math.pow(2, this.retryCount++);
    this.view.state = 'reconnecting';
    this.view.message = ui('Connection interrupted. Reconnecting…', '连接中断，正在重连…');
    this.retryTimer = setTimeout(() => {
      this.retryTimer = -1;
      if (generation === this.generation && this.trustedConnection) { this.connectPeer(peer, false); }
    }, delay);
  }

  private cancelRetry(): void {
    if (this.retryTimer >= 0) { clearTimeout(this.retryTimer); this.retryTimer = -1; }
    this.retryCount = 0; this.readySince = 0; this.trustedConnection = false; this.recoverableClose = false;
  }

  private watchDisplays(): void {
    try {
      display.on('add', this.displayChanged);
      display.on('remove', this.displayChanged);
      display.on('change', this.displayChanged);
      this.watchingDisplays = true;
    } catch (_) { this.stopWatchingDisplays(); }
  }

  private stopWatchingDisplays(): void {
    this.watchingDisplays = false;
    try { display.off('add', this.displayChanged); } catch (_) {}
    try { display.off('remove', this.displayChanged); } catch (_) {}
    try { display.off('change', this.displayChanged); } catch (_) {}
  }

  private validDisplay(screen: display.Display): boolean {
    return Number.isInteger(screen.id) && screen.id >= 0 && screen.id <= 2147483647 &&
      Number.isInteger(screen.width) && screen.width > 0 && screen.width <= 65535 &&
      Number.isInteger(screen.height) && screen.height > 0 && screen.height <= 65535;
  }

  private async receiverDisplays(): Promise<ReceiverDisplay[]> {
    const primary = display.getPrimaryDisplaySync();
    if (!this.validDisplay(primary)) { throw new Error('Invalid primary display'); }
    const first: ReceiverDisplay = { displayId: primary.id, x: 0, y: 0, width: primary.width, height: primary.height };
    const fallback: ReceiverDisplay[] = [first];
    if (!this.watchingDisplays) { throw new Error('Cannot monitor display changes'); }
    try {
      const all = await display.getAllDisplays();
      if (all.length < 2 || all.length > 64) { return fallback; }
      const screens: ReceiverDisplay[] = [first];
      let seenPrimary = false;
      for (const screen of all) {
        if (!this.validDisplay(screen)) { return fallback; }
        const origin = display.convertRelativeToGlobalCoordinate({ displayId: screen.id, position: { x: 0, y: 0 } });
        const last = display.convertRelativeToGlobalCoordinate({ displayId: screen.id,
          position: { x: screen.width - 1, y: screen.height - 1 } });
        const right = origin.x + screen.width;
        const bottom = origin.y + screen.height;
        if (!Number.isInteger(origin.x) || !Number.isInteger(origin.y) || origin.x < -2147483648 || origin.y < -2147483648 ||
          right > 2147483647 || bottom > 2147483647 || last.x !== right - 1 || last.y !== bottom - 1) { return fallback; }
        if (screen.id === first.displayId) {
          if (seenPrimary || origin.x !== 0 || origin.y !== 0 || screen.width !== first.width || screen.height !== first.height) {
            return fallback;
          }
          seenPrimary = true;
          continue;
        }
        for (const known of screens) {
          const overlaps = origin.x < known.x + known.width && known.x < right &&
            origin.y < known.y + known.height && known.y < bottom;
          if (screen.id === known.displayId || overlaps) { return fallback; }
        }
        const geometry: ReceiverDisplay = { displayId: screen.id, x: origin.x, y: origin.y, width: screen.width, height: screen.height };
        screens.push(geometry);
      }
      return seenPrimary ? screens : fallback;
    } catch (_) { return fallback; }
  }

  private refreshClipboardPermission(): void {
    try {
      this.view.clipboardAuthorized = this.context !== undefined &&
        abilityAccessCtrl.createAtManager().checkAccessTokenSync(
          this.context.applicationInfo.accessTokenId, this.clipboardPermission) === abilityAccessCtrl.GrantStatus.PERMISSION_GRANTED;
    } catch (_) { this.view.clipboardAuthorized = false; }
  }

  private pauseClipboard(): void {
    this.clipboardLinkEnabled = false;
    const state = hopv2.clipboardStatus(this.generation);
    if (state && state.active) { hopv2.clipboardAction(this.generation, state.epoch, 'pause'); }
    this.clearClipboardFocus();
    // Keep polling to acknowledge the native drain after an in-flight clipboard write.
  }

  setClipboardEnabled(enabled: boolean): void {
    if (enabled === this.view.clipboardEnabled) { return; }
    this.view.clipboardEnabled = enabled;
    if (!enabled) { this.pauseClipboard(); }
    this.refreshClipboardPermission();
    this.view.clipboardMessage = enabled && this.view.clipboardAuthorized ?
      ui('Applies on the next connection.', '重新连接后生效。') : '';
    try {
      if (!this.settings) { throw new Error('Settings unavailable'); }
      this.settings.putSync('clipboardEnabled', enabled);
      this.settings.flushSync();
    } catch (_) {
      this.view.clipboardMessage = ui('This change could not be saved.', '暂时无法保存此设置。');
    }
  }

  async authorizeClipboard(context: common.UIAbilityContext): Promise<void> {
    if (!this.view.clipboardEnabled || this.clipboardPermissionPending) { return; }
    this.clipboardPermissionPending = true;
    try {
      await abilityAccessCtrl.createAtManager().requestPermissionsFromUser(context, [this.clipboardPermission]);
    } catch (_) {}
    finally { this.clipboardPermissionPending = false; }
    this.refreshClipboardPermission();
    if (!this.view.clipboardEnabled) { return; }
    this.view.clipboardMessage = this.view.clipboardAuthorized ?
      ui('Applies on the next connection.', '重新连接后生效。') :
      ui('Allow pasteboard access in System Settings.', '请在系统设置中允许读取剪贴板。');
  }

  private startClipboardTimer(): void {
    if (this.clipboardTimer < 0) {
      this.clipboardTimer = setInterval(() => { this.clipboardTick(); }, 80);
    }
  }
  private stopClipboardTimer(): void {
    if (this.clipboardTimer >= 0) { clearInterval(this.clipboardTimer); this.clipboardTimer = -1; }
    this.clipboardLinkEnabled = false;
    this.clearClipboardFocus();
  }
  private clearClipboardFocus(): void {
    this.clipboardEpoch = '';
    if (this.clipboard) { this.clipboard.configure(false, false, '', ''); }
  }
  private async clipboardTick(): Promise<void> {
    const request = this.generation;
    const state = hopv2.clipboardStatus(request);
    if (!state) { return; }
    if (!state.active) {
      if (this.clipboard && this.clipboardEpoch.length > 0) {
        this.clipboard.configure(false, false, '', '');
        this.clipboardEpoch = '';
      }
      if (state.draining && !this.clipboardBusy) { hopv2.clipboardAction(request, state.epoch, 'drained'); }
      return;
    }
    if (!this.clipboardLinkEnabled) { hopv2.clipboardAction(request, state.epoch, 'pause'); return; }
    if (this.clipboardBusy) { return; }
    this.clipboardBusy = true;
    try { await this.clipboardWork(request, state); }
    catch (_) {
      hopv2.clipboardAction(request, state.epoch, 'pause');
      if (request === this.generation) { this.view.clipboardMessage = ui('Clipboard paused for this focus.', '本次控制的剪贴板同步已暂停。'); }
    } finally { this.clipboardBusy = false; }
  }
  private async clipboardWork(request: number, state: ClipboardStatus): Promise<void> {
    const peer = this.view.peerFingerprint;
    const epoch = state.epoch;
    if (peer.length !== 64) { return; }
    if (!this.clipboard) { this.clipboard = new HopClipboard(); }
    const adapter = this.clipboard;
    if (this.clipboardEpoch !== epoch) {
      adapter.configure(true, true, peer, epoch, state.baseline);
      if (!hopv2.clipboardAction(request, epoch, 'activate')) { return; }
      this.clipboardEpoch = epoch;
    }
    const permitted = (): boolean => request === this.generation && this.clipboardLinkEnabled &&
      hopv2.clipboardAction(request, epoch, 'permitted');
    if (!permitted()) { return; }
    const transfer = hopv2.takeClipboard(request, epoch);
    if (transfer) {
      try {
        const packet = new ClipboardPacket();
        if (transfer.format === 1) {
          packet.kind = 'text'; packet.text = util.TextDecoder.create('utf-8', { fatal: true, ignoreBOM: true }).decodeToString(transfer.bytes);
        } else {
          packet.kind = 'png'; packet.png = transfer.bytes.buffer as ArrayBuffer;
        }
        await adapter.apply(packet, peer, epoch, transfer.baseline, permitted);
      } finally {
        transfer.bytes.fill(0);
        hopv2.clipboardAction(request, epoch, 'finish');
      }
    } else if (state.poll) {
      const packet = await adapter.poll(peer, epoch, permitted);
      if (packet && permitted()) {
        const bytes = packet.kind === 'text' ? util.TextEncoder.create('utf-8').encodeInto(packet.text) : new Uint8Array(packet.png);
        try { hopv2.submitClipboard(request, epoch, packet.kind === 'text' ? 1 : 2, bytes); }
        finally { bytes.fill(0); }
      }
    }
    if (!adapter.healthy() && permitted()) { throw new Error('Clipboard unavailable'); }
  }
  initialize(context: common.Context): void {
    if (!this.initialized) {
      this.context = context;
      try {
        this.settings = preferences.getPreferencesSync(context, { name: 'hop-v2' });
        this.view.clipboardEnabled = this.settings.getSync('clipboardEnabled', true) === true;
      } catch (_) { this.view.clipboardEnabled = false; }
      hopv2.listen((event: V2Event) => { this.handle(event); });
      this.discovery.onPeers = (peers: V2NearbyPeer[]) => {
        this.view.peers = peers;
        if (peers.length > 0) { this.view.discoveryFailed = false; }
      };
      this.discovery.onError = () => { this.view.discoveryFailed = true; };
      this.watchDisplays();
      this.initialized = true;
      this.refreshTrusted();
    }
    this.refreshClipboardPermission();
    this.discovery.start(context);
  }
  async refreshTrusted(): Promise<void> {
    try { this.view.trusted = await this.manager.vault.listPeers(); }
    catch (_) { this.view.message = ui('Secure device list unavailable.', '无法读取安全设备列表。'); }
  }
  async offer(host: string, port: number): Promise<void> {
    const address = host.trim();
    if (!this.initialized || !address || address.length > 255 || port < 1 || port > 65535) {
      this.view.message = ui('Enter a valid address and port.', '请输入有效的地址和端口。'); return;
    }
    const generation = ++this.generation; this.host = address; this.port = port;
    this.cancelRetry();
    this.pending = undefined; this.approved = false; this.view.state = 'offering';
    this.view.message = ui('Requesting device fingerprints…', '正在获取设备指纹…');
    try {
      const identity = await this.manager.loadOrCreate();
      try {
        if (generation !== this.generation) { return; }
        this.view.localFingerprint = hopv2.fingerprint(identity.certificatePEM);
        const request: OfferConfig = { host: address, port: port, name: 'HarmonyOS PC', certificatePEM: identity.certificatePEM, requestId: generation };
        hopv2.offer(request);
      } finally { identity.privateKeyPEM.fill(0); }
    } catch (_) { if (generation === this.generation) { this.view.state = 'error'; this.view.message = ui('Pairing request failed.', '配对请求失败。'); } }
  }
  async approveCandidate(legacyDisconnected: boolean, authorized: boolean, background: boolean): Promise<void> {
    if (!this.pending || this.view.state !== 'candidate') { return; }
    if (!legacyDisconnected || !authorized || !background) {
      this.view.message = ui('Disconnect the old session and grant input/background access first.', '请先断开旧连接并允许输入控制和后台运行。'); return;
    }
    this.approved = true; await this.connectPeer(this.pending, true);
  }
  async connectTrusted(peer: V2TrustedPeer, host: string, port: number, legacyDisconnected: boolean, authorized: boolean, background: boolean): Promise<void> {
    const address = host.trim();
    if (!this.initialized || !address || port < 1 || port > 65535 || !legacyDisconnected || !authorized || !background) {
      this.view.message = ui('Enter an address and grant input/background access first.', '请先输入地址并允许输入控制和后台运行。'); return;
    }
    this.cancelRetry();
    this.host = address; this.port = port; this.pending = peer; this.approved = false; this.trustedConnection = true;
    await this.connectPeer(peer, false);
  }
  private async connectPeer(peer: V2TrustedPeer, pairing: boolean): Promise<void> {
    const generation = ++this.generation; this.view.state = 'connecting';
    this.displayLease = 0; this.displaySignature = ''; this.recoverableClose = false;
    this.view.peerName = peer.name; this.view.peerFingerprint = peer.fingerprint;
    this.view.message = ui('Establishing an encrypted connection…', '正在建立加密连接…');
    try {
      const identity = await this.manager.vault.loadIdentity();
      if (!identity) { throw new Error('Missing device identity'); }
      try {
        if (generation !== this.generation) { return; }
        this.view.localFingerprint = hopv2.fingerprint(identity.certificatePEM);
        const displays = await this.receiverDisplays();
        if (generation !== this.generation) { return; }
        this.displaySignature = this.signature(displays); this.displayLease = generation;
        this.view.displayCount = displays.length;
        this.refreshClipboardPermission();
        const clipboard = this.view.clipboardEnabled && this.view.clipboardAuthorized;
        const config: ReceiverConfig = { host: this.host, port: this.port, displays: displays,
          requestId: generation, certificatePEM: identity.certificatePEM, peerCertificatePEM: peer.certificatePEM,
          privateKeyPEM: identity.privateKeyPEM, pairing: pairing, clipboard: clipboard };
        hopv2.connect(config);
        this.clipboardLinkEnabled = clipboard;
        if (clipboard) { this.startClipboardTimer(); }
      } finally { identity.privateKeyPEM.fill(0); }
    } catch (_) { if (generation === this.generation) { this.displayLease = 0; this.view.state = 'error'; this.view.message = ui('Secure connection could not start.', '无法启动加密连接。'); } }
  }
  confirmExisting(): void {
    if (this.view.state !== 'pairing' || !this.pending) { return; }
    this.approved = true;
    try { hopv2.command('confirm'); this.view.state = 'waiting'; }
    catch (_) { this.view.state = 'error'; this.view.message = ui('Confirmation failed.', '确认失败。'); }
  }
  private handle(event: V2Event): void {
    if (event.requestId !== this.generation || event.requestId === 0) { return; }
    if (event.state === 'candidate') {
      try {
        if (!event.peerCertificatePEM || hopv2.fingerprint(event.peerCertificatePEM) !== event.peerFingerprint) { throw new Error('Invalid candidate'); }
        const peer = new V2TrustedPeer(); peer.name = event.peerName; peer.fingerprint = event.peerFingerprint; peer.certificatePEM = event.peerCertificatePEM;
        this.pending = peer; this.view.peerName = peer.name; this.view.peerFingerprint = peer.fingerprint;
        this.view.state = 'candidate'; this.view.message = ui('Compare BOTH complete fingerprints on both devices.', '请在两台设备分别核对双方的完整指纹。');
      } catch (_) { this.view.state = 'error'; this.view.message = ui('Invalid pairing candidate.', '配对设备信息无效。'); }
    } else if (event.state === 'pairing') {
      this.view.state = 'pairing'; this.view.message = ui('Confirm both fingerprints on both devices.', '请在两台设备分别核对并确认双方指纹。');
      if (this.approved) { this.confirmExisting(); }
    } else if (event.state === 'paired') {
      this.persistConfirmed(event.peerFingerprint, event.requestId);
    } else if (event.state === 'ready') {
      this.view.state = this.approved ? 'saving' : 'ready';
      this.view.message = this.approved ? ui('Saving secure pairing…', '正在安全保存配对…') : ui('Securely connected.', '加密连接已建立。');
      if (!this.approved && this.trustedConnection) { this.rememberEndpoint(); }
    } else if (event.state === 'remote') {
      this.view.state = 'remote'; this.view.message = ui('Receiving keyboard and mouse.', '正在接收键盘和鼠标。');
    } else if (event.state === 'error' || event.state === 'disconnected') {
      this.displayLease = 0;
      this.stopClipboardTimer();
      this.view.state = event.state; this.view.message = ui('Connection closed. Choose a device to reconnect.', '连接已断开，可以选择设备重新连接。');
      if (event.state === 'error') {
        hilog.warn(0, 'Hop', 'Secure receiver: %{public}s', event.message);
        if (event.message === 'Secure session disconnected' || event.message === 'Network polling failed') {
          this.recoverableClose = true; // wait for successful native input release
        } else {
          this.recoverableClose = false;
          if (event.message === 'Cannot connect to peer' || event.message === 'Secure authentication timed out') { this.scheduleRetry(); }
        }
      } else if (this.recoverableClose) {
        if (this.readySince > 0 && Date.now() - this.readySince > 20000) { this.retryCount = 0; }
        this.readySince = 0; this.recoverableClose = false; this.scheduleRetry();
      }
    }
  }
  private rememberEndpoint(): void {
    if (this.readySince === 0) { this.readySince = Date.now(); }
    this.lastPeer = this.pending; this.lastHost = this.host; this.lastPort = this.port;
  }
  private async persistConfirmed(fingerprint: string, generation: number): Promise<void> {
    if (this.saving || !this.approved || !this.pending || this.pending.fingerprint !== fingerprint) { this.disconnect(); return; }
    this.saving = true; let added = false;
    try {
      const existing = await this.manager.vault.loadPeer(fingerprint);
      if (!existing) { await this.manager.vault.saveConfirmedPeer(this.pending, fingerprint, true, true); added = true; }
      if (generation !== this.generation) {
        if (added) { await this.manager.vault.revokePeer(fingerprint); }
        return;
      }
      hopv2.command('saved'); this.approved = false; this.view.state = 'ready';
      this.trustedConnection = true; this.rememberEndpoint();
      this.view.message = ui('Device paired and securely connected.', '设备已配对并建立加密连接。');
      this.refreshTrusted();
    } catch (_) {
      if (generation === this.generation) { this.disconnect(); this.view.state = 'error'; this.view.message = ui('Could not save secure pairing.', '无法安全保存配对。'); }
    } finally { this.saving = false; }
  }
  async revoke(peer: V2TrustedPeer): Promise<void> {
    if (this.pending && this.pending.fingerprint === peer.fingerprint) { this.disconnect(); }
    if (this.canReconnect(peer)) { this.lastPeer = undefined; this.lastHost = ''; }
    try { await this.manager.vault.revokePeer(peer.fingerprint); await this.refreshTrusted(); }
    catch (_) { this.view.message = ui('Could not remove trusted device.', '无法解除该设备的配对。'); }
  }
  disconnect(): void {
    ++this.generation; this.approved = false; this.pending = undefined;
    this.cancelRetry();
    if (this.displayTimer >= 0) { clearTimeout(this.displayTimer); this.displayTimer = -1; }
    this.displayLease = 0;
    this.stopClipboardTimer();
    try { hopv2.command('disconnect'); } catch (_) {}
    this.view.state = 'disconnected'; this.view.message = ui('Secure session disconnected.', '加密连接已断开。');
  }
  shutdown(): void { this.disconnect(); this.stopWatchingDisplays(); this.discovery.stop(); try { hopv2.unlisten(); } catch (_) {} this.initialized = false; }
}
