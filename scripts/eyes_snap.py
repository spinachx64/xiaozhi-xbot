#!/usr/bin/env python3
"""Capture screenshots dumped by CONFIG_EYES_UI_DEBUG firmware.

Reads the serial console, decodes ===SNAP-BEGIN ...=== blocks to JPEG files
and copies EyesView log lines to stdout.

    python scripts/eyes_snap.py -p /dev/cu.usbmodemXXXX -t 60 -o tmp/snaps --reset
"""
import argparse
import base64
import re
import time
from pathlib import Path

import serial

BEGIN = re.compile(r"===SNAP-BEGIN (\S+) (\d+)===")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-p", "--port", required=True)
    parser.add_argument("-t", "--timeout", type=float, default=60, help="seconds to listen")
    parser.add_argument("-o", "--out", default="tmp/eyes-snaps")
    parser.add_argument("--reset", action="store_true", help="reset the chip via RTS first")
    args = parser.parse_args()

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    ser = serial.Serial(args.port, 115200, timeout=0.5)
    if args.reset:
        ser.dtr = False
        ser.rts = True
        time.sleep(0.1)
        ser.rts = False

    deadline = time.time() + args.timeout
    label, chunks, count = None, [], 0
    while time.time() < deadline:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if not line:
            continue
        m = BEGIN.search(line)
        if m:
            label, chunks = m.group(1), []
            continue
        if label is not None and "SNAP:" in line:
            chunks.append(line.split("SNAP:", 1)[1])
            continue
        if label is not None and "===SNAP-END===" in line:
            count += 1
            path = out / f"{count:02d}-{label}.jpg"
            path.write_bytes(base64.b64decode("".join(chunks)))
            print(f"[snap] {path}")
            label = None
            continue
        if any(k in line for k in ("EyesView", "StateMachine", "Guru", "abort", "WDT")):
            print(line)
    ser.close()


if __name__ == "__main__":
    main()
