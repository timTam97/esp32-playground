# ESP32 camera projects

Camera firmware and supporting services for the Freenove ESP32-WROVER.

| Project | Description |
| --- | --- |
| [SD recorder](firmware/camera_recorder/README.md) | Portable video recording with automatic start and BOOT stop/restart. |
| [Wi-Fi camera](firmware/camera_stream/README.md) | Local streaming webpage and performance tools. |
| [Garage monitor](firmware/garage_monitor/README.md) | Periodic JPEG capture and upload to AWS. |
| [AWS backend](infra/README.md) | CDK infrastructure, image classification and email alerts. |
| [WebRTC experiment](firmware/camera_webrtc/README.md) | Experimental encrypted JPEG streaming; setup and measured results. |
| [Browser harness](web/README.md) | WebRTC viewer and local testing interface. |
| [SD-card diagnostic](firmware/sd_card_test/README.md) | On-board storage and camera compatibility checks. |

Shared [tools](tools/), [tests](tests/), and [research notes](docs/).
