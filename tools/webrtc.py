#!/usr/bin/env python3
"""Reproducible native build, full-flash backup, flash, and recovery."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SDK = ROOT / ".arduino-build/webrtc-tools/esp-idf"
TOOLCHAIN = ROOT / ".arduino-build/webrtc-tools/toolchain"
BUILD = ROOT / ".arduino-build/camera-webrtc"
PROJECT = ROOT / "firmware/camera_webrtc"
PYTHON = ROOT / ".venv/bin/python"
IDF_REVISION = "b774170ff46c393eeb5e495ea37936038d3f4f4f"


def run(arguments, **kwargs):
    return subprocess.run([str(arg) for arg in arguments], check=True, **kwargs)


def checksum(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_backup(path):
    path = path.resolve()
    if path.stat().st_size != 4 * 1024 * 1024:
        raise ValueError("Recovery requires a complete 4 MiB flash backup")
    recorded = path.with_suffix(".sha256").read_text().split()[0]
    if checksum(path) != recorded:
        raise ValueError("Backup SHA-256 does not match its sidecar")
    return path


def idf(*arguments):
    revision = subprocess.check_output(["git", "-C", str(SDK), "rev-parse", "HEAD"], text=True).strip()
    if revision != IDF_REVISION:
        raise ValueError("Expected the pinned ESP-IDF v5.5.5 revision")
    env = dict(os.environ, IDF_PATH=str(SDK), IDF_TOOLS_PATH=str(TOOLCHAIN), IDF_TARGET="esp32")
    # All values are passed as argv/environment; none is interpolated as shell code.
    run([
        "/bin/bash", "-c", 'source "$IDF_PATH/export.sh" >/dev/null && exec "$@"',
        "webrtc-build", "idf.py", "-C", PROJECT, "-B", BUILD, *arguments,
    ], env=env, cwd=ROOT)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    sub.add_parser("setup")
    sub.add_parser("build")
    backup = sub.add_parser("backup")
    backup.add_argument("--output", type=Path, required=True)
    backup.add_argument("--port", default="/dev/cu.usbserial-10")
    for name in ("flash", "restore"):
        operation = sub.add_parser(name)
        operation.add_argument("--backup", type=Path, required=True)
        operation.add_argument("--port", default="/dev/cu.usbserial-10")
    args = parser.parse_args()
    if args.action == "setup":
        SDK.parent.mkdir(parents=True, exist_ok=True)
        if not SDK.exists():
            run(["git", "clone", "--depth", "1", "--branch", "v5.5.5",
                 "--recursive", "--shallow-submodules",
                 "https://github.com/espressif/esp-idf.git", SDK])
        revision = subprocess.check_output(["git", "-C", str(SDK), "rev-parse", "HEAD"], text=True).strip()
        if revision != IDF_REVISION:
            raise ValueError("Existing SDK is not the pinned revision; it was left unchanged")
        run([SDK / "install.sh", "esp32"], env=dict(os.environ, IDF_TOOLS_PATH=str(TOOLCHAIN)), cwd=ROOT)
        interpreters = list((TOOLCHAIN / "python_env").glob("idf5.5_py*_env/bin/python"))
        if len(interpreters) != 1:
            raise ValueError("Expected exactly one isolated IDF Python environment")
        run([interpreters[0], "-m", "pip", "install", "ninja==1.13.0"])
    elif args.action == "build":
        defaults_hash = checksum(PROJECT / "sdkconfig.defaults")
        prior_manifest = BUILD / "manifest.json"
        previous = json.loads(prior_manifest.read_text()) if prior_manifest.exists() else {}
        if previous.get("defaults_sha256") != defaults_hash:
            # This project treats the checked-in defaults as authoritative.
            # Existing generated choices otherwise override changed defaults.
            (PROJECT / "sdkconfig").unlink(missing_ok=True)
        idf("build")
        binaries = {p.name: checksum(p) for p in BUILD.glob("*.bin")}
        manifest = {
            "built_at": datetime.now(timezone.utc).isoformat(),
            "idf": "5.5.5", "idf_revision": IDF_REVISION,
            "esp_peer": "1.5.5", "esp32_camera": "2.1.7",
            "defaults_sha256": defaults_hash,
            "sdkconfig_sha256": checksum(PROJECT / "sdkconfig"),
            "binaries": binaries,
        }
        (BUILD / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        print(json.dumps(manifest, indent=2))
    elif args.action == "backup":
        if args.output.exists():
            raise ValueError("Choose a new backup filename; existing backups are never overwritten")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        run([PYTHON, "-m", "esptool", "--port", args.port, "--baud", "115200", "--after", "no-reset",
             "read-flash", "0", "0x400000", args.output])
        args.output.chmod(0o600)
        if args.output.stat().st_size != 4 * 1024 * 1024:
            raise ValueError("Incomplete backup")
        sidecar = args.output.with_suffix(".sha256")
        sidecar.write_text(f"{checksum(args.output)}  {args.output.name}\n")
        sidecar.chmod(0o600)
        # Verify before the application boots and writes Wi-Fi state to NVS.
        run([PYTHON, "-m", "esptool", "--port", args.port, "--baud", "115200",
             "verify-flash", "0", args.output])
    else:
        path = verify_backup(args.backup)
        if args.action == "flash":
            idf("-p", args.port, "-b", "460800", "flash")
        else:
            run([PYTHON, "-m", "esptool", "--port", args.port, "--baud", "460800", "--after", "no-reset",
                 "write-flash", "0", path])
            # Reset directly into the ROM loader to restore its 115200-baud
            # synchronization rate, without booting the app between operations.
            run([PYTHON, "-m", "esptool", "--port", args.port, "--baud", "115200",
                 "verify-flash", "0", path])


if __name__ == "__main__":
    main()
