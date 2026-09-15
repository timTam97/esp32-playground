# Recorder validation — 15 September 2026

The standalone recorder was built, flashed and left installed on the user's
Freenove ESP32-WROVER / ESP32-D0WD-V3 board with OV3660 camera, 4 MiB PSRAM,
and the FAT32 64 GB microSD card. The final state is **stopped with SD unmounted**.
Connecting power again automatically starts recording.

## Final hardware results

| Check | Observed result |
| --- | --- |
| Capture configuration | 1600 × 1200 JPEG, quality 12, 20 MHz XCLK, two PSRAM frame buffers |
| SD configuration | One-bit SDMMC at 20 MHz; internal DMA-capable 32 KiB file buffer |
| Automatic five-minute clip | `REC00000004.avi`: 2,996 frames, 300.000 seconds, **9.99 fps** |
| Five-minute file size | 131,381,794 bytes |
| Capture errors during the gate | No rejected frames or SD write errors reported |
| Automatic rollover | `REC00000005.avi` opened without intervention |
| Following clip | 52 frames, 5.153 seconds; cleanly finalized on stop |
| Saved-file verification | Both clips' RIFF sizes, frame counts, complete indexes and JPEG boundary markers checked after remount |
| Short clip exported to Mac | 11 frames, 1.088 seconds, 477,112 bytes; transport CRC32 matched |
| Short-clip image verification | Every frame fully decoded using Pillow and FFmpeg; AVI index and playback duration independently checked |
| Native playback | The short AVI opened and played in QuickTime Player; its image was visually inspected |
| Physical BOOT button | User held the actual button; firmware observed `BOOT_HOLD`, finalized 160 frames, and unmounted SD |
| Indicator | User observation identified the onboard LED as active-low; final firmware emits two lit pulses with a dark pause |
| Restart | Serial start/stop worked repeatedly; a clean hardware reset automatically opened `REC00000006.avi` |
| Existing-file preservation | The earlier clips and their expected sizes remained present after reset |
| Final stop | New clip finalized; `safe_to_unplug: true` |

The long clips were checked on the ESP32 for container/index consistency and
complete JPEG boundaries. They were not downloaded and fully decoded on the Mac.
The short exported clip received the full decoder and playback checks.

Six setup-test clips remain in `/recordings/`. The next automatic recording
will use the next available number. These clips include the ceiling scene seen
during setup.

## Build and host checks

Arduino ESP32 **3.3.11**, shared FQBN `esp32:esp32:esp32wrover`, 460800-baud
upload. The final build completed with `--warnings all`, using 459,067 bytes
of program storage (35%) and 35,424 bytes of static memory (10%).
Upload verification succeeded.

Application SHA-256:

```text
e9b031ccce954e402cea326cfb11c7a6fd38d1e26f1757a286973015da30abf6
```

Run the portable checks with Python, Pillow, clang++, ffprobe and ffmpeg:

```sh
python3 tools/test_recorder.py
python3 tools/check_recorder_video.py /path/to/recording.avi
```

The native writer/button checks use AddressSanitizer and UndefinedBehaviorSanitizer.
They cover odd-length JPEG padding, timing and index construction, frame/file
capacity limits, short header/payload/index writes, sync failures, invalid JPEGs,
button bounce, a held button firing only once, release, and clock wrap.
Independent parsing and decoding checks also reject a corrupted index and a
truncated file.

## Findings resolved during setup

- Early SD write errors occurred while manipulating USB DTR to imitate BOOT.
  Serial-command start/stop and the actual physical button were tested separately.
  The DTR-based button command was removed from the helper.
- The final file buffer uses internal DMA-capable memory; frame buffers and
  the AVI index use PSRAM.
- The Mac's CH340 control-line order could reset the board when opening a port.
  The helper now releases RTS before DTR and disables hang-up-on-close.
  Reconnecting to the stopped recorder preserved its state in the subsequent tests.
- The initial LED polarity produced dark pulses followed by a lit pause.
  The final firmware reverses that output to give the documented double flash.
- Espressif's camera driver logged that the shared GPIO interrupt service was
  already installed during camera reinitialization. Recording, stop/restart,
  and the full gate succeeded despite that library diagnostic.

Battery runtime, a physical power-bank session, card exhaustion, removal while
recording, and recovery after abrupt power loss have not been hardware tested.
The firmware preserves unfinished `.part` files but does not automatically
repair them. The real camera scene and card determine achievable frame rate.

## Evidence and recovery

Ignored local evidence is under:

```text
.arduino-build/recorder/20260915T022608Z/
```

It includes `installed-manifest.json`, `rollover-test.json`, `rollover-test.log`,
`physical-button-test.json`, `final-short-test.json`,
`final-short-video-validation.json`, and `final-short-clip.avi`.
The manifest includes source and application hashes.

The complete previous streaming firmware was verified against the board before
installation and saved as `before-recorder.bin` with a SHA-256 sidecar.
Its checksum is:

```text
ffe281ed1d02caddae38170c85453ceb28bc3ea1003e05cd09e540fdaa5b2511
```

See the [operating instructions](README.md) for the restore command.
