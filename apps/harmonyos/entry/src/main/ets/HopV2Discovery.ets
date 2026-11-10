import { common } from '@kit.AbilityKit';
import { mdns } from '@kit.NetworkKit';

// Network names and addresses are only discovery hints, never peer identity.
export class V2NearbyPeer {
  name: string = '';
  host: string = '';
  port: number = 0;
}
export class HopV2Discovery {
  private browser?: mdns.DiscoveryService;
  private context?: common.Context;
  private generation: number = 0;
  private peers: V2NearbyPeer[] = [];
  onPeers: (items: V2NearbyPeer[]) => void = (_items: V2NearbyPeer[]) => {};
  onError: () => void = () => {};
  start(context: common.Context): void {
    if (this.browser) { return; }
    this.context = context; const generation = ++this.generation;
    try {
      this.browser = mdns.createDiscoveryService(context, '_hop2._tcp');
      this.browser.on('serviceFound', (item: mdns.LocalServiceInfo) => { this.resolve(item, generation); });
      this.browser.on('serviceLost', (item: mdns.LocalServiceInfo) => {
        if (generation !== this.generation) { return; }
        this.peers = this.peers.filter((peer: V2NearbyPeer) => peer.name !== item.serviceName);
        this.onPeers(this.peers);
      });
      this.browser.on('discoveryStart', (event: mdns.DiscoveryEventInfo) => {
        if (generation === this.generation && event.errorCode !== undefined) { this.onError(); }
      });
      this.browser.startSearchingMDNS();
    } catch (_) { this.browser = undefined; this.onError(); }
  }
  private async resolve(item: mdns.LocalServiceInfo, generation: number): Promise<void> {
    if (!this.context) { return; }
    try {
      const info = await mdns.resolveLocalService(this.context, item);
      if (generation !== this.generation) { return; }
      const attrs = info.serviceAttribute || [];
      const version = attrs.find((attribute: mdns.ServiceAttribute) => attribute.key === 'v');
      if (!info.host || !info.port || !version || String.fromCharCode(...version.value) !== '2' ||
        !info.serviceName || /[\u0000-\u001f\u007f-\u009f\u061c\u200e\u200f\u2028-\u202e\u2066-\u2069]/.test(info.serviceName)) { return; }
      const peer = new V2NearbyPeer(); peer.name = info.serviceName; peer.host = info.host.address; peer.port = info.port;
      this.peers = this.peers.filter((p: V2NearbyPeer) => !(p.name === peer.name && p.host === peer.host && p.port === peer.port));
      this.peers = this.peers.concat([peer]).slice(0, 64); this.onPeers(this.peers);
    } catch (_) {
      // One disappearing or unresolvable service does not disable discovery.
    }
  }
  stop(): void {
    ++this.generation;
    try { if (this.browser) { this.browser.stopSearchingMDNS(); } } catch (_) {}
    this.browser = undefined; this.peers = []; this.onPeers(this.peers);
  }
}
