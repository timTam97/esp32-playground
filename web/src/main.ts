import './style.css';
import { JPEGAssembler, type JPEGFrame } from './jpeg.ts';

const element = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;
const canvas = element<HTMLCanvasElement>('camera');
const context = canvas.getContext('2d')!;
const startButton = element<HTMLButtonElement>('start');
const stopButton = element<HTMLButtonElement>('stop');
const assembler = new JPEGAssembler();
let pc: RTCPeerConnection | undefined;
let control: RTCDataChannel | undefined;
let session = '';
let pollTimer: ReturnType<typeof setInterval> | undefined;
let pingTimer: ReturnType<typeof setInterval> | undefined;
let timeout: ReturnType<typeof setTimeout> | undefined;
let cursor = 0, generation = 0, startTime = 0, stopTime = 0, deviceOffset = 0, bestRTT = Infinity;
let pending: JPEGFrame | undefined, decoding = false;
let pendingGeneration = 0;
let device: Record<string, unknown> = {};
let decoded = 0, decodeErrors = 0, skippedDecode = 0, receivedBytes = 0;
let route = 'unknown', polling = false;
let sampleTimes: number[] = [], latencies: number[] = [], events: string[] = [];
let jpegReady = false, streamingStarted = false;

interface Metrics {
  session: string; decoded: number; decodeErrors: number; skippedDecode: number;
  malformed: number; incomplete: number; duplicates: number; elapsedSeconds: number;
  averageFPS: number; recentFPS: number; averageMbps: number;
  p95LatencyMs: number | null; route: string; device: Record<string, unknown>;
}
declare global { interface Window { garageMetrics: Metrics; } }

