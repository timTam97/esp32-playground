export const HEADER_SIZE = 32;
export const CHUNK_SIZE = 10_000;
export const MAX_FRAME_SIZE = 512 * 1024;
export const FRAME_EXPIRY_MS = 1000;

export interface JPEGFrame {
  id: number;
  capturedMs: number;
  receivedMs: number;
  bytes: Uint8Array<ArrayBuffer>;
}

interface Assembly {
  id: number;
  capturedMs: number;
  started: number;
  total: number;
  count: number;
  received: number;
  chunks: Uint8Array;
  bytes: Uint8Array<ArrayBuffer>;
}

// Signed subtraction handles the uint32 frame counter rolling over.
export const newer = (id: number, previous: number) => ((id - previous) | 0) > 0;

export class JPEGAssembler {
  private frames = new Map<number, Assembly>();
  private completed: number | undefined;
  dropped = 0;
  malformed = 0;
  duplicates = 0;

  get pendingFrames() { return this.frames.size; }
  get pendingBytes() { return [...this.frames.values()].reduce((total, frame) => total + frame.total, 0); }

  reset() {
    this.frames.clear();
    this.completed = undefined;
    this.dropped = this.malformed = this.duplicates = 0;
  }

  expire(now: number) {
    for (const [id, frame] of this.frames) {
      if (now - frame.started >= FRAME_EXPIRY_MS) {
        this.frames.delete(id);
        this.dropped++;
      }
    }
  }

  push(input: ArrayBuffer, now: number): JPEGFrame | null {
    this.expire(now);
    if (input.byteLength < HEADER_SIZE || input.byteLength > HEADER_SIZE + CHUNK_SIZE) return this.bad();
    const header = new DataView(input);
    if (header.getUint32(0) !== 0x474a5047 || header.getUint8(4) !== 1 ||
        header.getUint8(5) !== 0 || header.getUint16(6) !== HEADER_SIZE || header.getUint16(30) !== 0) return this.bad();
    const id = header.getUint32(8);
    const capturedMs = Number(header.getBigUint64(12));
    const total = header.getUint32(20);
    const index = header.getUint16(24);
    const count = header.getUint16(26);
    const size = header.getUint16(28);
    if (!Number.isSafeInteger(capturedMs) || total < 4 || total > MAX_FRAME_SIZE ||
        count !== Math.ceil(total / CHUNK_SIZE) || index >= count ||
        size !== Math.min(CHUNK_SIZE, total - index * CHUNK_SIZE) ||
        input.byteLength !== HEADER_SIZE + size) return this.bad();
    if (this.completed !== undefined && !newer(id, this.completed)) return null;
    let assembly = this.frames.get(id);
    if (!assembly) {
      // Two frames at most; prefer recent frames over an unbounded backlog.
      if (this.frames.size === 2) {
        const oldest = [...this.frames.values()].reduce((a, b) => newer(a.id, b.id) ? b : a);
        if (!newer(id, oldest.id)) { this.dropped++; return null; }
        this.frames.delete(oldest.id);
        this.dropped++;
      }
      assembly = {
        id, capturedMs, total, count, started: now, received: 0,
        chunks: new Uint8Array(count), bytes: new Uint8Array(total),
      };
      this.frames.set(id, assembly);
    } else if (assembly.total !== total || assembly.capturedMs !== capturedMs || assembly.count !== count) {
      this.frames.delete(id);
      this.dropped++;
      return this.bad();
    }
    const payload = new Uint8Array(input, HEADER_SIZE);
    const offset = index * CHUNK_SIZE;
    if (assembly.chunks[index]) {
      this.duplicates++;
      if (payload.some((byte, i) => byte !== assembly!.bytes[offset + i])) {
        this.frames.delete(id);
        this.dropped++;
        return this.bad();
      }
      return null;
    }
    assembly.bytes.set(payload, offset);
    assembly.chunks[index] = 1;
    assembly.received++;
    if (assembly.received !== count) return null;
    this.frames.delete(id);
    const bytes = assembly.bytes;
    if (bytes[0] !== 0xff || bytes[1] !== 0xd8 || bytes[total - 2] !== 0xff || bytes[total - 1] !== 0xd9) return this.bad();
    this.completed = id;
    for (const pending of this.frames.keys()) {
      if (!newer(pending, id)) { this.frames.delete(pending); this.dropped++; }
    }
    return { id, capturedMs, bytes, receivedMs: now };
  }

  private bad(): null { this.malformed++; return null; }
}
