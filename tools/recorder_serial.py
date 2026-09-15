#!/usr/bin/env python3
"""Control the recorder over USB and download finalized AVI files with CRC checks."""

import argparse
import json
from pathlib import Path
import re
import serial
import termios
import time
import zlib


class RecorderClient:
    def __init__(self, port, logger=print):
        self.log = logger
        self.lines = []
        self.partial_line = bytearray()
        # On this board release RTS/EN before changing DTR/GPIO0. pyserial's
        # normal DTR-then-RTS open order can pulse reset on the Mac's CH340.
        self.serial = serial.Serial(port=None, baudrate=115200, timeout=0.1,
                                    dsrdtr=True)
        self.serial.dtr = False
        self.serial.rts = False
        self.serial.port = port
        self.serial.open()
        self.serial.dtr = False
        # macOS can otherwise drop modem-control lines on close and reset a
        # board that was just stopped safely. Preserve their explicit states.
        attributes = termios.tcgetattr(self.serial.fileno())
        attributes[2] &= ~termios.HUPCL
        termios.tcsetattr(self.serial.fileno(), termios.TCSANOW, attributes)

    def close(self):
        self.serial.dtr = False
        self.serial.rts = False
        self.serial.close()

    def read_line(self, timeout=1):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            raw = self.serial.read_until(b"\n", 4096)
            if not raw:
                continue
            self.partial_line.extend(raw)
            if len(self.partial_line) > 8192:
                raise ValueError("Overlong serial response")
            if raw.endswith(b"\n"):
                text = self.partial_line.decode("utf-8", errors="replace").strip()
                self.partial_line.clear()
                if text:
                    self.lines.append(text)
                    self.log(text)
                    return text
        return ""

    def send(self, command):
        if "\n" in command or "\r" in command or len(command) > 90:
            raise ValueError("Invalid serial command")
        self.serial.write(command.encode("ascii") + b"\n")
        self.serial.flush()

    def wait(self, prefix, timeout=20):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            line = self.read_line(min(1, deadline - time.monotonic()))
            if line.startswith(prefix):
                return line
            if line.startswith(("ERR ", "ERROR ", "VERIFY_FAILED")):
                raise RuntimeError(line)
        raise TimeoutError(f"No {prefix!r} response")

    def wait_status(self, state=None, timeout=20):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            text = self.wait("STATUS ", max(0.1, deadline - time.monotonic()))
            status = json.loads(text[7:])
            if state is None or status["state"] == state:
                return status
            if status["state"] == "error":
                raise RuntimeError(status["error"])
        raise TimeoutError(f"Recorder did not reach {state}")

    def status(self):
        self.send("STATUS")
        return self.wait_status()

    def stop(self):
        self.send("STOP")
        return self.wait_status("stopped")

    def start(self):
        self.send("START")
        return self.wait_status("recording")

    def reset(self):
        self.serial.rts = True
        time.sleep(0.1)
        self.serial.rts = False

    @staticmethod
    def check_name(name):
        if not re.fullmatch(r"REC[0-9]{8}\.avi", name):
            raise ValueError("Expected a finalized REC00000001.avi-style filename")

    def verify(self, name):
        self.check_name(name)
        self.send("VERIFY " + name)
        return json.loads(self.wait("VERIFIED ", 180)[9:])

    def get(self, name, output):
        self.check_name(name)
        output = Path(output)
        if output.exists():
            raise FileExistsError(output)
        output.parent.mkdir(parents=True, exist_ok=True)
        temporary = output.with_name(output.name + ".partial")
        if temporary.exists():
            raise FileExistsError(temporary)
        self.send("GET " + name)
        size = int(self.wait("FILE_BEGIN ")[11:])
        if not 224 <= size <= 512 * 1024 * 1024:
            raise ValueError("Unexpected download size")
        remaining, crc, since_progress = size, 0, 0
        deadline = time.monotonic() + size / 6000 + 30
        with temporary.open("xb") as destination:
            while remaining and time.monotonic() < deadline:
                chunk = self.serial.read(min(32768, remaining))
                if not chunk:
                    continue
                destination.write(chunk)
                crc = zlib.crc32(chunk, crc)
                remaining -= len(chunk)
                since_progress += len(chunk)
                if since_progress >= 262144:
                    self.log(f"DOWNLOAD {size - remaining}/{size} bytes")
                    since_progress = 0
        if remaining:
            raise TimeoutError(f"Download incomplete; {remaining} bytes missing")
        trailer = self.wait("FILE_END ")
        parts = trailer.split()
        if len(parts) != 3 or parts[2] != "OK" or int(parts[1], 16) != crc:
            raise ValueError("Download failed its CRC check")
        temporary.rename(output)
        return {"file": str(output), "bytes": size, "crc32": f"{crc:08x}"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("command", choices=("status", "stop", "start", "list",
                                            "verify", "get"))
    parser.add_argument("filename", nargs="?")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    client = RecorderClient(args.port)
    try:
        if args.command in ("status", "stop", "start"):
            result = getattr(client, args.command)()
        elif args.command == "list":
            client.send("LIST")
            client.wait("LIST_END")
            result = None
        elif args.command == "verify":
            result = client.verify(args.filename or "")
        else:
            if not args.output:
                parser.error("get requires --output")
            result = client.get(args.filename or "", args.output)
        if result:
            print(json.dumps(result, indent=2))
    finally:
        client.close()


if __name__ == "__main__":
    main()
