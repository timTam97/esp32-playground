#pragma once

// Self-contained page: no internet connection, fonts, or CDN dependencies.
static const char CAMERA_PAGE[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="color-scheme" content="light">
<link rel="icon" href="data:,">
<title>Garage camera</title>
<style>
:root{font:16px/1.5 "Avenir Next",Avenir,"Segoe UI",sans-serif;color:#213747;background:#edf2f5;--blue:#185e88;--muted:#526a7a;--line:#c9d5de}
*{box-sizing:border-box}body{margin:0}button,select,input{font:inherit}
button,select{min-height:44px;border:1px solid var(--line);border-radius:6px;background:#fff;color:inherit}
button{padding:9px 18px;cursor:pointer;font-weight:600}button:hover{background:#e6eef4}
button.primary{background:var(--blue);color:white;border-color:var(--blue)}button.primary:hover{background:#12486a}
button:disabled{opacity:.55;cursor:wait}:focus-visible{outline:3px solid #d68522;outline-offset:3px}
.shell{max-width:1440px;margin:auto;padding:24px 32px 32px}
header{display:flex;align-items:center;justify-content:space-between;gap:16px;margin-bottom:20px}
h1{margin:0;font-size:clamp(24px,3vw,34px);letter-spacing:-1px;font-weight:650}
.connection{display:flex;align-items:center;gap:8px;color:var(--muted);font-size:14px}
.dot{width:9px;height:9px;background:#758896;border-radius:50%}.dot.live{background:#247346}
main{display:grid;grid-template-columns:minmax(0,1fr) 280px;align-items:start;gap:24px}
.view{position:relative;width:100%;aspect-ratio:4/3;max-height:calc(100svh - 210px);min-height:180px;background:#152632;overflow:hidden;border-radius:8px}
.view img{display:block;width:100%;height:100%;object-fit:contain}.view img[hidden]{display:none}
.placeholder{position:absolute;inset:0;display:grid;place-content:center;text-align:center;color:#d9e5ee;padding:28px;pointer-events:none}
.placeholder[hidden]{display:none}.placeholder svg{margin:0 auto 16px;opacity:.8}.placeholder p{margin:0;max-width:35ch}
.view:fullscreen{border-radius:0;aspect-ratio:auto;max-height:none;background:#000}
.view:fullscreen img{width:100vw;height:100vh}
.exit-fullscreen{display:none;position:absolute;top:16px;right:16px}
.view:fullscreen .exit-fullscreen{display:block}
.under-video{display:flex;justify-content:space-between;align-items:center;gap:12px;padding:12px 0}
.under-video span{color:var(--muted);font-size:14px}
aside{padding:4px 0}h2{font-size:18px;margin:0 0 16px;font-weight:650}
label{display:block;font-weight:600;margin:20px 0 8px}label:first-of-type{margin-top:0}
select{width:100%;padding:9px 10px}input[type=range]{width:100%;accent-color:var(--blue);margin:6px 0}
.range-label{display:flex;align-items:center;justify-content:space-between}.range-label label{margin-bottom:4px}
output{color:var(--muted);font-variant-numeric:tabular-nums}
.range-hints{display:flex;justify-content:space-between;color:var(--muted);font-size:12px}
.apply{width:100%;margin-top:22px}.notice{font-size:13px;color:var(--muted);min-height:40px;margin:10px 0 24px}
.notice.error{color:#a33324}.metrics{border-top:1px solid var(--line);padding-top:18px}
dl{margin:0}dl>div{display:flex;justify-content:space-between;align-items:baseline;padding:7px 0}
dt{color:var(--muted);font-size:14px}dd{margin:0;font-size:17px;font-variant-numeric:tabular-nums}
.fine{color:var(--muted);font-size:12px;line-height:1.6;margin:16px 0 0;max-width:44ch}
footer{margin-top:16px;font-size:12px;color:var(--muted)}
@media(max-width:800px){.shell{padding:18px 16px}main{grid-template-columns:1fr;gap:14px}aside{display:grid;grid-template-columns:1fr 1fr;gap:24px}.notice{margin-bottom:0}.metrics{border-top:0;border-left:1px solid var(--line);padding:0 0 0 20px}.connection{max-width:45%;text-align:right}}
@media(max-width:460px){aside{grid-template-columns:1fr;gap:20px}.metrics{border-left:0;border-top:1px solid var(--line);padding:16px 0 0}.connection{font-size:12px}.under-video{flex-wrap:wrap}header{align-items:flex-start}}
</style>
</head>
<body>
<div class="shell">
  <header><h1>Garage camera</h1><div class="connection" role="status"><span id="dot" class="dot"></span><span id="connection">Connecting…</span></div></header>
  <main>
    <section aria-label="Camera view">
      <div class="view" id="view">
        <img id="camera" alt="Live view from the garage camera" hidden>
        <div id="placeholder" class="placeholder">
          <svg width="52" height="42" viewBox="0 0 52 42" fill="none" aria-hidden="true"><path d="M5 12h10l4-7h14l4 7h10v25H5z" stroke="currentColor" stroke-width="2"/><circle cx="26" cy="24" r="8" stroke="currentColor" stroke-width="2"/></svg>
          <p id="viewMessage">Connecting to the camera…</p>
        </div>
        <button class="exit-fullscreen" id="exitFullscreen">Exit full screen</button>
      </div>
      <div class="under-video">
        <button class="primary" id="toggle" disabled>Start stream</button>
        <span id="dimensions">1600 × 1200</span>
        <button id="fullscreen">Full screen</button>
      </div>
    </section>
    <aside>
      <form id="settings">
        <h2>Picture settings</h2>
        <label for="resolution">Resolution</label>
        <select id="resolution" name="resolution">
          <option value="UXGA">1600 × 1200 · 2 MP</option>
          <option value="SXGA">1280 × 1024</option>
          <option value="XGA">1024 × 768</option>
          <option value="SVGA">800 × 600</option>
          <option value="VGA">640 × 480</option>
          <option value="QVGA">320 × 240</option>
        </select>
        <div class="range-label"><label for="quality">JPEG compression</label><output for="quality" id="qualityValue">12</output></div>
        <input type="range" id="quality" name="quality" min="10" max="40" value="12">
        <div class="range-hints"><span>Sharper picture</span><span>Smaller files</span></div>
        <button type="submit" class="apply" id="apply">Apply settings</button>
        <p class="notice" id="notice" role="status">Settings reset when the camera restarts.</p>
      </form>
      <section class="metrics" aria-label="Stream performance">
        <h2>Stream performance</h2>
        <dl>
          <div><dt>Sent FPS</dt><dd id="fps">—</dd></div>
          <div><dt>Data rate</dt><dd id="bitrate">—</dd></div>
          <div><dt>JPEG size</dt><dd id="jpeg">—</dd></div>
          <div><dt>Wi-Fi signal</dt><dd id="signal">—</dd></div>
        </dl>
        <p class="fine">FPS counts complete images sent by the camera. Browser playback may be slower. Try smaller files or a lower resolution for smoother video.</p>
      </section>
    </aside>
  </main>
  <footer>One viewer at a time. Keep the camera powered while viewing.</footer>
</div>
<script>
const $ = id => document.getElementById(id);
const viewer = Array.from(crypto.getRandomValues(new Uint32Array(2)), n => n.toString(16).padStart(8, '0')).join('');
const camera = $('camera');
let wanted = false, starting = false, initialized = false, startTime = 0;
function connection(text, live = false) {
  $('connection').textContent = text;
  $('dot').classList.toggle('live', live);
}
function placeholder(text) {
  $('viewMessage').textContent = text;
  $('placeholder').hidden = false;
  camera.hidden = true;
}
function notice(text, error = false) {
  $('notice').textContent = text;
  $('notice').classList.toggle('error', error);
}
async function api(path, options = {}) {
  const response = await fetch(path, {...options, cache:'no-store', signal:AbortSignal.timeout(10000)});
  const data = await response.json();
  if (!response.ok) throw new Error(data.error || 'Camera request failed');
  return data;
}
function renderStatus(s) {
  $('signal').textContent = `${s.rssi} dBm`;
  $('fps').textContent = s.streaming ? s.fps.toFixed(1) : '—';
  $('bitrate').textContent = s.streaming ? `${s.mbps.toFixed(1)} Mb/s` : '—';
  $('jpeg').textContent = s.streaming && s.frame_bytes ? `${(s.frame_bytes/1024).toFixed(0)} KiB` : '—';
  $('dimensions').textContent = `${s.width} × ${s.height}`;
  $('view').style.aspectRatio = `${s.width} / ${s.height}`;
  if (!initialized) {
    $('resolution').value = s.resolution;
    $('quality').value = s.quality;
    $('qualityValue').value = s.quality;
    initialized = true;
  }
  $('toggle').disabled = starting;
  if (wanted && s.streaming && s.viewer === viewer) {
    const live = s.last_frame_ms >= 0 && s.last_frame_ms < 3000;
    connection(live ? 'Live' : 'Waiting for frames…', live);
    if (live) {camera.hidden = false; $('placeholder').hidden = true;}
  } else if (s.streaming && s.viewer !== viewer) {
    if (wanted) {wanted = false; camera.removeAttribute('src'); $('toggle').textContent = 'Start stream';}
    connection('Another viewer is connected');
    placeholder('Close the stream on the other device, then select Start stream.');
  } else if (wanted && Date.now() - startTime > 7000) {
    stopStream();
    connection('Stream interrupted');
    placeholder('The stream stopped. Select Start stream to reconnect.');
  } else if (!wanted) {
    connection('Camera ready');
    placeholder('Select Start stream to view the camera.');
  }
}
async function startStream() {
  if (starting || wanted) return;
  starting = true;
  $('toggle').disabled = true;
  try {
    let status = await api('/status');
    // A previous document may still be releasing its socket after a reload.
    for (let attempt = 0; status.streaming && attempt < 8; attempt++) {
      await new Promise(resolve => setTimeout(resolve, 400));
      status = await api('/status');
    }
    renderStatus(status);
    if (status.streaming) {
      placeholder(status.viewer === viewer
        ? 'The previous stream is still closing. Select Start stream again in a moment.'
        : 'Close the stream on the other device, then select Start stream.');
      return;
    }
    wanted = true;
    startTime = Date.now();
    placeholder('Starting the live view…');
    connection('Starting stream…');
    $('toggle').textContent = 'Pause stream';
    camera.src = `/stream?viewer=${viewer}&t=${Date.now()}`;
  } catch (error) {
    connection('Camera unreachable');
    placeholder('Check the camera’s power and Wi-Fi, then select Start stream.');
  } finally {
    starting = false;
    $('toggle').disabled = false;
  }
}
function stopStream() {
  wanted = false;
  camera.removeAttribute('src');
  camera.hidden = true;
  $('toggle').textContent = 'Start stream';
  placeholder('Stream paused. Select Start stream to resume.');
  connection('Paused');
  // The viewer ID ensures a late page-close request cannot stop another tab.
  fetch(`/stop?viewer=${viewer}`, {method:'POST', keepalive:true}).catch(() => {});
}
camera.addEventListener('error', () => {
  if (!wanted) return;
  stopStream();
  connection('Stream unavailable');
  placeholder('The stream could not start. Close other viewers and try again.');
});
$('toggle').addEventListener('click', () => wanted ? stopStream() : startStream());
$('quality').addEventListener('input', () => {$('qualityValue').value = $('quality').value;});
$('settings').addEventListener('submit', async event => {
  event.preventDefault();
  $('apply').disabled = true;
  notice('Applying settings…');
  try {
    const status = await api('/settings', {method:'POST', body:new URLSearchParams(new FormData(event.target))});
    renderStatus(status);
    notice('Settings applied. Allow a moment for the picture to settle.');
  } catch (error) {
    notice(error.message, true);
  } finally {$('apply').disabled = false;}
});
if (!document.fullscreenEnabled) $('fullscreen').hidden = true;
$('fullscreen').addEventListener('click', async () => {
  try {
    if (document.fullscreenElement) await document.exitFullscreen();
    else await $('view').requestFullscreen();
  } catch (error) {notice('Full screen is unavailable in this browser.', true);}
});
$('exitFullscreen').addEventListener('click', async () => {
  try {await document.exitFullscreen();}
  catch (error) {notice('Use your browser’s full-screen control to exit.', true);}
});
window.addEventListener('pagehide', () => {
  if (wanted) navigator.sendBeacon(`/stop?viewer=${viewer}`);
});
async function poll() {
  try {renderStatus(await api('/status'));}
  catch (error) {
    connection('Camera unreachable');
    $('fps').textContent = $('bitrate').textContent = $('jpeg').textContent = '—';
    if (wanted) stopStream();
    placeholder('Check the camera’s power and Wi-Fi, then select Start stream.');
    connection('Camera unreachable');
  }
  setTimeout(poll, 1500);
}
startStream().then(poll);
</script>
</body>
</html>
)HTML";
