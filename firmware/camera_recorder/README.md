# Portable SD video recorder

The Freenove ESP32-WROVER records automatically when USB power is connected.
It saves **1600 × 1200 MJPEG video in AVI files**, targeting **10 fps**, with
no audio. Wi-Fi is not started, and no computer or network is needed to record.

## Use the camera

1. Insert the FAT32 microSD card while power is disconnected.
2. Connect USB power. Recording starts after camera and SD initialization.
3. To stop, **hold BOOT for about one second, then release it**.
4. Wait for the **IO2 LED to flash twice, pause in darkness, and repeat**.
   The clip is then finalized, closed, and the card unmounted. Power can be
   disconnected or the card removed.
5. To record again while powered, hold and release BOOT again.

Use the BOOT button, not EN/RST. Leave BOOT released when connecting power;
holding it during startup can select the programming mode.

The IO2 LED shares the card's D0 pin. During recording its activity belongs to
the SD interface. The deliberate double flash is used only after SD has been
unmounted. Continuous fast blinking indicates an error; the serial console
provides the reason.

## Files and limits

Videos are saved as `/recordings/REC00000001.avi`, `REC00000002.avi`, and so on.
The next unused number is found on startup, including any unfinished files.
Existing recordings are never automatically deleted or overwritten.

Each clip lasts approximately five minutes. A 512 MiB safety limit can close
a clip earlier if frames are unusually large. If space is nearly exhausted,
recording stops and closes the current clip, keeping approximately 32 MiB in
reserve for metadata and finalization.

An in-progress file has the extension `.part`. It becomes `.avi` only after
the video index and final header have been written and synchronized. Sudden
power loss can leave the current `.part` unfinished or damage the filesystem;
automatic recovery is not implemented. Use the BOOT stop procedure before
disconnecting power.

The frame rate is a target. Card speed, scene detail and exposure affect capture
timing. The AVI header uses the measured clip duration, so a slower capture run
does not play back artificially fast. Frames have a uniform playback interval
within each AVI; this is not timestamped variable-frame-rate recording.

The saved sample was opened and played in QuickTime Player on the test Mac.
See [hardware validation](VALIDATION.md) for measured results and limits.

## Build and install

Use Arduino ESP32 core **3.3.11** and the Wrover board definition. Verify the
current USB serial port; this setup used `/dev/cu.usbserial-110`.

```sh
arduino-cli compile \
  --fqbn esp32:esp32:esp32wrover \
  --build-path .arduino-build/recorder/build \
  --warnings all firmware/camera_recorder

arduino-cli upload \
  --fqbn esp32:esp32:esp32wrover:UploadSpeed=460800 \
  --port /dev/cu.usbserial-110 \
  --input-dir .arduino-build/recorder/build \
  firmware/camera_recorder
```

The camera uses 20 MHz XCLK, JPEG quality 12, and two PSRAM frame buffers.
SD uses one-bit SDMMC at 20 MHz: CLK GPIO14, CMD GPIO15, D0 GPIO2.
The AVI index is in PSRAM; the 32 KiB stdio transfer buffer is in DMA-capable
internal memory. The BOOT input is debounced and generates one event per hold,
requiring release before another event.

`RECORDER_CLIP_SECONDS` defaults to 300 and can be overridden at compile time
for a short diagnostic build. The installed production build uses 300 seconds.

## USB diagnostics

The serial interface is optional and runs at 115200 baud. Use this helper to
manage control-line order on the Mac's CH340 adapter. It requires pyserial,
which is available in the project's `.venv`.

```sh
.venv/bin/python tools/recorder_serial.py --port /dev/cu.usbserial-110 status
.venv/bin/python tools/recorder_serial.py --port /dev/cu.usbserial-110 stop
.venv/bin/python tools/recorder_serial.py --port /dev/cu.usbserial-110 list
.venv/bin/python tools/recorder_serial.py --port /dev/cu.usbserial-110 verify REC00000001.avi
.venv/bin/python tools/recorder_serial.py --port /dev/cu.usbserial-110 get REC00000001.avi --output /tmp/recording.avi
.venv/bin/python tools/recorder_serial.py --port /dev/cu.usbserial-110 start
```

Stop before listing, verifying or downloading files. These operations temporarily
remount the card and unmount it when finished. Do not remove the card during a
USB operation. Downloads are CRC-checked and do not overwrite local files.
Large clips transfer slowly at 115200 baud; a card reader is preferable.

Do not imitate BOOT by asserting USB DTR: this caused SD errors during hardware
testing. The physical BOOT button was tested directly. The helper releases
RTS before DTR when opening the port and disables hang-up-on-close, avoiding
unwanted resets from normal diagnostic connections.

## Restore the previous streaming camera

A complete recovery image was verified against the board before installation.
It contains private configuration and remains ignored by git:

```sh
python3 tools/webrtc.py restore \
  --backup .arduino-build/recorder/20260915T022608Z/before-recorder.bin \
  --port /dev/cu.usbserial-110
```

The restore helper validates the backup checksum, writes the full 4 MiB flash
image, verifies it on the board, and resets into the restored application.
