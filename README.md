# Garage camera

Firmware for the Freenove ESP32-WROVER with an OV2640 camera. Choose one sketch:

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

Connect the board to reliable USB power and a 2.4 GHz Wi-Fi network. Other devices
on that network can view the camera. This is a trusted-LAN HTTP service with no
password; do not forward its port to the internet. There are no CDN dependencies.

1. Install Arduino CLI and the ESP32 core:

   ```sh
   arduino-cli core update-index --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
   arduino-cli core install esp32:esp32 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```

2. Copy `firmware/camera_stream/wifi_credentials.example.h` to
   `firmware/camera_stream/wifi_credentials.h` and enter your Wi-Fi details.
   The local credentials file is ignored by git.

3. Compile and upload (460800 baud is reliable on this board):

   ```sh
   arduino-cli compile --fqbn esp32:esp32:esp32wrover --build-path .arduino-build/camera_stream firmware/camera_stream
   arduino-cli upload --fqbn esp32:esp32:esp32wrover:UploadSpeed=460800 --port /dev/cu.usbserial-10 --input-dir .arduino-build/camera_stream firmware/camera_stream
   ```

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

## Measure received frame rate

The standard-library Python tool validates frame boundaries, complete JPEGs,
dimensions and capture timestamps, then measures frames actually received after
a warmup. It restores the original settings when it finishes. Pause browser
streams first.

```sh
python3 tools/benchmark_camera.py http://garage-camera.local --seconds 30
python3 tools/benchmark_camera.py http://garage-camera.local --resolution UXGA --quality 20 --seconds 30
python3 tools/benchmark_camera.py http://garage-camera.local --resolution VGA --save-frame /tmp/garage-camera.jpg
```

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
