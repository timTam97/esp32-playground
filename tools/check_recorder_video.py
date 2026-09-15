#!/usr/bin/env python3
"""Validate a recorder AVI's RIFF layout, every JPEG, index, and playback timing."""

import argparse
from io import BytesIO
import json
from pathlib import Path
import struct
import subprocess

from PIL import Image


def inspect(path):
    data = Path(path).read_bytes()
    if len(data) < 232 or data[:4] != b"RIFF" or data[8:12] != b"AVI ":
        raise ValueError("Not a complete AVI")

    def number(offset):
        return struct.unpack_from("<I", data, offset)[0]

    if number(4) + 8 != len(data):
        raise ValueError("RIFF size does not match the file")
    if data[20:24] != b"hdrl" or data[96:100] != b"strl":
        raise ValueError("AVI stream headers are missing")
    if data[108:116] != b"vidsMJPG" or data[220:224] != b"movi":
        raise ValueError("Not the expected MJPEG stream")
    frames = number(48)
    if not frames or frames != number(140) or not number(44) & 0x10:
        raise ValueError("Missing frame counts or index flag")
    width, height, scale, rate = number(64), number(68), number(128), number(132)
    if not scale or not rate:
        raise ValueError("Invalid playback time base")
    index_position = 220 + number(216)
    if data[index_position:index_position + 4] != b"idx1":
        raise ValueError("Index is not immediately after the movie data")
    if number(index_position + 4) != frames * 16:
        raise ValueError("Index entry count differs from frame count")
    if index_position + 8 + frames * 16 != len(data):
        raise ValueError("Index length does not match the file")
    position = 224
    for frame in range(frames):
        chunk_id, flags, offset, size = struct.unpack_from(
            "<4sIII", data, index_position + 8 + frame * 16
        )
        if chunk_id != b"00dc" or not flags & 0x10 or 220 + offset != position:
            raise ValueError(f"Invalid index entry {frame}")
        if data[position:position + 4] != b"00dc" or number(position + 4) != size:
            raise ValueError(f"Index does not locate frame {frame}")
        end = position + 8 + size
        if end > index_position:
            raise ValueError(f"Frame {frame} extends outside movie data")
        jpeg = data[position + 8:end]
        if not jpeg.startswith(b"\xff\xd8") or not jpeg.endswith(b"\xff\xd9"):
            raise ValueError(f"Incomplete JPEG {frame}")
        with Image.open(BytesIO(jpeg)) as image:
            image.load()
            if image.size != (width, height):
                raise ValueError(f"Unexpected dimensions for frame {frame}")
        if size % 2 and data[end] != 0:
            raise ValueError(f"Invalid RIFF padding after frame {frame}")
        position = end + size % 2
    if position != index_position:
        raise ValueError("Unindexed movie data")
    probe = json.loads(subprocess.check_output([
        "ffprobe", "-v", "error", "-count_frames", "-show_streams",
        "-show_format", "-of", "json", str(path),
    ], text=True))
    video = probe["streams"][0]
    duration = frames * scale / rate
    if (video["codec_name"] != "mjpeg" or int(video["nb_read_frames"]) != frames or
            video["width"] != width or video["height"] != height or
            abs(float(video["duration"]) - duration) > 0.002):
        raise ValueError("Independent FFmpeg interpretation differs from the AVI")
    subprocess.run(["ffmpeg", "-v", "error", "-xerror", "-i", str(path),
                    "-f", "null", "-"], check=True, capture_output=True)
    return {
        "file": str(path), "frames": frames, "width": width, "height": height,
        "duration_s": duration, "fps": rate / scale, "bytes": len(data),
        "all_jpegs_decoded": True, "index_checked": True, "ffmpeg_decoded": True,
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("file", type=Path)
    parser.add_argument("--json-output", type=Path)
    args = parser.parse_args()
    result = inspect(args.file)
    text = json.dumps(result, indent=2) + "\n"
    print(text, end="")
    if args.json_output:
        args.json_output.write_text(text)
