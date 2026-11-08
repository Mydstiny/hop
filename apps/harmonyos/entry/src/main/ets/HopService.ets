import { common } from '@kit.AbilityKit';
import { preferences } from '@kit.ArkData';
import { display } from '@kit.ArkUI';
import { mdns } from '@kit.NetworkKit';
import hop, { HopEvent, HopConfig } from 'libhop.so';

export class MacPeer {
  name: string = '';
  host: string = '';
  port: number = 24890;
  id: string = '';
}
class Binding {
  peerId: string = '';
  token: string = '';
  host: string = '';
  port: number = 24890;
}
@Observed
export class HopViewState {
  state: string = 'disconnected';
  message: string = 'Choose a Mac on your trusted local network.';
  code: number = 0;
  authorization: number = 0;
  paired: boolean = false;
  background: boolean = false;
  peers: MacPeer[] = [];
  discovery: string = '';
}
// The UIAbility owns persistence and reconnect for the entire app lifetime.
// The statusBarView runs with the main process, never in an isolated process.
export class HopService {
  static readonly instance: HopService = new HopService();
  readonly view: HopViewState = new HopViewState();
  private prefs?: preferences.Preferences;
  private context?: common.Context;
  private owner: boolean = false;
  private timer: number = -1;
  private retryAt: number = 0;
  private attempts: number = 0;
  private browsing?: mdns.DiscoveryService;
  private saving: boolean = false;
  private paused: boolean = false;
  private unpairPending: boolean = false;
  private width: number = 0;
  private height: number = 0;

