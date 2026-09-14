# Hardware verification — 14 September 2026

## FPS optimization

The connected camera identifies itself as **OV3660, PID `0x3660`**. The earlier
OV2640 identification in project notes was incorrect for this unit.

The accepted change increases the TCP send buffer from **5,744 to 32,768 bytes**.
The camera keeps **1600 × 1200, JPEG compression 12, 20 MHz XCLK, two PSRAM
buffers, and the original asynchronous HTTP/MJPEG implementation**.

All following streaming runs checked complete JPEGs, dimensions, increasing
capture timestamps, and decoded **every** JPEG with Pillow, including warmup.
They captured an indoor ceiling/room scene with a three-second warmup. Wi-Fi
and image size varied; the final RSSI reading and JPEG sizes show those limits.

| Configuration | Received FPS | Mean JPEG | Final RSSI | Measured duration | Decoded measured frames |
| --- | ---: | ---: | ---: | ---: | ---: |
| Original TCP buffer, control | 7.42 | 45.0 KiB | −53 dBm | 25.1 s | 186 |
| 32 KiB TCP buffer | 13.47 | 45.3 KiB | −53 dBm | 25.0 s | 337 |
| 32 KiB, longer run | 13.37 | 50.8 KiB | −76 dBm | 120.0 s | 1,605 |
| Original firmware restored and retested | 6.89 | 49.7 KiB | −79 dBm | 30.0 s | 207 |
| Finished optimized build, stability run | 13.37 | 53.6 KiB | −78 dBm | 180.0 s | 2,407 |

The first controlled comparison improved received FPS by **81.5%** at the same
resolution and compression. Capture-only timing at the original clock was
13.9 FPS. The larger TCP window lets more JPEG data be queued while earlier
packets are acknowledged, bringing delivery close to that capture rate.

Raw reports are in `.arduino-build/fps-stock-tcp-fresh-0-mode0-q12.json`,
`fps-tcp32-buffers2-clock20-0-mode0-q12.json`,
`fps-tcp32-clock20-stability-0-mode0-q12.json`, and
`fps-original-repeat-q12.json` in the same directory. The finished build's
three-minute report is `.arduino-build/fps-final-uxga-q12.json`; its sample
image was also visually checked.

At the user's existing compression setting of **27**, the finished build
delivered **13.40 FPS** over 20.1 seconds, with 269 unique, successfully decoded
2 MP frames. That report is `.arduino-build/fps-final-uxga-q27.json`.

### Experiments not retained

- Raw multipart output, scatter/gather writes, and TCP_NODELAY did not provide
  a consistent additional improvement. The original HTTP streaming code remains.
- A third camera buffer did not show a reliable improvement.
- A 24 MHz clock reached 16.21 FPS in a short run, but subsequent full JPEG
  decoding found corrupt images. Keeping its pixel bus at 10 MHz also failed
  decoding. Both faster-clock configurations were rejected.
- Temporary tuning, capture-probe, and task-diagnostic endpoints were removed.
  They return HTTP 404 in the finished firmware.

### Build and runtime verification

`python3 tools/build_camera.py` builds the optimized profile using Arduino ESP32
3.3.11 / ESP-IDF 5.5.5. The helper verified **164 lwIP headers** against source
revision `fd432e4ee2cfb7f7f1c7eb7227e0173412e7b84e`, rebuilt the four affected
TCP source files in a local library copy, and checked all four selections in the
firmware link map. The installed SDK was not edited.

The finished build uses **1,069,381 bytes of program flash (81%)** and
**59,904 bytes of static RAM (18%)**. Its 460800-baud upload passed flash hash
verification. The stock-network build also compiles and selects the original
SDK library. The original and final firmware binaries are saved locally in
`.arduino-build/fps-original/` and `.arduino-build/fps-final/`.

Runtime checks on the finished firmware passed:

- The three-minute 2 MP run delivered 2,407 unique, fully decoded measured
  frames without an error, plus successfully decoded warmup frames.
- `/status` reported sensor PID `13920`, `xclk_mhz: 20`, and
  `tcp_send_buffer: 32768`; it responded in 40.5 ms during streaming.
- Second viewers, malformed viewer IDs and invalid settings were rejected.
  A stop request for another viewer did not interrupt the stream.
- Live changes between VGA, SXGA and UXGA, and compression 10, 12 and 40,
  produced correctly decoded frames.
