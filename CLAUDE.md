# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Garage door monitor: a Freenove ESP32-WROVER board with camera captures a JPEG every 5 minutes, POSTs it to an AWS Lambda function URL, which saves it to S3 and sends it to Amazon Bedrock (Claude) to determine if the garage door is open or closed. If open, an SNS email alert is sent.

## Architecture

```
ESP32-WROVER (deep sleep cycle, powerbank-powered)
    → POST JPEG to Lambda Function URL
        → Save image to S3 (YYYY-MM-DD/HH-MM-SS.jpg, 30-day lifecycle)
        → Bedrock Claude vision analysis
        → SNS email alert if door is open
        → Structured JSON log to CloudWatch
```

## Key Components

- **`firmware/garage_monitor/garage_monitor.ino`** — Arduino sketch for ESP32-WROVER. Handles camera init, WiFi, HTTP POST, deep sleep. Pin mapping is specific to the Freenove ESP32-WROVER board.
- **`infra/garage_stack.py`** — CDK stack: Lambda + Function URL, SNS topic, S3 bucket, Bedrock permissions.
- **`infra/lambda/handler.py`** — Lambda function: receives JPEG, stores in S3, calls Bedrock, publishes SNS if open.
- **`infra/app.py`** — CDK app entry point.

## Commands

### AWS Infrastructure (CDK)
```bash
uv run npx cdk synth                              # synthesize CloudFormation
uv run npx cdk deploy --context email=you@email.com  # deploy stack
uv run npx cdk diff                                # preview changes
```

### Firmware (Arduino CLI)
```bash
# Compile
arduino-cli compile --fqbn esp32:esp32:esp32wrover firmware/garage_monitor

# Flash (use 460800 baud — 921600 is unreliable with this board)
arduino-cli upload --fqbn esp32:esp32:esp32wrover:UploadSpeed=460800 --port /dev/cu.usbserial-10 firmware/garage_monitor

# Monitor serial output (board resets and goes to deep sleep quickly)
uv run python3 -c "
import serial, time
ser = serial.Serial('/dev/cu.usbserial-10', 115200, timeout=1)
ser.dtr = False; ser.rts = True; time.sleep(0.1); ser.rts = False
end = time.time() + 25
while time.time() < end:
    line = ser.readline()
    if line: print(line.decode('utf-8', errors='replace').strip())
ser.close()
"
```

### Board Communication
```bash
uv run esptool chip-id --port /dev/cu.usbserial-10   # verify board connection
```

## Hardware Details

- **Board**: Freenove ESP32-WROVER with OV2640 camera
- **Chip**: ESP32-D0WD-V3 (rev v3.1), dual core 240MHz, Wi-Fi + BT
- **Serial port**: `/dev/cu.usbserial-10`
- **Arduino FQBN**: `esp32:esp32:esp32wrover`
- **Flash usage**: ~86% program storage, ~18% dynamic memory

## Configuration

WiFi credentials and the Lambda URL belong in the ignored `firmware/garage_monitor/secrets.h`; copy `secrets.example.h` in that directory to get started. The streaming sketch uses its ignored `firmware/camera_stream/wifi_credentials.h`, with `wifi_credentials.example.h` as the template. Never put real credentials in tracked source or example files. Generated CDK output and firmware binaries are also ignored because they can contain local configuration and credentials. The SNS email is passed as CDK context (`--context email=...`). The Bedrock model ID is set in `garage_stack.py` as an environment variable.
