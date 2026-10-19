#!/usr/bin/env python3
"""Print a Teensy's USB serial output: serial_read.py [--secs S] [--send CHARS]
Waits for the Teensy's serial port to appear (after an upload it re-enumerates)."""
import argparse
import glob
import sys
import time

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--secs", type=float, default=60.0)
    ap.add_argument("--send", default="")
    a = ap.parse_args()
    t0 = time.time()
    port = None
    while time.time() - t0 < 20 and not port:
        found = sorted(glob.glob("/dev/cu.usbmodem*"))
        port = found[0] if found else None
        if not port:
            time.sleep(0.2)
    if not port:
        sys.exit("no Teensy serial port (/dev/cu.usbmodem*)")
    with serial.Serial(port, 115200, timeout=0.2) as s:
        if a.send:
            s.write(a.send.encode())
        while time.time() - t0 < a.secs:
            try:
                d = s.read(4096)
            except serial.SerialException:
                print("\n(serial port went away)")
                return
            if d:
                sys.stdout.write(d.decode(errors="replace"))
                sys.stdout.flush()


if __name__ == "__main__":
    main()
