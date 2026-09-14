#!/usr/bin/env python3
"""Measure complete MJPEG frames (stdlib; optional Pillow decoding)."""

import argparse
import io
import json
from pathlib import Path
import secrets
import statistics
import struct
import time
from urllib.error import HTTPError
from urllib.parse import urlencode, urlsplit
from urllib.request import ProxyHandler, Request, build_opener


# A local camera should not be sent through a system-configured HTTP proxy.
HTTP = build_opener(ProxyHandler({}))
SIZES = {
    "UXGA": (1600, 1200), "SXGA": (1280, 1024), "XGA": (1024, 768),
    "SVGA": (800, 600), "VGA": (640, 480), "QVGA": (320, 240),
}


def api(base, path, data=None):
    request = Request(base + path, data=None if data is None else urlencode(data).encode())
    try:
        with HTTP.open(request, timeout=12) as response:
            return json.load(response)
    except HTTPError as error:
        raise RuntimeError(f"HTTP {error.code}: {error.read().decode()}") from error


def jpeg_dimensions(jpeg):
    if not jpeg.startswith(b"\xff\xd8") or not jpeg.endswith(b"\xff\xd9"):
        raise ValueError("Incomplete JPEG")
    offset = 2
    while offset < len(jpeg) - 3:
        if jpeg[offset] != 0xFF:
            raise ValueError("Invalid JPEG marker")
        while jpeg[offset] == 0xFF:
            offset += 1
        marker = jpeg[offset]
        offset += 1
        if marker == 0xDA:
            break
        length = struct.unpack_from(">H", jpeg, offset)[0]
        if length < 2 or offset + length > len(jpeg):
            raise ValueError("Invalid JPEG segment length")
        if marker in (0xC0, 0xC1, 0xC2):
            height, width = struct.unpack_from(">HH", jpeg, offset + 3)
            return width, height
        offset += length
    raise ValueError("JPEG has no supported size marker")


def frames(response):
    boundary = response.headers.get_param("boundary")
    if not boundary:
        raise ValueError("Response is not a multipart stream")
    delimiter = b"--" + boundary.encode()
    while True:
        line = response.readline(4096)
        if not line:
            raise EOFError("Camera closed the stream")
        if line.strip() == delimiter + b"--":
            return
        if line.strip() != delimiter:
            if line.strip():
                raise ValueError("Unexpected multipart boundary")
            continue
        headers = {}
        for _ in range(20):
            line = response.readline(4096)
            if line in (b"\r\n", b"\n"):
                break
            name, value = line.decode("ascii").split(":", 1)
            headers[name.lower()] = value.strip()
        else:
            raise ValueError("Too many frame headers")
        length = int(headers["content-length"])
        if headers.get("content-type") != "image/jpeg" or not 0 < length < 2_000_000:
            raise ValueError("Invalid JPEG frame headers")
        jpeg = response.read(length)
        if len(jpeg) != length:
            raise EOFError("Camera closed a partial frame")
        yield jpeg, headers.get("x-timestamp")


def wait_stopped(base):
    deadline = time.monotonic() + 12
    while time.monotonic() < deadline:
        if not api(base, "/status")["streaming"]:
            return
        time.sleep(0.2)
    raise TimeoutError("Stream did not release the camera")


