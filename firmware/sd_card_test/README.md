# SD-card hardware diagnostic

This temporary sketch tests the Freenove ESP32-WROVER's SD slot and its ability
to save camera JPEGs. It waits for a serial command before creating any files.
Automatic formatting is disabled.

## Verified on 15 September 2026

The user's **64 GB microSD card**, formatted FAT32 with MBR and 32 KiB clusters,
passed on the connected ESP32-D0WD-V3 / OV3660 board with 4 MiB PSRAM.

| Check | Result |
| --- | --- |
| SD interface | One-bit SDMMC, 20 MHz; CLK GPIO14, CMD GPIO15, D0 GPIO2 |
| Reported card capacity | 64,088,965,120 bytes |
| Patterned data write | 8 MiB in 7.203 seconds |
| Data verification after unmount/remount | All 8 MiB matched byte for byte; 6.370 seconds |
| Camera and SD together | Ten 1600 × 1200 JPEGs saved at JPEG compression 12 |
| JPEG persistence | All ten file sizes and CRC32 checks matched after unmount/remount |
| Host image check | One saved JPEG exported over USB; matching CRC32, fully decoded with Pillow, visually inspected |
| Cleanup | All diagnostic files and their temporary directory removed; SD unmounted |
| Original firmware | Complete 4 MiB recovery image restored and its on-device digest verified |
| Restored camera | Original startup and HTTP status checked; 14 measured 2 MP stream frames fully decoded |

The data timings include pattern generation or comparison and filesystem work.
They are short diagnostic measurements, not sustained recorder throughput.
JPEGs in this indoor scene were approximately 47 kB; write, flush and close
took 53–76 ms per file.
The short restored-stream check included a 3.27-second delivery gap; it confirms
valid image delivery, not sustained network performance.

This preflight test establishes card access and camera/SD compatibility.
The subsequent [recorder setup](../camera_recorder/README.md) and
[recorder validation](../camera_recorder/VALIDATION.md) document video recording,
five-minute clips, BOOT controls, and the remaining verification limits.

The full-flash backup contains private configuration and remains ignored under
`.arduino-build/sd-card-test/20260915T020330Z/`. That folder also contains
`sd-test.log`, `result.json`, the exported `sample-from-sd.jpg`, and restoration
evidence. An existing recovery image was updated with a fresh read of the first
64 KiB, then accepted only after the board verified the complete 4 MiB image.
A faster full-flash read encountered serial noise; 115200-baud reads and
verification and 460800-baud writes succeeded.

## Run another test

Check the connected serial port first; this run used `/dev/cu.usbserial-110`.
Use a new backup filename on every run. The backup helper refuses to overwrite
an existing backup and checks it against the board.

```sh
python3 tools/webrtc.py backup \
  --output .arduino-build/sd-card-backups/before-test.bin \
  --port /dev/cu.usbserial-110

arduino-cli compile \
  --fqbn esp32:esp32:esp32wrover \
  --build-path .arduino-build/sd-card-test/build \
  --warnings all firmware/sd_card_test

arduino-cli upload \
  --fqbn esp32:esp32:esp32wrover:UploadSpeed=460800 \
  --port /dev/cu.usbserial-110 \
  --input-dir .arduino-build/sd-card-test/build \
  firmware/sd_card_test
```

At 115200 baud, send `RUN` followed by a newline. The sketch:

1. Mounts the existing filesystem without formatting.
2. Creates a unique `/esp32-sd-test-XXXXXXXX` directory.
3. Writes 8 MiB of block-dependent data, remounts, and checks every byte.
4. Starts the camera while SD is mounted and saves ten JPEGs.
5. Remounts SD and checks every image's size and CRC32.
6. Prints `RESULT PASS`, or a specific `RESULT FAIL` message.

For a binary JPEG export, send `EXPORT0` through `EXPORT9`. Read the byte count
from `JPEG_BEGIN <length>`, consume exactly that many binary bytes, then read
the `JPEG_END` marker.

Send `CLEANUP` and wait for `CLEANUP PASS`. This removes only files in the
directory created by this boot's diagnostic and unmounts SD. Close the serial
connection, then restore the original firmware even if a test failed:

```sh
python3 tools/webrtc.py restore \
  --backup .arduino-build/sd-card-backups/before-test.bin \
  --port /dev/cu.usbserial-110
```

The restore helper validates the backup's SHA-256, writes the complete flash
image, verifies it against the board, and resets into the restored application.
Confirm the original camera initializes and its HTTP stream works afterward.
