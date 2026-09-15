#!/usr/bin/env python3
"""Loopback-only USB signaling bridge. No JPEG data is carried by this process."""
import argparse
from collections import deque
import json
from pathlib import Path
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

import serial

ROOT = Path(__file__).resolve().parents[1]


class Bridge:
    def __init__(self, port, log):
        self.serial = serial.Serial(port=None, baudrate=115200, timeout=.2)
        self.serial.dtr = False
        self.serial.rts = False
        self.serial.port = port
        self.serial.open()
        self.lock = threading.RLock()
        self.events = deque(maxlen=200)
        self.cursor = 0
        self.owner = ""
        self.last_seen = 0
        self.log = log
        self.stopping = threading.Event()
        threading.Thread(target=self.read, daemon=True).start()

    def read(self):
        with self.log.open("a", buffering=1) as output:
            while not self.stopping.is_set():
                try:
                    raw = self.serial.readline(20000)
                except serial.SerialException:
                    return
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace").strip()
                if not line.startswith("@signal "):
                    output.write(line + "\n")
                    continue
                try:
                    value = json.loads(line[8:])
                except json.JSONDecodeError:
                    continue
                # SDP contains short-lived ICE credentials. Forward it in memory,
                # but keep credentials out of the experiment's durable log.
                logged = dict(value)
                if "sdp" in logged:
                    logged["sdp"] = "[redacted]"
                output.write("@signal " + json.dumps(logged, separators=(",", ":")) + "\n")
                with self.lock:
                    self.cursor += 1
                    self.events.append((self.cursor, value))

    def write(self, value):
        payload = (json.dumps(value, separators=(",", ":")) + "\n").encode()
        if len(payload) > 16384:
            raise ValueError("Command is too large")
        self.serial.write(payload)
        self.serial.flush()

    def send(self, value):
        with self.lock:
            session = value.get("session", "")
            if not isinstance(session, str) or len(session) > 64:
                return 400
            if value.get("type") == "offer":
                if self.owner and self.owner != session:
                    return 409
                if not session:
                    return 400
                self.owner = session
                self.last_seen = time.monotonic()
            elif value.get("type") != "status" and session != self.owner:
                return 409
            self.write(value)
            if value.get("type") == "close":
                self.owner = ""
            return 200

    def snapshot(self, after, session):
        with self.lock:
            if session and session == self.owner:
                self.last_seen = time.monotonic()
            return {"cursor": self.cursor, "events": [
                value for cursor, value in self.events
                if cursor > after and (not session or not value.get("session") or value.get("session") == session)
            ]}

    def expire(self):
        with self.lock:
            if self.owner and time.monotonic() - self.last_seen > 30:
                self.write({"type": "close", "session": self.owner})
                self.owner = ""

    def close(self):
        with self.lock:
            if self.owner:
                self.write({"type": "close", "session": self.owner})
            self.stopping.set()
            self.serial.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/cu.usbserial-10")
    parser.add_argument("--http-port", type=int, default=8766)
    parser.add_argument("--log", type=Path, default=ROOT / ".arduino-build/webrtc-lab-serial.log")
    args = parser.parse_args()
    args.log.parent.mkdir(parents=True, exist_ok=True)
    bridge = Bridge(args.port, args.log)

    class Handler(BaseHTTPRequestHandler):
        def reply(self, status, value):
            body = json.dumps(value).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            parsed = urlsplit(self.path)
            if parsed.path != "/events":
                return self.reply(404, {"error": "Not found"})
            query = parse_qs(parsed.query)
            try:
                after = int(query.get("after", ["0"])[0])
            except ValueError:
                return self.reply(400, {"error": "Invalid cursor"})
            self.reply(200, bridge.snapshot(after, query.get("session", [""])[0]))

        def do_POST(self):
            if self.path != "/signal":
                return self.reply(404, {"error": "Not found"})
            origin = self.headers.get("Origin")
            if origin and origin not in {"http://127.0.0.1:5173", "http://localhost:5173"}:
                return self.reply(403, {"error": "Origin denied"})
            if self.headers.get("Content-Type") != "application/json":
                return self.reply(415, {"error": "JSON required"})
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if length <= 0 or length > 16384:
                    raise ValueError()
                value = json.loads(self.rfile.read(length))
                if not isinstance(value, dict):
                    raise ValueError()
                code = bridge.send(value)
            except (ValueError, serial.SerialException):
                return self.reply(400, {"error": "Invalid signaling command"})
            self.reply(code, {"ok": code == 200})

        def log_message(self, *_):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", args.http_port), Handler)
    server.timeout = .5
    print(f"USB signaling bridge ready on 127.0.0.1:{args.http_port}; log: {args.log}", flush=True)
    try:
        while True:
            server.handle_request()
            bridge.expire()
    except KeyboardInterrupt:
        pass
    finally:
        bridge.close()
        server.server_close()


if __name__ == "__main__":
    main()