function log(text: string) {
  events.push(`${new Date().toISOString().slice(11, 23)} ${text}`);
  events = events.slice(-50);
  element('events').textContent = events.join('\n');
}
function state(text: string) { element('state').textContent = text; log(text); }
function command(value: Record<string, unknown>) {
  if (control?.readyState === 'open') control.send(JSON.stringify(value));
}
async function signal(value: Record<string, unknown>) {
  const response = await fetch('/lab/signal', {
    method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ ...value, session }),
  });
  if (!response.ok) throw new Error(`Signaling failed (${response.status})`);
}
function settings() {
  const fps = element<HTMLInputElement>('fps').valueAsNumber;
  const quality = element<HTMLInputElement>('quality').valueAsNumber;
  if (!Number.isInteger(fps) || fps < 2 || fps > 5 || !Number.isInteger(quality) || quality < 10 || quality > 40) {
    element('error').textContent = 'Use 2–5 fps and JPEG compression 10–40.';
    return false;
  }
  command({ type: 'settings', fps, quality });
  return true;
}
function beginStreaming() {
  if (streamingStarted || !jpegReady || control?.readyState !== 'open') return;
  if (!settings()) return;
  streamingStarted = true;
  command({ type: 'ping', sent: performance.now() });
  command({ type: 'start' });
  state('Streaming');
  if (timeout) clearTimeout(timeout);
  pingTimer = setInterval(() => command({ type: 'ping', sent: performance.now() }), 2000);
}
async function decode(frame: JPEGFrame, currentGeneration: number) {
  if (decoding) {
    if (pending) skippedDecode++;
    pending = frame;
    pendingGeneration = currentGeneration;
    return;
  }
  decoding = true;
  try {
    // The JPEG remains compressed until it reaches the browser.
    const blob = new Blob([frame.bytes], { type: 'image/jpeg' });
    const bitmap = await createImageBitmap(blob);
    try {
      if (currentGeneration !== generation) return;
      if (bitmap.width !== 1600 || bitmap.height !== 1200) throw new Error('Unexpected JPEG dimensions');
      context.drawImage(bitmap, 0, 0);
      const now = performance.now();
      if (!startTime) startTime = now;
      decoded++;
      sampleTimes.push(now);
      sampleTimes = sampleTimes.filter(time => now - time < 10_000);
      if (Number.isFinite(bestRTT)) {
        latencies.push(Math.max(0, now + deviceOffset - frame.capturedMs));
        latencies = latencies.slice(-600);
      }
    } finally { bitmap.close(); }
  } catch (error) {
    if (currentGeneration === generation) {
      decodeErrors++;
      log(`Decode error: ${String(error)}`);
    }
  } finally {
    decoding = false;
    const next = pending;
    const nextGeneration = pendingGeneration;
    pending = undefined;
    if (next && nextGeneration === generation) void decode(next, nextGeneration);
  }
}
function report() {
  const now = performance.now();
  assembler.expire(now);
  const seconds = startTime ? ((stopTime || now) - startTime) / 1000 : 0;
  const sorted = [...latencies].sort((a, b) => a - b);
  const times = sampleTimes.filter(time => now - time < 10_000);
  window.garageMetrics = {
    session, decoded, decodeErrors, skippedDecode, malformed: assembler.malformed,
    incomplete: assembler.dropped, duplicates: assembler.duplicates, elapsedSeconds: seconds,
    averageFPS: seconds > 0 ? Math.max(0, decoded - 1) / seconds : 0,
    recentFPS: times.length > 1 ? (times.length - 1) * 1000 / (times.at(-1)! - times[0]) : 0,
    averageMbps: seconds > 0 ? receivedBytes * 8 / seconds / 1e6 : 0,
    p95LatencyMs: sorted.length ? sorted[Math.min(sorted.length - 1, Math.floor(sorted.length * .95))] : null,
    route, device,
  };
  element('metrics').textContent = JSON.stringify(window.garageMetrics, null, 2);
  element('summary').textContent = `1600 × 1200 · ${window.garageMetrics.recentFPS.toFixed(2)} decoded fps · ${route} · ${decoded} valid frames`;
}
async function poll(currentGeneration: number) {
  if (polling || currentGeneration !== generation) return;
  polling = true;
  try {
    const response = await fetch(`/lab/events?after=${cursor}&session=${encodeURIComponent(session)}`);
    if (!response.ok) throw new Error(`Event request failed (${response.status})`);
    const body = await response.json();
    if (currentGeneration !== generation) return;
    cursor = body.cursor;
    for (const event of body.events) {
      if (event.session && event.session !== session) continue;
      if (event.type === 'answer' && pc && !pc.remoteDescription) {
        await pc.setRemoteDescription({ type: 'answer', sdp: event.sdp });
      } else if (event.type === 'candidate' && pc?.remoteDescription) {
        await pc.addIceCandidate({ candidate: event.candidate.replace(/^a=/, ''), sdpMid: '0' });
      } else if (event.type === 'stats') device = event;
      else if (event.type === 'error') element('error').textContent = event.message;
      else if (event.type !== 'candidate') log(JSON.stringify(event));
    }
    if (pc?.connectionState === 'connected') {
      const stats = await pc.getStats();
      stats.forEach(item => {
        if (item.type === 'candidate-pair' && item.state === 'succeeded' && item.nominated) {
          const local = stats.get(item.localCandidateId), remote = stats.get(item.remoteCandidateId);
          route = local?.candidateType === 'relay' || remote?.candidateType === 'relay' ? 'relay' : 'direct';
        }
      });
    }
  } catch (error) {
    if (currentGeneration === generation) log(String(error));
  } finally { polling = false; }
}
async function start() {
  stop();
  const currentGeneration = generation;
  session = crypto.randomUUID();
  decoded = decodeErrors = skippedDecode = receivedBytes = 0;
  startTime = stopTime = 0; bestRTT = Infinity; route = 'unknown';
  sampleTimes = []; latencies = []; device = {};
  assembler.reset();
  element('error').textContent = '';
  startButton.disabled = true; stopButton.disabled = false;
  state('Connecting');
  try {
    const initial = await fetch('/lab/events?after=0').then(response => response.json());
    if (currentGeneration !== generation) return;
    cursor = initial.cursor;
    pc = new RTCPeerConnection({ iceServers: [] });
    const connection = pc;
    control = connection.createDataChannel('control', { ordered: true });
    const jpeg = connection.createDataChannel('jpeg', { ordered: false, maxRetransmits: 1 });
    jpeg.binaryType = 'arraybuffer';
    control.onopen = () => { if (currentGeneration === generation) beginStreaming(); };
    control.onmessage = event => {
      let value;
      try { value = JSON.parse(event.data); } catch { return; }
      if (currentGeneration !== generation) return;
      if (value.type === 'stats') device = value;
      else if (value.type === 'pong') {
        const rtt = performance.now() - value.sent;
        if (rtt >= 0 && rtt < bestRTT) {
          bestRTT = rtt;
          deviceOffset = value.device_ms - (value.sent + rtt / 2);
        }
      } else if (value.type === 'error') element('error').textContent = value.message;
    };
    jpeg.onopen = () => { if (currentGeneration === generation) { jpegReady = true; beginStreaming(); } };
    jpeg.onmessage = event => {
      if (currentGeneration !== generation || !(event.data instanceof ArrayBuffer)) return;
      receivedBytes += event.data.byteLength;
      const frame = assembler.push(event.data, performance.now());
      if (frame) void decode(frame, currentGeneration);
    };
    connection.onconnectionstatechange = () => {
      if (currentGeneration !== generation) return;
      log(`Peer: ${connection.connectionState}`);
      if (['failed', 'disconnected', 'closed'].includes(connection.connectionState)) stop();
    };
    await connection.setLocalDescription(await connection.createOffer());
    if (connection.iceGatheringState !== 'complete') await new Promise<void>(resolve => {
      const timer = setTimeout(resolve, 3000);
      connection.onicegatheringstatechange = () => {
        if (connection.iceGatheringState === 'complete') { clearTimeout(timer); resolve(); }
      };
    });
    if (currentGeneration !== generation) return;
    await signal({ type: 'offer', sdp: connection.localDescription!.sdp });
    pollTimer = setInterval(() => void poll(currentGeneration), 250);
    timeout = setTimeout(() => {
      if (currentGeneration === generation && !streamingStarted) {
        element('error').textContent = 'Camera is busy or unavailable. Check the serial experiment log.';
        stop();
      }
    }, 20_000);
  } catch (error) {
    if (currentGeneration !== generation) return;
    element('error').textContent = String(error);
    stop();
  }
}
function stop() {
  const oldSession = session;
  if (startTime && !stopTime) stopTime = performance.now();
  generation++;
  command({ type: 'stop' });
  control = undefined;
  const connection = pc;
  pc = undefined;
  if (connection) { connection.onconnectionstatechange = null; connection.close(); }
  if (oldSession) void fetch('/lab/signal', {
    method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ type: 'close', session: oldSession }), keepalive: true,
  }).catch(() => {});
  session = '';
  if (pollTimer) clearInterval(pollTimer);
  if (pingTimer) clearInterval(pingTimer);
  if (timeout) clearTimeout(timeout);
  pending = undefined; jpegReady = streamingStarted = false;
  startButton.disabled = false; stopButton.disabled = true;
  state('Stopped');
}
startButton.onclick = () => void start();
stopButton.onclick = stop;
element('apply').onclick = settings;
element('fullscreen').onclick = () => { void canvas.requestFullscreen?.(); };
document.addEventListener('visibilitychange', () => { if (document.hidden) stop(); });
window.addEventListener('pagehide', stop);
setInterval(report, 500);
report();