def benchmark(args):
    base = args.url.rstrip("/")
    parsed = urlsplit(base)
    if parsed.scheme != "http" or not parsed.hostname or parsed.path:
        raise ValueError("Use the camera's HTTP base URL, such as http://garage-camera.local")
    original = api(base, "/status")
    if original["streaming"]:
        raise RuntimeError("Pause the browser stream before benchmarking.")
    viewer = secrets.token_hex(8)
    image_decoder = None
    if getattr(args, "decode", False):
        try:
            from PIL import Image
        except ImportError as error:
            raise RuntimeError("--decode requires Pillow (python3 -m pip install Pillow).") from error
        image_decoder = Image
    count = total_bytes = 0
    timestamps = set()
    intervals = []
    capture_intervals = []
    sample_start = last_frame = None
    last_capture_us = None
    first_frame = None
    last_status = None
    errors = []
    try:
        api(base, "/settings", {"resolution": args.resolution, "quality": args.quality})
        with HTTP.open(base + "/stream?" + urlencode({"viewer": viewer}), timeout=12) as response:
            warmup_until = time.monotonic() + args.warmup
            for jpeg, timestamp in frames(response):
                now = time.monotonic()
                if jpeg_dimensions(jpeg) != SIZES[args.resolution]:
                    raise ValueError("Camera sent a JPEG at the wrong resolution")
                if image_decoder:
                    with image_decoder.open(io.BytesIO(jpeg)) as image:
                        image.load()
                        if image.size != SIZES[args.resolution]:
                            raise ValueError("Decoded JPEG has the wrong resolution")
                if not timestamp:
                    raise ValueError("Missing capture timestamp")
                seconds, micros = timestamp.split(".")
                capture_us = int(seconds) * 1_000_000 + int(micros)
                if now < warmup_until:
                    continue
                if sample_start is None:
                    # Use this complete frame as the time origin; exclude its bytes.
                    sample_start = last_frame = now
                    first_frame = jpeg
                    last_capture_us = capture_us
                    continue
                if capture_us <= last_capture_us:
                    raise ValueError("Repeated or out-of-order capture timestamp")
                count += 1
                total_bytes += len(jpeg)
                timestamps.add(timestamp)
                intervals.append(now - last_frame)
                capture_intervals.append(capture_us - last_capture_us)
                last_frame = now
                last_capture_us = capture_us
                if now - sample_start >= args.seconds:
                    # Verify the control server still responds during streaming.
                    last_status = api(base, "/status")
                    break
            else:
                raise EOFError("Camera ended before the benchmark completed")
        elapsed = last_frame - sample_start
        ordered = sorted(intervals)
        result = {
            "resolution": args.resolution,
            "width": SIZES[args.resolution][0],
            "height": SIZES[args.resolution][1],
            "quality": args.quality,
            "duration_s": round(elapsed, 3),
            "received_frames": count,
            "unique_capture_timestamps": len(timestamps),
            "received_fps": round(count / elapsed, 2),
            "received_mbps": round(total_bytes * 8 / elapsed / 1_000_000, 2),
            "mean_jpeg_kib": round(total_bytes / count / 1024, 1),
            "median_frame_interval_ms": round(statistics.median(intervals) * 1000, 1),
            "p95_frame_interval_ms": round(ordered[min(len(ordered) - 1, int(len(ordered) * .95))] * 1000, 1),
            "max_frame_interval_ms": round(max(intervals) * 1000, 1),
            "median_capture_interval_ms": round(statistics.median(capture_intervals) / 1000, 1),
            "min_capture_interval_ms": round(min(capture_intervals) / 1000, 1),
            "decoded_frames": count if image_decoder else None,
            "server_status": last_status,
        }
        if args.save_frame:
            args.save_frame.write_bytes(first_frame)
        return result
    finally:
        # Release only our viewer, then restore the settings present at startup.
        for action in [
            lambda: api(base, "/stop?" + urlencode({"viewer": viewer}), {}),
            lambda: wait_stopped(base),
            lambda: api(base, "/settings", {key: original[key] for key in ("resolution", "quality")}),
        ]:
            try:
                action()
            except Exception as error:
                errors.append(str(error))
        if errors:
            raise RuntimeError("Could not cleanly restore camera: " + "; ".join(errors))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("url")
    parser.add_argument("--resolution", choices=SIZES, default="UXGA")
    parser.add_argument("--quality", type=int, default=12, help="JPEG compression, 10–40")
    parser.add_argument("--seconds", type=float, default=20)
    parser.add_argument("--warmup", type=float, default=3)
    parser.add_argument("--save-frame", type=Path)
    parser.add_argument("--decode", action="store_true", help="Decode every JPEG with Pillow, in addition to checking its markers")
    parser.add_argument("--json-output", type=Path)
    args = parser.parse_args()
    if not 10 <= args.quality <= 40 or args.seconds <= 0 or args.warmup < 0:
        parser.error("Use compression 10–40, positive duration, and nonnegative warmup.")
    try:
        output = json.dumps(benchmark(args), indent=2)
    except (OSError, RuntimeError, ValueError, EOFError) as error:
        parser.exit(1, f"Benchmark failed: {error}\n")
    if args.json_output:
        args.json_output.write_text(output + "\n")
    print(output)


if __name__ == "__main__":
    main()