- Clean stopping and four abrupt disconnect/reconnect cycles released the
  viewing session.
- A client that stopped reading could be stopped in 5.27 seconds while control
  requests remained responsive; another viewer could then connect.
- Idle free heap after these checks was 153,576 bytes, with no restart or
  reported capture error.
- The browser displayed the live 2 MP image, and pause/resume worked. The
  original compression setting of 27 was restored and the viewer left paused.

The larger TCP window uses more internal RAM while data is queued. One active
compression-27 sample had 68,756 bytes free; after stopping, free heap recovered
to 154,780 bytes.

The runtime transcript is `.arduino-build/fps-final-runtime-checks.txt`.
The build provenance and flashed firmware SHA-256 are recorded in
`.arduino-build/fps-final/profile.json`.

## Earlier streaming baseline

The following notes and measurements predate the TCP-window optimization.

Tested on the connected Freenove ESP32-WROVER, ESP32-D0WD-V3 revision
3.1, with 4 MiB PSRAM. Arduino ESP32 core 3.3.11, FQBN
`esp32:esp32:esp32wrover`, default partition scheme and 20 MHz camera clock.

The sketch compiles using 1,068,973 bytes of flash (81%) and 59,904 bytes of
internal static RAM (18%). Uploads at 460800 baud passed flash hash verification.
The original 4 MiB flash image was backed up before the first upload.

## Received frame rate

One Python client on the existing Wi-Fi, three-second warmup, then 20–30 seconds
of complete JPEGs. The camera was indoors, with a partly obstructed view of a
ceiling. These are measurements for that scene and network, not hardware limits.
Every measured frame had a unique capture timestamp and the expected dimensions.

| Resolution | JPEG compression | Received FPS | Mean JPEG size | Duration |
| --- | ---: | ---: | ---: | ---: |
| 1600 × 1200 | 12 | 1.90 | 91.7 KiB | 30 s |
| 1600 × 1200 | 25 | 4.08 | 60.9 KiB | 20 s |
| 1600 × 1200 | 40 | 4.97 | 47.6 KiB | 20 s |
| 1600 × 1200 | 25, repeat with timing diagnostics | 6.17 | 60.2 KiB | 20 s |
| 640 × 480 | 12 | 14.57 | 9.2 KiB | 20 s |

**15 fps at 2 MP was not achieved.** In the 6.17 fps run, the final one-second
window averaged 0.92 ms to acquire a buffered frame and 157.78 ms to send it.
This indicates transmission dominated that run; buffered acquisition time is
not a measurement of the sensor's exposure/readout time. Signal strength was
roughly −60 to −68 dBm across runs, and network performance varied.

A TCP_NODELAY experiment measured 4.21 fps at compression 25. It did not
demonstrate an improvement and was removed from the final firmware.

Raw reports are kept locally under `.arduino-build/benchmark-*.json`.

## Runtime checks

- Full-resolution JPEGs received and decoded using Pillow.
- Control/status requests responded while streaming (51 ms in the final
  protocol-check run).
- Invalid settings and malformed viewer IDs returned HTTP 400 without changing
  settings.
- A second viewer received HTTP 503.
- A stop request for another viewer did not interrupt the active stream.
- Resolution/compression changes during streaming produced correctly decoded
  VGA, SXGA and UXGA images.
- Stopping the viewer released the camera.
- Six abrupt disconnect/reconnect cycles recovered.
- A client that stopped reading could be stopped while the control API remained
  responsive.
- The camera page displayed the live image in the Codex browser; its resolution
  and compression controls updated the running camera.
- Browser pause/resume and refreshing an active stream recovered automatically.
- Full-screen viewing and the on-screen Exit full screen button worked.
- A 390-pixel browser viewport rendered the live 1600 × 1200 image without
  horizontal overflow; desktop viewing kept the playback controls visible.

The final protocol-check output is saved locally in
`.arduino-build/runtime-checks.txt`. The browser preview was left paused to
release the single viewing connection.

The reliable address in this session was `http://CAMERA_IP/`. The firmware
advertised `garage-camera.local`, but both the browser and the Mac's resolver
failed to resolve it.

Long-duration unattended operation, deliberate Wi-Fi outages, battery runtime
and physical mobile-device playback have not been tested. The original AWS
monitor sketch is preserved, but AWS uploads/alerts do not run in streaming mode.
