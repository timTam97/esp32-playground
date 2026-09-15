# WebRTC browser harness

A TypeScript/Vite viewer for the experimental
[JPEG-over-WebRTC firmware](../firmware/camera_webrtc/README.md). It provides
Start/Stop, fullscreen, image settings, decoded-frame measurements, and
direct/relay status. The experiment failed its sustained local acceptance gate;
see the [measured result](../firmware/camera_webrtc/README.md#measured-result--15-september-2026).

## Build and test

Run from the repository root:

```sh
npm --prefix web ci
npm --prefix web test
npm --prefix web run build
```

## Run locally

After flashing experimental firmware, run these in separate terminals:

```sh
.venv/bin/python tools/webrtc_lab.py
npm --prefix web run dev
```

Open `http://127.0.0.1:5173` on the same Mac and press Start. Stop sends a control
message, closes the peer and releases the camera. Backgrounding the page also
stops viewing; returning requires Start again.

The bridge listens only on loopback. It carries session descriptions and control
events over USB; JPEG payloads travel between the ESP32 and browser over DTLS/SCTP.
It rejects competing sessions, caps command size and redacts SDP in its log.

The tested peer library does not resolve Chrome's mDNS-obscured host candidates.
The local lab used an isolated Chrome profile launched with:

```text
--disable-features=WebRtcHideLocalIpsWithMdns
```

This is a lab-only setting. Normal Chrome/Safari operation with STUN/TURN has not
been verified. Do not change an everyday browser profile for this experiment.

The test interface exposes browser-decoded frame counts and device measurements
in `window.garageMetrics`, as well as in the visible measurements panel.

The Vite development server listens on port 5173 and proxies `/lab` to the
USB bridge on `127.0.0.1:8766`, as configured in [vite.config.ts](vite.config.ts).
Supply `--port` to the bridge command if the ESP32's USB device path changes.

The firmware README documents the
[data-channel and signaling protocol](../firmware/camera_webrtc/README.md#wire-interface),
including chunk sizes, frame limits, and control messages.
