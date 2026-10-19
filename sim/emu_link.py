#!/usr/bin/env python3
"""A minimal host for rytm-emu's Overbridge link with the hello (rytm-emu src/oblink.rs): asks for the USB MIDI pair,
no R frames and voice frames, answers every N with silent voice frames ('a'), and prints the Rytm's USB MIDI
(decoded with sim/protocol.py). The emulator side of the Pi engine's TCP link, tested without the Pi.

    emu_link.py [--link HOST:PORT] [--secs S] [--send HEX]   (--send: one MIDI message into the Rytm at 2 s)
"""
import argparse
import os
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import protocol as P  # noqa: E402

HELLO_MIDI, HELLO_NO_R, HELLO_VOICES = 1, 2, 4


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--link", default="127.0.0.1:7301")
    ap.add_argument("--secs", type=float, default=10.0)
    ap.add_argument("--send", default=None)
    a = ap.parse_args()
    h, p = a.link.rsplit(":", 1)
    t0 = time.time()
    while True:
        try:
            s = socket.create_connection((h, int(p)), timeout=1)
            break
        except OSError:
            if time.time() - t0 > 20:
                sys.exit(f"no link at {a.link}")
            time.sleep(0.5)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.settimeout(0.2)
    s.sendall(bytes([ord("H"), 1, HELLO_MIDI | HELLO_NO_R | HELLO_VOICES]))
    buf, frames, granted, midi, sent = b"", 0, None, b"", False
    t0 = time.time()
    while time.time() - t0 < a.secs:
        if a.send and not sent and time.time() - t0 > 2:
            m = bytes.fromhex(a.send)
            s.sendall(b"M" + struct.pack("<H", len(m)) + m)
            sent = True
        try:
            d = s.recv(65536)
            if not d:
                break
            buf += d
        except socket.timeout:
            pass
        while buf:
            k = buf[0:1]
            if k == b"N" and len(buf) >= 5:
                n = struct.unpack("<I", buf[1:5])[0]
                buf = buf[5:]
                s.sendall(b"a" + struct.pack("<I", n) + bytes(16 * n))
                frames += n
            elif k == b"H" and len(buf) >= 3:
                granted, buf = buf[2], buf[3:]
                print(f"hello answered: flags {granted:#x}", flush=True)
            elif k == b"M" and len(buf) >= 3 and len(buf) >= 3 + struct.unpack("<H", buf[1:3])[0]:
                n = struct.unpack("<H", buf[1:3])[0]
                midi += buf[3:3 + n]
                buf = buf[3 + n:]
                while b"\xf0" in midi and b"\xf7" in midi[midi.index(b"\xf0"):]:
                    i = midi.index(b"\xf0")
                    j = midi.index(b"\xf7", i)
                    msg, midi = midi[i:j + 1], midi[j + 1:]
                    print("rytm sysex:", P.parse(msg) or msg.hex(" "), flush=True)
            elif k == b"T" and len(buf) >= 4:
                buf = buf[4:]
            elif k == b"P" and len(buf) >= 10:
                buf = buf[10:]
            elif k == b"R" and len(buf) >= 5 and len(buf) >= 5 + 48 * struct.unpack("<I", buf[1:5])[0]:
                buf = buf[5 + 48 * struct.unpack("<I", buf[1:5])[0]:]
                print("unexpected R frames (asked for none)", flush=True)
            elif k in (b"N", b"H", b"M", b"T", b"P", b"R"):
                break   # incomplete: more bytes to come
            else:
                sys.exit(f"bad message {buf[:8]!r}")
    print(f"served {frames} frames in {time.time() - t0:.1f} s; hello flags {granted}")


if __name__ == "__main__":
    main()
