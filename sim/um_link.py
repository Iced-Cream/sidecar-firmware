#!/usr/bin/env python3
"""The engine core (core/build/libcore.dylib) with its user machines (core/user/um.h: the card's /Plugins, RMPA
packages run in place, as on the Pi) as rytm-emu's Overbridge host: the Rytm's USB MIDI into the core, the core's
SysEx back, its voices rendered and recorded. sim/um_e2e.py drives it.

    um_link.py --card DIR [--link HOST:PORT] [--log OUT.jsonl] [--wav OUT.wav] [--secs S] [--remove-at F:PATH]...

--wav: the 8 voices at 48 kHz (8 channels). --remove-at F:PATH: at frame F delete the card file PATH (a /Plugins
package, as the web UI's delete) and rescan. The log, one JSON object a line, "f" = frames so far:
  {"rx": [cmd, payload]} from the Rytm, {"tx": [cmd, payload]} to it, {"um": [...]} the status after a change,
  {"removed": path}.
"""
import argparse
import json
import os
import socket
import struct
import sys
import time
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import protocol as P  # noqa: E402
from coreengine import CoreEngine  # noqa: E402

HELLO_MIDI, HELLO_NO_R, HELLO_VOICES = 1, 2, 4


def jsonable(x):
    if isinstance(x, (bytes, bytearray)):
        return x.hex()
    if isinstance(x, (list, tuple)):
        return [jsonable(v) for v in x]
    return x


class Host:
    def __init__(self, card, log, wav, removes):
        self.card = card
        self.core = CoreEngine(card, um=True)
        self.log = open(log, "w") if log else None
        self.wav, self.audio = wav, []
        self.frames = 0
        self.removes = sorted(removes)
        self.last_um = None

    def note(self, **kw):
        if self.log:
            self.log.write(json.dumps(dict(f=self.frames, **{k: jsonable(v) for k, v in kw.items()})) + "\n")
            self.log.flush()

    def out(self):
        msgs = self.core.take_sysex()
        for m in msgs:
            r = P.parse(m)
            self.note(tx=r if r else m)
        st = [x for x in self.core.um_json() if x["t"] in ("slot", "plugins")]
        if st != self.last_um:
            self.last_um = st
            self.note(um=st)
        return msgs

    def from_rytm(self, msg):
        r = P.parse(msg)
        self.note(rx=r if r else msg)
        self.core.report(msg)
        return self.out()

    def render(self, n):
        while self.removes and self.removes[0][0] <= self.frames:
            _, path = self.removes.pop(0)
            try:
                os.remove(os.path.join(self.card, path.lstrip("/")))
            except OSError:
                pass
            self.note(removed=path)
            self.core.um_command("um=rescan")
        self.core.idle()
        a = self.core.frames(n)
        if self.wav:
            self.audio.append(a.copy())
        return self.out()

    def save(self):
        self.note(end=True)
        if self.wav and self.audio:
            with wave.open(self.wav, "wb") as w:
                w.setnchannels(8)
                w.setsampwidth(2)
                w.setframerate(48000)
                w.writeframes(np.concatenate(self.audio).astype("<i2").tobytes())


def serve(h, link, secs):
    host, port = link.rsplit(":", 1)
    t0 = time.time()
    while True:
        try:
            s = socket.create_connection((host, int(port)), timeout=1)
            break
        except OSError:
            if time.time() - t0 > 60:
                sys.exit(f"no link at {link}")
            time.sleep(0.2)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.settimeout(0.2)
    s.sendall(bytes([ord("H"), 1, HELLO_MIDI | HELLO_NO_R | HELLO_VOICES]))

    def send(msgs):
        for m in msgs:
            s.sendall(b"M" + struct.pack("<H", len(m)) + m)

    buf, midi, insx = b"", bytearray(), False
    t0 = time.time()
    while time.time() - t0 < secs:
        try:
            d = s.recv(65536)
            if not d:
                break
            buf += d
        except socket.timeout:
            continue
        except OSError:
            break
        while buf:
            k = buf[0:1]
            if k == b"N" and len(buf) >= 5:
                n = struct.unpack("<I", buf[1:5])[0]
                buf = buf[5:]
                send(h.render(n))
                h.frames += n
                s.sendall(b"a" + struct.pack("<I", n) + bytes(16 * n))
            elif k == b"H" and len(buf) >= 3:
                buf = buf[3:]
            elif k == b"M" and len(buf) >= 3 and len(buf) >= 3 + struct.unpack("<H", buf[1:3])[0]:
                n = struct.unpack("<H", buf[1:3])[0]
                data, buf = buf[3:3 + n], buf[3 + n:]
                for c in data:
                    if c == 0xF0:
                        midi, insx = bytearray([c]), True
                    elif c >= 0xF8:
                        continue
                    elif insx:
                        midi.append(c)
                        if c == 0xF7:
                            insx = False
                            send(h.from_rytm(bytes(midi)))
                        elif c & 0x80:
                            insx = False
            elif k == b"T" and len(buf) >= 4:
                buf = buf[4:]
            elif k == b"P" and len(buf) >= 10:
                buf = buf[10:]
            elif k == b"R" and len(buf) >= 5 and len(buf) >= 5 + 48 * struct.unpack("<I", buf[1:5])[0]:
                buf = buf[5 + 48 * struct.unpack("<I", buf[1:5])[0]:]
            elif k in (b"N", b"H", b"M", b"T", b"P", b"R"):
                break
            else:
                sys.exit(f"bad message {buf[:8]!r}")
    s.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--link", default="127.0.0.1:7301")
    ap.add_argument("--card", required=True)
    ap.add_argument("--log")
    ap.add_argument("--wav")
    ap.add_argument("--secs", type=float, default=600.0)
    ap.add_argument("--remove-at", action="append", default=[], help="FRAME:PATH")
    a = ap.parse_args()
    rem = [(int(x.split(":", 1)[0]), x.split(":", 1)[1]) for x in a.remove_at]
    h = Host(a.card, a.log, a.wav, rem)
    try:
        serve(h, a.link, a.secs)
    finally:
        h.save()


if __name__ == "__main__":
    main()
