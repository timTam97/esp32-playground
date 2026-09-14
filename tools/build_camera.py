#!/usr/bin/env python3
"""Build the streaming camera with a larger TCP send buffer; optionally upload."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tarfile
from urllib.request import urlopen


ROOT = Path(__file__).resolve().parents[1]
SKETCH = ROOT / "firmware/camera_stream"
BUILD = ROOT / ".arduino-build/camera_stream"
FQBN = "esp32:esp32:esp32wrover"
# ESP-IDF v5.5.5, as shipped in Arduino ESP32 3.3.11.
LWIP_COMMIT = "fd432e4ee2cfb7f7f1c7eb7227e0173412e7b84e"
LWIP_ARCHIVE = f"https://codeload.github.com/espressif/esp-lwip/tar.gz/{LWIP_COMMIT}"
TCP_SEND_BUFFER = 32768
# These are the compiled lwIP sources that use TCP_SND_BUF or its derived
# queue/low-water limits. The SDK does not build lwIP's separate HTTP app.
SOURCES = ("core/tcp.c", "core/tcp_out.c", "api/api_msg.c", "core/init.c")


def run(arguments, **kwargs):
    return subprocess.run(arguments, check=True, text=True, **kwargs)


def properties():
    result = run(
        [
            "arduino-cli", "compile", "--fqbn", FQBN,
            "--build-path", str(ROOT / ".arduino-build/properties"),
            "--show-properties=expanded", str(SKETCH),
        ],
        capture_output=True,
    )
    return dict(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line)


def source_tree():
    cache = ROOT / ".arduino-build"
    source = cache / f"esp-lwip-{LWIP_COMMIT}"
    if source.is_dir():
        return source
    cache.mkdir(exist_ok=True)
    archive_path = cache / "esp-lwip.tar.gz"
    print("Downloading the matching Espressif lwIP sources...", flush=True)
    with urlopen(LWIP_ARCHIVE, timeout=30) as response:
        with archive_path.open("wb") as archive:
            shutil.copyfileobj(response, archive)
    with tarfile.open(archive_path) as archive:
        # Python 3.12+ rejects paths and links escaping the destination.
        archive.extractall(cache, filter="data")
    if not source.is_dir():
        raise RuntimeError("The lwIP download did not contain the expected source revision.")
    return source


def check_sdk(props, sdk, source):
    core_version = Path(props["runtime.platform.path"]).name
    if core_version != "3.3.11":
        raise RuntimeError(
            f"This profile is verified with Arduino ESP32 3.3.11; found {core_version}. "
            "Use --stock-network for an ordinary Arduino build."
        )
    installed = sdk / "include/lwip/lwip/src/include"
    checked = 0
    for header in (source / "src/include").rglob("*.h"):
        sdk_header = installed / header.relative_to(source / "src/include")
        if sdk_header.is_file():
            if sdk_header.read_bytes() != header.read_bytes():
                raise RuntimeError(f"SDK/source mismatch: {sdk_header}. No SDK files were changed.")
            checked += 1
    if checked < 150:
        raise RuntimeError("Could not verify the installed lwIP headers against the pinned source.")
    return checked


def network_library(props):
    sdk = Path(props["compiler.sdk.path"])
    source = source_tree()
    checked = check_sdk(props, sdk, source)
    output = ROOT / ".arduino-build" / f"tcp-window-{TCP_SEND_BUFFER}"
    output.mkdir(exist_ok=True)
    config_path = sdk / props["build.memory_type"] / "include/sdkconfig.h"
    original_config = config_path.read_text()
    config, replacements = re.subn(
        r"(?m)^#define CONFIG_LWIP_TCP_SND_BUF_DEFAULT \d+$",
        f"#define CONFIG_LWIP_TCP_SND_BUF_DEFAULT {TCP_SEND_BUFFER}",
        original_config,
    )
    if replacements != 1:
        raise RuntimeError("Could not locate the SDK TCP send-buffer setting.")
    # Only this local header and archive change. All other SDK options stay as
    # installed, including TCP receive windows and the public structure layouts.
    (output / "sdkconfig.h").write_text(config)
    compiler = str(Path(props["compiler.path"]) / props["compiler.c.cmd"])
    archiver = str(Path(props["compiler.path"]) / props["compiler.ar.cmd"])
    flags = [
        "-I" + str(output),
        *shlex.split(props["compiler.c.flags"]),
        *shlex.split(props["compiler.cpreprocessor.flags"]),
    ]
    # Verify include precedence before compiling the replacement objects.
    preprocess_flags = [flag for flag in flags if flag not in ("-MMD", "-c")]
    defines = run(
        [compiler, *preprocess_flags, "-E", "-dM", "-x", "c", "-"],
        input='#include "lwip/opt.h"\n', capture_output=True,
    ).stdout
    if f"#define CONFIG_LWIP_TCP_SND_BUF_DEFAULT {TCP_SEND_BUFFER}\n" not in defines:
        raise RuntimeError("The compiler did not select the local TCP configuration.")
    print(f"Verified {checked} SDK headers; building a {TCP_SEND_BUFFER // 1024} KiB TCP send buffer.",
          flush=True)
    objects = []
    for name in SOURCES:
        obj = output / (Path(name).name + ".obj")
        run([compiler, *flags, str(source / "src" / name), "-o", str(obj)])
        objects.append(str(obj))

    library = output / "liblwip.a"
    shutil.copy2(sdk / "lib/liblwip.a", library)
    members = run([archiver, "t", str(library)], capture_output=True).stdout.splitlines()
    if any(Path(obj).name not in members for obj in objects):
        raise RuntimeError("The SDK archive has unexpected object names.")
    run([archiver, "rcs", str(library), *objects])

    # Preserve the SDK's library ordering and repeated groups. Replace every
    # occurrence so the linker cannot select a stock object in a later pass.
    libraries = shlex.split((sdk / "flags/ld_libs").read_text())
    if "-llwip" not in libraries:
        raise RuntimeError("Could not locate lwIP in the SDK linker response file.")
    linker_file = output / "ld_libs"
    linker_file.write_text(shlex.join(str(library) if lib == "-llwip" else lib for lib in libraries))
    manifest = {
        "arduino_core": Path(props["runtime.platform.path"]).name,
        "lwip_commit": LWIP_COMMIT,
        "tcp_send_buffer": TCP_SEND_BUFFER,
        "verified_headers": checked,
        "recompiled_sources": list(SOURCES),
        "sdk_config_sha256": hashlib.sha256(original_config.encode()).hexdigest(),
        "library_sha256": hashlib.sha256(library.read_bytes()).hexdigest(),
    }
    (output / "profile.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return linker_file, library


def build(stock_network=False):
    command = ["arduino-cli", "compile", "--fqbn", FQBN, "--build-path", str(BUILD)]
    library = None
    if not stock_network:
        linker_file, library = network_library(properties())
        command.extend([
            "--build-property", f'compiler.c.elf.libs="@{linker_file}"',
            "--build-property", f"compiler.cpp.extra_flags=-DCAMERA_TCP_SEND_BUFFER_BYTES={TCP_SEND_BUFFER}",
        ])
    run([*command, str(SKETCH)])
    if library:
        link_map = (BUILD / "camera_stream.ino.map").read_text()
        for source in SOURCES:
            if f"{library}({Path(source).name}.obj)" not in link_map:
                raise RuntimeError(f"The link map did not select the replacement {source}.")
        print("Verified the firmware links the larger-window TCP implementation.", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--upload", action="store_true", help="Flash after a successful build")
    parser.add_argument("--port", default="/dev/cu.usbserial-10")
    parser.add_argument("--stock-network", action="store_true", help="Use the unmodified SDK for comparison")
    args = parser.parse_args()
    try:
        build(args.stock_network)
        if args.upload:
            run([
                "arduino-cli", "upload", "--fqbn", FQBN + ":UploadSpeed=460800",
                "--port", args.port, "--input-dir", str(BUILD), str(SKETCH),
            ])
    except (OSError, RuntimeError, subprocess.CalledProcessError, tarfile.TarError) as error:
        parser.exit(1, f"Camera build failed: {error}\n")


if __name__ == "__main__":
    main()