  private guard(action: () => void): void {
    try { action(); } catch (error) {
      this.view.message = `Hop operation failed: ${String(error)}`;
    }
  }
  private command(name: string): void {
    this.guard(() => { hop.command(name); });
  }
  private authorization(): number {
    try { return hop.authorization(); } catch (error) {
      this.view.message = `Cannot query system authorization: ${String(error)}`;
      return 0;
    }
  }
  initialize(context: common.Context, owner: boolean): void {
    this.owner = this.owner || owner;
    if (!this.prefs) {
      this.context = context;
      this.prefs = preferences.getPreferencesSync(context, { name: 'hop' });
      if (!this.prefs.getSync('uuid', '')) {
        this.prefs.putSync('uuid', hop.randomHex(16));
        this.prefs.flushSync();
      }
      hop.listen((event: HopEvent) => { this.guard(() => { this.onEvent(event); }); });
      this.view.paired = this.binding().token.length > 0;
    }
    if (this.owner && this.timer < 0) {
      this.timer = setInterval(() => { this.guard(() => { this.tick(); }); }, 1000);
    }
    this.view.authorization = this.authorization();
  }
  private binding(): Binding {
    const raw = this.prefs?.getSync('binding', '') as string;
    if (!raw) { return new Binding(); }
    try { return JSON.parse(raw) as Binding; } catch (_) { return new Binding(); }
  }
  private onEvent(event: HopEvent): void {
    if (event.kind === 'persist') {
      if (this.owner) { this.persist(event); }
      return;
    }
    if (event.kind === 'unpair') {
      this.unpairPending = false;
      if (this.owner) { this.clearBinding().catch((error: Error) => { this.view.message = `Cannot remove pairing: ${String(error)}`; }); }
      this.view.paired = false;
      this.view.message = event.message;
      return;
    }
    this.view.state = event.state || this.view.state;
    this.view.code = event.code;
    if (event.message) { this.view.message = event.message; }
    if (event.state === 'local' || event.state === 'remote') { this.attempts = 0; }
    if (event.state === 'disconnected') {
      // Also handles a worker exiting between snapshot() and queued unpair.
      // Native emits disconnected only after releasing its pressed set.
      if (this.unpairPending) {
        this.unpairPending = false;
        this.clearBinding().catch((error: Error) => { this.view.message = `Cannot remove pairing: ${String(error)}`; });
      }
      this.retryAt = Date.now() + Math.min(30000, 1000 * Math.pow(2, this.attempts++));
    }
  }
  private async persist(event: HopEvent): Promise<void> {
    if (this.paused || this.saving || !this.prefs) { return; }
    this.saving = true;
    try {
      const b: Binding = { peerId: event.peerId, token: event.token, host: event.host, port: event.port };
      this.prefs.putSync('binding', JSON.stringify(b));
      this.prefs.putSync('autoReconnect', true);
      await this.prefs.flush();
      this.view.paired = true;
      if (!this.paused) { this.command(`saved:${event.session}`); } // never acknowledge COMMIT before durable flush
    } catch (_) {
      this.view.message = 'Pairing could not be saved. Reconnect and pair again.';
      this.command(`saveFailed:${event.session}`);
    } finally { this.saving = false; }
  }
  private async clearBinding(): Promise<void> {
    if (!this.prefs) { return; }
    this.prefs.putSync('binding', '');
    this.prefs.putSync('autoReconnect', false);
    await this.prefs.flush();
    this.view.paired = false;
  }
  private tick(): void {
    this.view.authorization = this.authorization();
    const state: string = hop.snapshot().state;
    const d = display.getDefaultDisplaySync();
    if (this.width && (d.width !== this.width || d.height !== this.height) && state !== 'disconnected') {
      this.command('disconnect'); // native worker releases before socket closes
    }
    if (this.paused || !this.view.background || this.view.authorization !== 2 || state !== 'disconnected' || Date.now() < this.retryAt) { return; }
    if (this.prefs?.getSync('autoReconnect', false) && this.binding().token) { this.connectBinding(); }
  }
  authorize(): void {
    this.guard(() => {
      // Explicit button only. Reconnect never spawns permission prompts.
      const result: number = hop.requestAuthorization();
      this.view.authorization = this.authorization();
      this.view.message = result === 0 || result === 3900005 || result === 3900007 ?
        'Approve the system input injection dialog, then choose Connect.' : `System authorization request failed (${result}).`;
    });
  }
  private start(host: string, port: number, b: Binding): void {
    if (!this.prefs || this.view.authorization !== 2) {
      this.view.message = 'Grant system injection permission first.'; return;
    }
    if (!this.view.background) { this.view.message = 'Background task/notification unavailable. Relaunch Hop to retry.'; return; }
    const d = display.getDefaultDisplaySync();
    this.width = d.width; this.height = d.height;
    const config: HopConfig = {
      host: host, port: port, width: d.width, height: d.height,
      uuid: this.prefs.getSync('uuid', '') as string, peerId: b.peerId, token: b.token, name: 'HarmonyOS PC'
    };
    this.paused = false;
    this.view.state = 'connecting';
    try { hop.connect(config); } catch (error) {
      this.view.state = 'disconnected';
      this.retryAt = Date.now() + 30000;
      this.view.message = `Cannot start native receiver: ${String(error)}`;
    }
  }
  connectPeer(peer: MacPeer): void {
    this.guard(() => {
      if (this.binding().token) { this.view.message = 'Unpair the current Mac before pairing another.'; return; }
      this.start(peer.host, peer.port, new Binding());
    });
  }
  connectManual(host: string): void {
    this.guard(() => {
      // Native AI_NUMERICHOST accepts IPv4/IPv6, never blocking DNS.
      const address = host.trim();
      if (!address || address.length > 255) { this.view.message = 'Enter a Mac LAN IP address.'; return; }
      const b = this.binding();
      this.start(address, 24890, b);
    });
  }
  connectBinding(): void {
    this.guard(() => {
      const b = this.binding();
      if (!b.token) { return; }
      this.prefs?.putSync('autoReconnect', true);
      this.prefs?.flushSync();
      const discovered = this.view.peers.find((peer: MacPeer) => peer.id === b.peerId);
      this.start(discovered?.host || b.host, discovered?.port || b.port, b);
    });
  }
  confirm(): void { this.command('confirm'); }
  disconnect(): void {
    this.paused = true;
    // Always release native input even if Preferences is unavailable/full.
    this.command('disconnect');
    this.guard(() => {
      this.prefs?.putSync('autoReconnect', false);
      this.prefs?.flushSync();
    });
  }
  async unpair(): Promise<void> {
    this.paused = true;
    this.unpairPending = true;
    try {
      const state: string = hop.snapshot().state;
      this.command('unpair');
      // Active session: only its post-release event may clear Preferences.
      // Offline/finished worker: no injected state exists, clear immediately.
      if (state === 'disconnected') {
        this.unpairPending = false;
        await this.clearBinding();
      }
      this.view.code = 0;
    } catch (error) { this.view.message = `Cannot remove pairing: ${String(error)}`; }
  }
  browse(): void {
    if (!this.context || this.browsing) { return; }
    try {
      this.browsing = mdns.createDiscoveryService(this.context, '_hop._tcp');
      this.browsing.on('serviceFound', (service: mdns.LocalServiceInfo) => { this.resolve(service); });
      this.browsing.on('serviceLost', (service: mdns.LocalServiceInfo) => {
        this.view.peers = this.view.peers.filter((p: MacPeer) => p.name !== service.serviceName);
      });
      this.browsing.on('discoveryStart', (event: mdns.DiscoveryEventInfo) => {
        this.view.discovery = event.errorCode !== undefined ? `Discovery failed (${event.errorCode}). Use manual IP.` : 'Searching local network…';
      });
      this.browsing.startSearchingMDNS();
    } catch (_) { this.view.discovery = 'mDNS unavailable. Use manual IP.'; }
  }
  private async resolve(service: mdns.LocalServiceInfo): Promise<void> {
    if (!this.context) { return; }
    try {
      const info = await mdns.resolveLocalService(this.context, service);
      const attrs = info.serviceAttribute || [];
      const version = attrs.find((a: mdns.ServiceAttribute) => a.key === 'v');
      const identity = attrs.find((a: mdns.ServiceAttribute) => a.key === 'id');
      if (!info.host || !info.port || !version || String.fromCharCode(...version.value) !== '1' || !identity) { return; }
      const id = String.fromCharCode(...identity.value).replace(/-/g, '').toLowerCase();
      if (!/^[a-f0-9]{32}$/.test(id)) { return; }
      const peer: MacPeer = { name: info.serviceName, host: info.host.address, port: info.port, id: id };
      this.view.peers = this.view.peers.filter((p: MacPeer) => p.id !== id).concat([peer]).slice(0, 32);
      this.view.discovery = 'Select a Mac; discovery alone never starts a new pairing.';
    } catch (_) { this.view.discovery = 'A Mac could not be resolved. Use manual IP.'; }
  }
  stop(): void {
    this.disconnect();
    if (this.timer >= 0) { clearInterval(this.timer); this.timer = -1; }
    this.guard(() => { if (this.browsing) { this.browsing.stopSearchingMDNS(); } });
    this.browsing = undefined;
    this.guard(() => { hop.unlisten(); });
  }
}
