# Garage camera

Firmware for the Freenove ESP32-WROVER with a camera. The connected unit reports
an **OV3660** (sensor PID `0x3660`); earlier project notes identified it as an
OV2640. The streaming page keeps its existing 1600 × 1200 maximum. Choose one sketch:

- `firmware/camera_stream`: a local camera webpage with MJPEG streaming, resolution
  and JPEG compression controls, and live performance readings.
- `firmware/garage_monitor`: the original five-minute capture → AWS Lambda →
  Bedrock/SNS monitor.

Uploading one sketch replaces the firmware currently running on the board.
The streaming sketch stays awake and does not upload images to AWS or send alerts.

## Local camera webpage

The default is **1600 × 1200 JPEG**, compression **12**, with two frame buffers in
PSRAM. The page supports 320 × 240 through 1600 × 1200 and compression 10–40
(lower means sharper pictures and larger files). Changes last until restart.

The optimized build uses a **32 KiB TCP send buffer** instead of the SDK's
5,744-byte default. At 2 MP and compression 12, controlled tests improved from
7.42 to 13.47 received FPS; the finished build sustained 13.37 FPS for three
minutes with every JPEG decoded successfully. The camera clock stays at 20 MHz;
faster-clock experiments produced corrupt JPEGs and were rejected. Scene and
Wi-Fi conditions affect the result. See `firmware/camera_stream/VALIDATION.md`
for the measurements.

Connect the board to reliable USB power and a 2.4 GHz Wi-Fi network. Other devices
on that network can view the camera. This is a trusted-LAN HTTP service with no
password; do not forward its port to the internet. There are no CDN dependencies.

1. Install Arduino CLI and the ESP32 core:

   ```sh
   arduino-cli core update-index --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
   arduino-cli core install esp32:esp32@3.3.11 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```

2. Copy `firmware/camera_stream/wifi_credentials.example.h` to
   `firmware/camera_stream/wifi_credentials.h` and enter your Wi-Fi details.
   The local credentials file is ignored by git.

3. Compile and upload the optimized build (Python 3.12+ and Arduino ESP32 core
   **3.3.11**):

   ```sh
   python3 tools/build_camera.py --upload
   ```

   Omit `--upload` to compile only, or supply `--port` if the serial port changes.
   Uploads use 460800 baud.

   The helper downloads the lwIP revision from ESP-IDF 5.5.5, checks its headers
   against the installed SDK, and rebuilds the four TCP source files affected by
   the larger send buffer. It modifies a **local copy** of the library under
   `.arduino-build/`, preserves the other SDK settings, and verifies the link map.
   The installed Arduino SDK is unchanged. Sources are cached after the first
   build, and `profile.json` beside the local library records its provenance.

   Use `python3 tools/build_camera.py --stock-network` for a control build using
   the unmodified SDK. A direct `arduino-cli compile` also uses the stock network
   settings. The optimized profile deliberately requires the tested core version;
   it must be revalidated when upgrading the SDK.

4. Open the IP address printed on the serial console at 115200 baud from a browser
   on the same network. This board was reachable at **http://CAMERA_IP/** during
   verification. DHCP can change that address after reconnecting.
   **http://garage-camera.local/** is also advertised, but local-name resolution
   did not work on the test Mac. Guest Wi-Fi client isolation can prevent access.

The webpage starts streaming automatically. Pause it before viewing from another
device or running a benchmark. A second concurrent viewer gets HTTP 503. The
stream uses an asynchronous worker so the webpage and controls remain available
while frames are sent. Slow or disconnected clients are released; Wi-Fi reconnects
automatically. Camera initialization errors are printed on serial.

The page's **Sent FPS** and **Data rate** count complete JPEG payloads written by
the ESP32, averaged over approximately one second. They are not browser-rendered
FPS or a promise of 15 fps. Image complexity, exposure, Wi-Fi and JPEG compression
affect throughput.

`/status` also exposes `capture_ms` (time to acquire a buffered JPEG, not the
sensor's exposure/readout time) and `send_ms` (time spent sending that JPEG).
These help distinguish waiting for a frame from waiting for the network.
It also reports `sensor_pid`, `xclk_mhz`, and `tcp_send_buffer`, so the running
firmware's camera and network configuration can be checked directly.

## Measure received frame rate

The standard-library Python tool validates frame boundaries, complete JPEGs,
dimensions and capture timestamps, then measures frames actually received after
a warmup. It rejects repeated or out-of-order capture timestamps. It restores the
original settings when it finishes. Pause browser streams first.

```sh
python3 tools/benchmark_camera.py http://garage-camera.local --seconds 30
python3 tools/benchmark_camera.py http://garage-camera.local --resolution UXGA --quality 20 --seconds 30
python3 tools/benchmark_camera.py http://garage-camera.local --resolution VGA --save-frame /tmp/garage-camera.jpg
python3 tools/benchmark_camera.py http://CAMERA_IP --resolution UXGA --quality 12 --seconds 180 --decode
```

`--decode` requires Pillow and decodes every JPEG, including warmup frames. This
is stronger than checking JPEG markers alone and caught the invalid images in
the rejected clock experiments. Without that option, the benchmark needs only
Python's standard library. Reports include median and p95 delivery intervals,
capture timestamp intervals, and the number of fully decoded measured frames.

Substitute `http://CAMERA_IP` (or the current serial-reported IP) when local-name
lookup is unavailable. Hardware measurements and verification limits are recorded
in `firmware/camera_stream/VALIDATION.md`.

Optional `--json-output result.json` saves the report. The stream and control API
share port 80:

| Endpoint | Purpose |
| --- | --- |
| `GET /` | Camera webpage |
| `GET /status` | Settings, stream counters, Wi-Fi signal and free memory |
| `GET /stream?viewer=<hex-id>` | MJPEG; one viewer, 1–32 hexadecimal characters |
| `POST /settings` | Form fields `resolution=UXGA&quality=12` |
| `POST /stop?viewer=<hex-id>` | Stop only that viewer's stream |

## Restore the garage monitor

For a fresh checkout, create the ignored local configuration file:

```sh
cp firmware/garage_monitor/secrets.example.h firmware/garage_monitor/secrets.h
```

Enter your Wi-Fi network name, password, and Lambda Function URL in `secrets.h`,
then compile and upload:

```sh
arduino-cli compile --fqbn esp32:esp32:esp32wrover firmware/garage_monitor
arduino-cli upload --fqbn esp32:esp32:esp32wrover:UploadSpeed=460800 --port /dev/cu.usbserial-10 firmware/garage_monitor
```

Infrastructure commands and the hardware pin mapping are documented in `AGENTS.md`.

The original 4 MiB flash image for this checkout was also saved locally as
`.arduino-build/garage-monitor-backup.bin`, with a SHA-256 sidecar file.
Build output and this backup are ignored by git and can contain Wi-Fi credentials.
