import { asset } from '@kit.AssetStoreKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { util } from '@kit.ArkTS';

// v2-only: ASSET encrypts private key at rest and integrity-protects the
// associated public certificate. Never fall back to plaintext Preferences.
export interface V2IdentityValidator {
  fingerprint(certificatePEM: string): string;
  identityMatches(certificatePEM: string, privateKeyPEM: Uint8Array): boolean;
}
export class V2LocalIdentity {
  certificatePEM: string = '';
  privateKeyPEM: Uint8Array = new Uint8Array(0);
}
export class V2TrustedPeer {
  certificatePEM: string = '';
  fingerprint: string = '';
  name: string = '';
}
const LOCAL_ALIAS: string = 'hop-v2-local-identity';
const PEER_PREFIX: string = 'hop-v2-peer-';
const PEER_MARKER: string = 'hop-v2-peer-record';
function utf8(value: string): Uint8Array { return util.TextEncoder.create('utf-8').encodeInto(value); }
function pemBytes(value: string, limit: number): Uint8Array {
  const bytes = utf8(value);
  if (!bytes || bytes.length < 1 || bytes.length > limit) { throw new Error('Certificate size invalid'); }
  for (let b of bytes) { if (b !== 10 && b !== 13 && (b < 32 || b > 126)) { throw new Error('Certificate encoding invalid'); } }
  return bytes;
}
function pemString(bytes: Uint8Array, limit: number): string {
  if (!bytes || bytes.length < 1 || bytes.length > limit) { throw new Error('Certificate size invalid'); }
  let result = '';
  for (let b of bytes) {
    if (b !== 10 && b !== 13 && (b < 32 || b > 126)) { throw new Error('Certificate encoding invalid'); }
    result += String.fromCharCode(b);
  }
  return result;
}
function validFingerprint(value: string): boolean { return /^[A-F0-9]{64}$/.test(value); }
function queryFor(alias: string, all: boolean): asset.AssetMap {
  const query: asset.AssetMap = new Map();
  query.set(asset.Tag.ALIAS, utf8(alias));
  query.set(asset.Tag.RETURN_TYPE, all ? asset.ReturnType.ALL : asset.ReturnType.ATTRIBUTES);
  return query;
}
async function queryOne(alias: string, all: boolean): Promise<asset.AssetMap | undefined> {
  try {
    const items = await asset.query(queryFor(alias, all));
    if (items.length !== 1) { throw new Error('Unexpected asset count'); }
    return items[0];
  } catch (error) {
    if ((error as BusinessError).code === 24000002) { return undefined; }
    throw new Error('Secure asset query failed');
  }
}
export class HopV2Vault {
  private validator: V2IdentityValidator;
  constructor(validator: V2IdentityValidator) { this.validator = validator; }
  async loadIdentity(): Promise<V2LocalIdentity | undefined> {
    const item = await queryOne(LOCAL_ALIAS, true);
    if (!item) { return undefined; }
    const key = item.get(asset.Tag.SECRET) as Uint8Array;
    const cert = item.get(asset.Tag.DATA_LABEL_CRITICAL_1) as Uint8Array;
    if (!key || key.length < 1 || key.length > 1024 || !cert) {
      if (key) { key.fill(0); } throw new Error('Stored device identity invalid');
    }
    try {
      const pem = pemString(cert, 2048);
      if (!this.validator.identityMatches(pem, key) || !validFingerprint(this.validator.fingerprint(pem))) {
        throw new Error('Stored device identity does not match');
      }
      const result = new V2LocalIdentity(); result.certificatePEM = pem; result.privateKeyPEM = key;
      return result; // TLS importer must clear the returned key after use
    } catch (_) {
      key.fill(0); throw new Error('Stored device identity invalid');
    }
  }
  async saveNewIdentity(identity: V2LocalIdentity): Promise<void> {
    if (await queryOne(LOCAL_ALIAS, false)) { throw new Error('A device identity already exists'); }
    const key = identity.privateKeyPEM, cert = pemBytes(identity.certificatePEM, 2048);
    if (!key || key.length < 1 || key.length > 1024 || !this.validator.identityMatches(identity.certificatePEM, key) ||
      !validFingerprint(this.validator.fingerprint(identity.certificatePEM))) { throw new Error('Device identity invalid'); }
    const attributes: asset.AssetMap = new Map();
    attributes.set(asset.Tag.ALIAS, utf8(LOCAL_ALIAS)); attributes.set(asset.Tag.SECRET, key);
    attributes.set(asset.Tag.DATA_LABEL_CRITICAL_1, cert);
    attributes.set(asset.Tag.ACCESSIBILITY, asset.Accessibility.DEVICE_FIRST_UNLOCKED);
    await asset.add(attributes); // duplicate/locked/unavailable fails; never overwrite
  }
  async loadPeer(fingerprint: string): Promise<V2TrustedPeer | undefined> {
    if (!validFingerprint(fingerprint)) { throw new Error('Fingerprint invalid'); }
    const item = await queryOne(PEER_PREFIX + fingerprint, true);
    if (!item) { return undefined; }
    const cert = item.get(asset.Tag.SECRET) as Uint8Array;
    const label = item.get(asset.Tag.DATA_LABEL_CRITICAL_1) as Uint8Array;
    const pem = pemString(cert, 1024);
    if (this.validator.fingerprint(pem) !== fingerprint || !label || label.length < 1 || label.length > 128) {
      throw new Error('Stored peer certificate invalid');
    }
    const peer = new V2TrustedPeer(); peer.certificatePEM = pem; peer.fingerprint = fingerprint;
    peer.name = util.TextDecoder.create('utf-8', { fatal: true }).decodeToString(label);
    return peer;
  }
  // Caller must have compared both fingerprints and received peer confirmation
  // over the exact pinned TLS connection.
  async saveConfirmedPeer(peer: V2TrustedPeer, authenticatedFingerprint: string, localConfirmed: boolean, peerConfirmed: boolean): Promise<void> {
    if (!localConfirmed || !peerConfirmed || !validFingerprint(peer.fingerprint) || authenticatedFingerprint !== peer.fingerprint ||
      this.validator.fingerprint(peer.certificatePEM) !== peer.fingerprint) { throw new Error('Peer is not confirmed'); }
    const name = utf8(peer.name), cert = pemBytes(peer.certificatePEM, 1024);
    if (!name || name.length < 1 || name.length > 128) { throw new Error('Peer name invalid'); }
    if (await queryOne(PEER_PREFIX + peer.fingerprint, false)) { throw new Error('Peer already trusted'); }
    const attributes: asset.AssetMap = new Map();
    attributes.set(asset.Tag.ALIAS, utf8(PEER_PREFIX + peer.fingerprint)); attributes.set(asset.Tag.SECRET, cert);
    attributes.set(asset.Tag.DATA_LABEL_CRITICAL_1, name);
    attributes.set(asset.Tag.DATA_LABEL_NORMAL_1, utf8(PEER_MARKER));
    attributes.set(asset.Tag.ACCESSIBILITY, asset.Accessibility.DEVICE_FIRST_UNLOCKED);
    await asset.add(attributes);
  }
  async listPeers(): Promise<V2TrustedPeer[]> {
    const query: asset.AssetMap = new Map();
    query.set(asset.Tag.DATA_LABEL_NORMAL_1, utf8(PEER_MARKER));
    query.set(asset.Tag.RETURN_TYPE, asset.ReturnType.ATTRIBUTES); query.set(asset.Tag.RETURN_LIMIT, 64);
    try {
      const items = await asset.query(query); const peers: V2TrustedPeer[] = [];
      for (let item of items) {
        const raw = item.get(asset.Tag.ALIAS) as Uint8Array;
        if (!raw || raw.length !== PEER_PREFIX.length + 64) { continue; }
        const alias = pemString(raw, 256);
        if (!alias.startsWith(PEER_PREFIX)) { continue; }
        const fingerprint = alias.substring(PEER_PREFIX.length);
        if (!validFingerprint(fingerprint)) { continue; }
        const peer = await this.loadPeer(fingerprint);
        if (peer) { peers.push(peer); }
      }
      return peers;
    } catch (error) {
      if ((error as BusinessError).code === 24000002) { return []; }
      throw new Error('Secure peer list unavailable');
    }
  }
  async revokePeer(fingerprint: string): Promise<void> {
    if (!validFingerprint(fingerprint)) { throw new Error('Fingerprint invalid'); }
    const query: asset.AssetMap = new Map(); query.set(asset.Tag.ALIAS, utf8(PEER_PREFIX + fingerprint));
    try { await asset.remove(query); } catch (error) {
      if ((error as BusinessError).code !== 24000002) { throw new Error('Secure peer removal failed'); }
    }
  }
}
