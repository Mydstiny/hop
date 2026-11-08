export interface HopConfig {
  host: string; port: number; width: number; height: number;
  uuid: string; peerId: string; token: string; name: string;
}
export interface HopEvent {
  kind: string; state: string; message: string; peerId: string; token: string; code: number;
  host: string; port: number; session: string;
}
export const listen: (callback: (event: HopEvent) => void) => void;
export const unlisten: () => void;
export const requestAuthorization: () => number;
export const authorization: () => number;
export const connect: (config: HopConfig) => void;
export const command: (name: string) => void;
export const randomHex: (bytes: number) => string;
export const snapshot: () => HopEvent;
