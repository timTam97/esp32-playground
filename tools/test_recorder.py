#!/usr/bin/env python3
"""Run portable AVI/button regression checks and independent video decoding."""

from pathlib import Path
import struct
import subprocess

from PIL import Image
from check_recorder_video import inspect

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / ".arduino-build/recorder/host-tests"


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    image = Image.new("RGB", (32, 24))
    image.putdata([
        (x * 7 % 256, y * 11 % 256, (x + y) * 9 % 256)
        for y in range(24) for x in range(32)
    ])
    jpeg = OUT / "fixture.jpg"
    image.save(jpeg, quality=90)
    data = jpeg.read_bytes()
    if len(data) % 2 == 0:
        # A valid odd-sized JPEG comment forces AVI's word-padding path.
        data = data[:2] + b"\xff\xfe\x00\x03X" + data[2:]
    jpeg.write_bytes(data)
    executable = OUT / "avi_writer_test"
    subprocess.run([
        "clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined",
        str(ROOT / "tests/recorder/avi_writer_test.cpp"), "-o", str(executable),
    ], check=True)
    movie = OUT / "fixture.avi"
    subprocess.run([str(executable), str(jpeg), str(movie)], check=True)
    result = inspect(movie)
    assert result["frames"] == 7 and abs(result["duration_s"] - 0.9) < 0.001
    original = movie.read_bytes()
    index = 220 + struct.unpack_from("<I", original, 216)[0]
    corrupt = bytearray(original)
    struct.pack_into("<I", corrupt, index + 16, 5)  # First chunk actually starts at 4.
    for name, contents in (("bad-index.avi", corrupt),
                           ("truncated.avi", original[:-3])):
        path = OUT / name
        path.write_bytes(contents)
        try:
            inspect(path)
        except ValueError:
            pass
        else:
            raise AssertionError(f"Accepted invalid video: {name}")
    print("PASS: AVI padding/timing/index, bounded clips, short writes, sync failures,")
    print("button debounce/hold/release/wrap, full JPEG/FFmpeg decode, corrupt-file rejection.")


if __name__ == "__main__":
    main()
