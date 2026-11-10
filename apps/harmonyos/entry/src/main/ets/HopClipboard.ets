import { pasteboard } from '@kit.BasicServicesKit';
import { image } from '@kit.ImageKit';
import { util } from '@kit.ArkTS';

// Used by the v2 coordinator after opt-in and an authenticated focus.
export class ClipboardPacket {
  kind: string = '';
  text: string = '';
  png: ArrayBuffer = new ArrayBuffer(0);
}
const MAX_TEXT: number = 2 * 1024 * 1024;
const MAX_IMAGE: number = 16 * 1024 * 1024;
const MAX_PIXELS: number = 40000000;

function safeText(value: string): boolean {
  // Reject actual URI/file records in poll(); do not censor path-looking text.
  return value.indexOf('\u0000') < 0 && util.TextEncoder.create('utf-8').encodeInto(value).length <= MAX_TEXT;
}
function safePngHeader(buffer: ArrayBuffer): boolean {
  if (buffer.byteLength < 33 || buffer.byteLength > MAX_IMAGE) { return false; }
  const data = new Uint8Array(buffer);
  const signature: number[] = [137, 80, 78, 71, 13, 10, 26, 10];
  for (let i = 0; i < signature.length; i++) { if (data[i] !== signature[i]) { return false; } }
  if (data[8] !== 0 || data[9] !== 0 || data[10] !== 0 || data[11] !== 13 ||
    data[12] !== 73 || data[13] !== 72 || data[14] !== 68 || data[15] !== 82) { return false; }
  const view = new DataView(buffer);
  const width = view.getUint32(16), height = view.getUint32(20);
  return width > 0 && height > 0 && width <= 16384 && height <= 16384 && width * height <= MAX_PIXELS;
}

export class HopClipboard {
  private board: pasteboard.SystemPasteboard = pasteboard.getSystemPasteboard();
  private enabled: boolean = false;
  private verified: boolean = false;
  private failed: boolean = false;
  private peer: string = '';
  private epoch: string = '';
  private seen: number = 0;
  private busy: boolean = false;

  configure(enabled: boolean, verified: boolean, peer: string, epoch: string, baseline?: number): void {
    if (this.enabled !== enabled || this.verified !== verified || this.peer !== peer || this.epoch !== epoch) {
      this.seen = enabled && verified && peer.length > 0 && epoch.length > 0 ? (baseline ?? this.board.getChangeCount()) : 0;
      this.failed = false;
    }
    this.enabled = enabled; this.verified = verified; this.peer = peer; this.epoch = epoch;
  }
  private active(peer: string, epoch: string): boolean {
    return this.enabled && this.verified && !this.failed && peer.length > 0 && peer === this.peer && epoch.length > 0 && epoch === this.epoch;
  }
  healthy(): boolean { return !this.failed; }
  async poll(peer: string, epoch: string, permitted: () => boolean): Promise<ClipboardPacket | undefined> {
    if (!this.active(peer, epoch) || this.busy || !permitted()) { return undefined; }
    const sequence = this.board.getChangeCount();
    if (sequence === this.seen) { return undefined; }
    this.busy = true;
    try {
      const data = await this.board.getData();
      if (!this.active(peer, epoch) || !permitted() || this.board.getChangeCount() !== sequence) { return undefined; }
      // Do not coerce URIs, Want, or file records into text.
      if (data.getRecordCount() !== 1 || data.hasMimeType(pasteboard.MIMETYPE_TEXT_URI) || data.hasMimeType(pasteboard.MIMETYPE_TEXT_WANT)) {
        this.seen = sequence; return undefined;
      }
      const packet = new ClipboardPacket();
      if (data.hasMimeType(pasteboard.MIMETYPE_PIXELMAP)) {
        const pixels = data.getPrimaryPixelMap();
        const info = await pixels.getImageInfo();
        if (!this.active(peer, epoch) || !permitted() || this.board.getChangeCount() !== sequence) { return undefined; }
        if (info.size.width <= 0 || info.size.height <= 0 || info.size.width > 16384 || info.size.height > 16384 ||
          info.size.width * info.size.height > MAX_PIXELS) { this.seen = sequence; return undefined; }
        const packer = image.createImagePacker();
        try { packet.png = await packer.packing(pixels, { format: 'image/png', quality: 100 }); }
        finally { await packer.release(); }
        if (!safePngHeader(packet.png)) { this.seen = sequence; return undefined; }
        packet.kind = 'png';
      } else if (data.hasMimeType(pasteboard.MIMETYPE_TEXT_PLAIN)) {
        packet.text = data.getPrimaryText();
        if (!safeText(packet.text)) { this.seen = sequence; return undefined; }
        packet.kind = 'text';
      } else { this.seen = sequence; return undefined; }
      if (!this.active(peer, epoch) || !permitted() || this.board.getChangeCount() !== sequence) { return undefined; }
      this.seen = sequence;
      return packet;
    } catch (_) { if (this.active(peer, epoch)) { this.failed = true; } return undefined; }
    finally { this.busy = false; }
  }
  async apply(packet: ClipboardPacket, peer: string, epoch: string, expectedSequence: number, permitted: () => boolean): Promise<boolean> {
    if (!this.active(peer, epoch) || !permitted() || this.busy ||
      this.board.getChangeCount() !== expectedSequence) { return false; }
    this.busy = true;
    let decoded: image.PixelMap | undefined;
    try {
      let data: pasteboard.PasteData;
      if (packet.kind === 'text') {
        if (!safeText(packet.text)) { return false; }
        data = pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, packet.text);
      } else if (packet.kind === 'png') {
        if (!safePngHeader(packet.png)) { return false; }
        const source = image.createImageSource(packet.png);
        try {
          if (await source.getFrameCount() !== 1) { return false; }
          const info = await source.getImageInfo();
          if (info.size.width <= 0 || info.size.height <= 0 || info.size.width > 16384 || info.size.height > 16384 ||
            info.size.width * info.size.height > MAX_PIXELS) { return false; }
          decoded = await source.createPixelMap();
          data = pasteboard.createData(pasteboard.MIMETYPE_PIXELMAP, decoded);
        } finally { await source.release(); }
      } else { return false; }
      if (!this.active(peer, epoch) || !permitted() || this.board.getChangeCount() !== expectedSequence) { return false; }
      // The caller must execute this adapter away from input/paint critical paths.
      // Synchronous write avoids an async focus change between the final check and set.
      this.board.setDataSync(data);
      this.seen = this.board.getChangeCount();
      return true;
    } catch (_) { if (this.active(peer, epoch)) { this.failed = true; } return false; }
    finally { if (decoded) { try { await decoded.release(); } catch (_) {} } this.busy = false; }
  }
}
