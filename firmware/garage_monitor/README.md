# Garage door monitor

The Freenove ESP32-WROVER captures a 640 × 480 JPEG, posts it to the
[AWS backend](../../infra/README.md), then deep-sleeps for five minutes before
repeating. The backend stores the image, classifies the door with Bedrock, and
sends an SNS alert for an open result.

Uploading this sketch replaces whichever firmware is installed on the board.
Run all commands below from the repository root. Check the current serial port
and use 460800 baud for uploads.

## Configure and flash

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

See the [backend README](../../infra/README.md) for infrastructure commands.
The Freenove camera pin mapping is defined in [garage_monitor.ino](garage_monitor.ino).

The original 4 MiB flash image for this checkout was also saved locally as
`.arduino-build/garage-monitor-backup.bin`, with a SHA-256 sidecar file.
Build output and this backup are ignored by git and can contain Wi-Fi credentials.
