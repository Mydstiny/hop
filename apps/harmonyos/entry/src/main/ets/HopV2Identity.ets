import hopv2, { GeneratedIdentity } from 'libhopv2.so';
import { HopV2Vault, V2IdentityValidator, V2LocalIdentity } from './HopV2Vault';

class NativeValidator implements V2IdentityValidator {
  fingerprint(certificatePEM: string): string { return hopv2.fingerprint(certificatePEM); }
  identityMatches(certificatePEM: string, privateKeyPEM: Uint8Array): boolean {
    return hopv2.identityMatches(certificatePEM, privateKeyPEM);
  }
}
// Explicit v2 startup only. Caller owns and must clear privateKeyPEM after TLS
// imports it; no identity is generated on v1 startup or on a failed load.
export class HopV2IdentityManager {
  readonly vault: HopV2Vault = new HopV2Vault(new NativeValidator());
  async loadOrCreate(): Promise<V2LocalIdentity> {
    const existing = await this.vault.loadIdentity();
    if (existing) { return existing; }
    const generated: GeneratedIdentity = hopv2.generateIdentity();
    const identity = new V2LocalIdentity(); identity.certificatePEM = generated.certificatePEM;
    identity.privateKeyPEM = generated.privateKeyPEM;
    try {
      if (hopv2.fingerprint(identity.certificatePEM) !== generated.fingerprint ||
        !hopv2.identityMatches(identity.certificatePEM, identity.privateKeyPEM)) { throw new Error('Generated identity invalid'); }
      await this.vault.saveNewIdentity(identity);
      return identity;
    } catch (_) {
      identity.privateKeyPEM.fill(0); throw new Error('Cannot securely save device identity');
    }
  }
}
