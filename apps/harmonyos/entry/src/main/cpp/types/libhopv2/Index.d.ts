export interface GeneratedIdentity { certificatePEM: string; fingerprint: string; privateKeyPEM: Uint8Array; }
export interface V2Event { state: string; message: string; peerFingerprint: string; peerCertificatePEM: string; peerName: string; requestId: number; }
export interface OfferConfig { host: string; port: number; name: string; certificatePEM: string; requestId: number; }
export interface ReceiverDisplay { displayId: number; x: number; y: number; width: number; height: number; }
export interface ReceiverConfig {
  host: string; port: number; displays: ReceiverDisplay[]; requestId: number;
  certificatePEM: string; peerCertificatePEM: string; privateKeyPEM: Uint8Array;
  pairing: boolean;
  clipboard?: boolean;
}
export interface ClipboardStatus { epoch: string; baseline: number; formats: number; active: boolean; poll: boolean; draining: boolean; }
export interface ClipboardTransfer { format: number; baseline: number; bytes: Uint8Array; }
declare const hopv2: {
  generateIdentity(): GeneratedIdentity;
  fingerprint(certificatePEM: string): string;
  identityMatches(certificatePEM: string, privateKeyPEM: Uint8Array): boolean;
  listen(callback: (event: V2Event) => void): void;
  unlisten(): void;
  offer(config: OfferConfig): void;
  // Consumes and clears privateKeyPEM; peer certificate must be independently verified.
  connect(config: ReceiverConfig): void;
  clipboardStatus(requestId: number): ClipboardStatus | undefined;
  clipboardAction(requestId: number, epoch: string, action: 'activate' | 'permitted' | 'finish' | 'drained' | 'pause'): boolean;
  takeClipboard(requestId: number, epoch: string): ClipboardTransfer | undefined;
  submitClipboard(requestId: number, epoch: string, format: number, bytes: Uint8Array): boolean;
  command(command: 'confirm' | 'saved' | 'cancel' | 'disconnect'): void;
};
export default hopv2;
