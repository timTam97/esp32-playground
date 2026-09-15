import assert from 'node:assert/strict';
import test from 'node:test';
import { JPEGAssembler, MAX_FRAME_SIZE, CHUNK_SIZE, HEADER_SIZE } from '../src/jpeg.ts';

function packets(id = 1, size = 24_321): ArrayBuffer[] {
  const jpeg = new Uint8Array(size).fill(0x42);
  jpeg.set([0xff, 0xd8]); jpeg.set([0xff, 0xd9], size - 2);
  return Array.from({ length: Math.ceil(size / CHUNK_SIZE) }, (_, index) => {
    const payload = jpeg.slice(index * CHUNK_SIZE, (index + 1) * CHUNK_SIZE);
    const data = new ArrayBuffer(HEADER_SIZE + payload.length);
    const view = new DataView(data);
    view.setUint32(0, 0x474a5047); view.setUint8(4, 1); view.setUint16(6, HEADER_SIZE);
    view.setUint32(8, id); view.setBigUint64(12, 12345n); view.setUint32(20, size);
    view.setUint16(24, index); view.setUint16(26, Math.ceil(size / CHUNK_SIZE));
    view.setUint16(28, payload.length);
    new Uint8Array(data, HEADER_SIZE).set(payload);
    return data;
  });
}

test('reassembles reordered chunks and ignores identical duplicates', () => {
  const a = new JPEGAssembler(), p = packets();
  assert.equal(a.push(p[2], 0), null);
  assert.equal(a.push(p[2], 1), null);
  assert.equal(a.push(p[0], 2), null);
  const frame = a.push(p[1], 3)!;
  assert.equal(frame.bytes.length, 24_321);
  assert.equal(frame.id, 1);
  assert.equal(a.duplicates, 1);
  assert.equal(a.pendingBytes, 0);
});
test('expires incomplete frames and rejects stale completed frames', () => {
  const a = new JPEGAssembler(), p = packets();
  a.push(p[0], 0); a.expire(1000);
  assert.equal(a.pendingFrames, 0); assert.equal(a.dropped, 1);
  for (const part of packets(2)) a.push(part, 1100);
  assert.equal(a.push(p[0], 1101), null);
  assert.equal(a.pendingFrames, 0);
});
test('bounds allocation before accepting an oversized frame', () => {
  const a = new JPEGAssembler(), p = packets();
  new DataView(p[0]).setUint32(20, MAX_FRAME_SIZE + 1);
  assert.equal(a.push(p[0], 0), null);
  assert.equal(a.pendingBytes, 0); assert.equal(a.malformed, 1);
});
test('rejects truncated payload and conflicting duplicate chunks', () => {
  const a = new JPEGAssembler(), p = packets();
  a.push(p[0].slice(0, -1), 0);
  assert.equal(a.malformed, 1);
  a.push(p[0], 1);
  const conflict = p[0].slice(0);
  new Uint8Array(conflict)[HEADER_SIZE + 10] ^= 0xff;
  a.push(conflict, 2);
  assert.equal(a.pendingFrames, 0); assert.equal(a.malformed, 2);
});
test('keeps at most two frame assemblies, preferring fresh frames', () => {
  const a = new JPEGAssembler();
  for (let id = 1; id <= 100; id++) a.push(packets(id)[0], id);
  assert.equal(a.pendingFrames, 2);
  assert.equal(a.pendingBytes, 2 * 24_321);
});
test('accepts frame ID wraparound and rejects late frames before wrap', () => {
  const a = new JPEGAssembler();
  for (const part of packets(0xffffffff)) a.push(part, 0);
  let result;
  for (const part of packets(0)) result = a.push(part, 1);
  assert.equal(result!.id, 0);
  assert.equal(a.push(packets(0xffffffff)[0], 2), null);
});
test('does not produce an image with invalid JPEG boundary markers', () => {
  const a = new JPEGAssembler(), p = packets();
  new Uint8Array(p[0])[HEADER_SIZE] = 0;
  for (const part of p) assert.equal(a.push(part, 0), null);
  assert.equal(a.malformed, 1);
});
