# 2 MP JPEG over WebRTC experiment

**Status: the local acceptance gate failed on 15 September 2026.** The original
Arduino firmware was restored and verified at the end of that experiment.
The private AWS viewer was not deployed.

This ESP-IDF firmware sends camera JPEGs through an encrypted WebRTC data
channel. The [browser harness](../../web/README.md) uses USB for signaling;
JPEG payloads travel directly between the board and browser.

See [the research and implementation record](../../docs/JPEG_WEBRTC_RESEARCH.md)
for detailed evidence and the proposed remote-viewer architecture.

## Measured result — 15 September 2026

**Result: the local acceptance gate failed.** The requirement was at least
2 browser-decoded fps for ten minutes while preserving 2 MP detail.

| Measurement | Observed result |
| --- | --- |
| Initial short sample | Approximately 2.72 decoded fps |
| Formal measurement window | 317 seconds; connection ended before ten minutes |
| Frames decoded during that window | 577, averaging **1.82 fps** |
| Valid decoded frames across the full session | 701 |
| JPEG decode errors / malformed chunks | 0 / 0 |
| Incomplete frames at gate completion | 104 |
| Final rolling estimated p95 display latency | 1.83 seconds |

Send calls developed stalls and the peer logged DTLS retry errors as Wi-Fi
signal weakened. The cause has not been isolated. A further experiment should
establish a repeatable stronger-signal baseline, compare encryption modes, and
investigate DTLS/SCTP backpressure and packet sizing.

The **original firmware was restored**, its complete 4 MiB flash digest matched
the recovery snapshot, and the original HTTP stream again supplied valid 2 MP
JPEGs. Native and web builds succeeded; seven transport tests and four recovery
tests passed. These checks do not replace the failed sustained hardware gate.

No AWS resources or Cognito account were created. Remote STUN/TURN, the
password-protected deployment, twenty-session stability, one-hour renewal,
physical iPhone Safari, and billing validation were not reached.

## Build environment

- ESP-IDF 5.5.5, revision `b774170ff46c393eeb5e495ea37936038d3f4f4f`
- `esp_peer` 1.5.5 and `esp32-camera` 2.1.7; transitive versions are in
  `dependencies.lock`
- ESP32-WROVER, OV3660 on the tested board, 4 MiB flash/PSRAM
- 1600×1200 JPEG, quality 12 initially, two frame buffers, 20 MHz camera clock
- 3 MiB app partition, 80 MHz PSRAM, 240 MHz CPU, separate receive task
- 2 Mbps JPEG payload limit

The build uses the existing ignored `firmware/camera_stream/wifi_credentials.h`.
Create it from that directory's example if necessary. Generated binaries and full
flash backups contain private configuration and must stay out of source control.

Run from the repository root:

```sh
python3 tools/webrtc.py setup
python3 tools/webrtc.py build
```

The isolated SDK, compiler and Python environment live under
`.arduino-build/webrtc-tools/`. The build helper regenerates SDK configuration
when the checked-in defaults change. It does not alter the installed Arduino SDK.

## Backup, flash and recover

Create a new full-flash backup before an experiment:

```sh
python3 tools/webrtc.py backup --output .arduino-build/webrtc-backups/BEFORE.bin
python3 tools/webrtc.py flash --backup .arduino-build/webrtc-backups/BEFORE.bin
```

The tools use `/dev/cu.usbserial-10` by default; supply `--port` to change it.
Reads use 115200 baud; writes use 460800 baud. The recovery file must be exactly
4 MiB and match its SHA-256 sidecar. Verification occurs before booting the
application, because booting can modify Wi-Fi state in NVS.

Stop the USB signaling bridge before flashing or restoring:

```sh
python3 tools/webrtc.py restore --backup .arduino-build/webrtc-backups/BEFORE.bin
```

The verified recovery snapshot from the completed experiment is:

```text
.arduino-build/webrtc-backups/pre-webrtc-20260915-verified.bin
SHA-256: 1735cdf6e2b0d8062cb82da49cc0655bd709b3a96ee74134399fad4c80675f31
```

## Local browser harness

See the [browser project README](../../web/README.md) for its build, USB bridge,
local viewing instructions, and browser compatibility limits.

## Wire interface

The browser creates `control` (reliable, ordered) and `jpeg` (unordered,
`maxRetransmits: 1`) data channels. No audio or native video tracks are created.
Channel IDs are obtained from channel-open callbacks, not assumed.

Every JPEG message begins with a 32-byte, network-byte-order header:

| Offset | Field |
| --- | --- |
| 0 | Four-byte `GJPG` magic |
| 4 | Version 1 |
| 5 | Flags, currently zero |
| 6 | uint16 header length, 32 |
| 8 | uint32 frame ID |
| 12 | uint64 capture timestamp in device-monotonic milliseconds |
| 20 | uint32 full JPEG length |
| 24 | uint16 chunk index |
| 26 | uint16 chunk count |
| 28 | uint16 payload length |
| 30 | uint16 reserved, zero |

Chunk payloads are 10,000 bytes except the final chunk. Frames are limited to
512 KiB. The viewer bounds assembly to two frames, expires incomplete frames
after one second, and keeps at most one pending JPEG decode. Complete images
must decode at 1600×1200 before display.

Control JSON supports `start`, `stop`, `settings` (`fps`: 2–5, `quality`: 10–40),
`status`, and timestamped `ping`/`pong`. USB signaling additionally supports
session-scoped `offer`, `candidate`, and `close`.

## Remaining gates

The ten-minute ≥2 decoded-fps gate has not passed. Before proceeding to AWS,
investigate the measured DTLS/SCTP send stalls, negotiated cipher choice and
behavior under packet loss. Preserve the 2 MP minimum throughout.

Remote STUN/TURN, device certificates, Cognito login, CloudFront deployment,
20-cycle stability, hour-long renewal, physical iPhone Safari and billing
validation have not been implemented or verified.
