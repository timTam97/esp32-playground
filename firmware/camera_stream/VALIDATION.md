# Hardware verification — 14 September 2026

Tested on the connected Freenove ESP32-WROVER / OV2640, ESP32-D0WD-V3 revision
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
