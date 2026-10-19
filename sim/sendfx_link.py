#!/usr/bin/env python3
"""The engine core (core/build/libcore.dylib, the code the Pi and the Teensy run) as rytm-emu's Overbridge host for
the Sidecar send FX: the Rytm's USB MIDI into the core (sim/coreengine.py), the core's SysEx (0x21 NAMES, 0x22 STATE
and the rest) back into the Rytm, a log of what the core took and holds, and the core's SendFx run live on a
synthetic stem with the values the Rytm sent (the link carries no IN stems: the stem is made here).

    sendfx_link.py --card DIR [--link HOST:PORT] [--log OUT.jsonl] [--wav OUT.wav] [--secs S]
    sendfx_link.py --card DIR --replay LOG.jsonl --blocks LIVE.wav.blocks.npy --wav OUT.wav [--stem impulse|sine]

--card: the core's card (a folder: /.rytm/fxslots/<project>.txt for the user-FX slots). The log, one JSON object a
line, each stamped with "f" = frames the Rytm asked for so far (48 kHz: the emulated time since the link started):
  {"rx": [cmd, payload]}       a SysEx from the Rytm (protocol.parse)
  {"vals": {...}}              the core's send FX values after a 0x20 changed them (send, knobs, route, tags)
  {"show": "sendfx: ..."}      the :sendfx console answer when it changes
  {"tx": [cmd, payload]}       a SysEx the core sent the Rytm
--wav: 3 channels at 48 kHz - the synthetic stem (BD, IN 2), the SendFx's L and R. The stem: a one-sample impulse
every 0.25 s (amplitude 0.5) on BD; the other stems silent. One process per run (the core keeps module state).
--replay: no link; a fresh core takes the logged run's 0x20 messages at their frames and renders the stem (impulse as
above, or sine: 220 Hz at 0.3 on BD) through SendFx in the same blocks: an impulse replay equals the live --wav.
"""
import argparse
import ctypes
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
SR = 48000
IMPULSE_EVERY = SR // 4


def jsonable(x):
    if isinstance(x, (bytes, bytearray)):
        return x.hex()
    if isinstance(x, dict):
        return {str(k): jsonable(v) for k, v in x.items()}
    if isinstance(x, (list, tuple)):
        return [jsonable(v) for v in x]
    return x


class Host:
    def __init__(self, card, log, wav):
        self.core = CoreEngine(card)
        lib = self.core.lib
        lib.core_c_sendfx_process.argtypes = [ctypes.POINTER(ctypes.c_float), ctypes.c_int,
                                              ctypes.POINTER(ctypes.c_float), ctypes.c_float]
        self.log = open(log, "w") if log else None
        self.wav = wav
        self.frames, self.vals, self.show = 0, None, None
        self.audio, self.blocks = [], []
        self.faults = 0

    def note(self, **kw):
        if self.log:
            self.log.write(json.dumps(jsonable(dict(f=self.frames, **kw))) + "\n")
            self.log.flush()

    def values(self):
        b = (ctypes.c_uint8 * 64)()
        n = self.core.lib.core_c_sendfx_values(b)
        v = bytes(b[:n])
        return dict(send=[[v[2 * t], v[2 * t + 1]] for t in range(12)], knobs=[list(v[24:30]), list(v[30:36])],
                    route=v[36], tags=[v[37] << 7 | v[38], v[39] << 7 | v[40]])

    def command(self, c):
        r = ctypes.create_string_buffer(400)
        self.core.lib.core_c_sendfx_command(c.encode(), r, 400)
        return r.value.decode()

    def from_rytm(self, msg):
        r = P.parse(msg)
        self.note(rx=r if r else msg)
        self.core.report(msg)
        v = self.values()
        if v != self.vals:
            self.vals = v
            self.note(vals=v)
        s = self.command("sendfx")
        s = s[:s.rfind(";")] if "0x20 msgs" in s else s   # the message count changes every time
        if s != self.show:
            self.show = s
            self.note(show=s)
        out = self.core.take_sysex()
        for m in out:
            r = P.parse(m)
            self.note(tx=r if r else m)
        return out

    def render(self, n, stem="impulse"):
        """n frames: the synthetic stem through the core's SendFx (live values)."""
        stems = np.zeros((n, 8), np.float32)
        idx = np.arange(self.frames, self.frames + n)
        if stem == "sine":
            stems[:, 0] = (0.3 * np.sin(2 * np.pi * 220.0 * idx / SR)).astype(np.float32)
        else:
            stems[idx % IMPULSE_EVERY == 0, 0] = 0.5
        out = np.zeros((n, 2), np.float32)
        self.blocks.append(n)
        fp = ctypes.POINTER(ctypes.c_float)
        self.faults = self.core.lib.core_c_sendfx_process(stems.ctypes.data_as(fp), n, out.ctypes.data_as(fp), SR)
        if self.wav:
            self.audio.append(np.column_stack([stems[:, 0], out]))

    def save(self):
        self.note(end=True, faults=self.faults)
        if self.wav and self.audio:
            a = np.clip(np.concatenate(self.audio), -1, 1)
            with wave.open(self.wav, "wb") as w:
                w.setnchannels(3)
                w.setsampwidth(2)
                w.setframerate(SR)
                w.writeframes((a * 32767).astype("<i2").tobytes())
            np.save(self.wav + ".blocks.npy", np.array(self.blocks, np.int32))   # the N sizes, for --replay


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
                h.render(n)
                h.frames += n
                s.sendall(b"a" + struct.pack("<I", n) + bytes(16 * n))
            elif k == b"H" and len(buf) >= 3:
                h.note(hello=buf[2])
                buf = buf[3:]
            elif k == b"M" and len(buf) >= 3 and len(buf) >= 3 + struct.unpack("<H", buf[1:3])[0]:
                n = struct.unpack("<H", buf[1:3])[0]
                data, buf = buf[3:3 + n], buf[3 + n:]
                for c in data:   # a byte stream: whole SysEx out of it (real-time bytes inside one skipped)
                    if c == 0xF0:
                        midi, insx = bytearray([c]), True
                    elif c >= 0xF8:
                        continue
                    elif insx:
                        midi.append(c)
                        if c == 0xF7:
                            insx = False
                            for m in h.from_rytm(bytes(midi)):
                                s.sendall(b"M" + struct.pack("<H", len(m)) + m)
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


def replay(h, log, stem, blocks):
    """The logged run again, without the link: the live run's N blocks (BLOCKS.npy), the 0x20s between them."""
    rx = {}
    with open(log) as f:
        lines = f.readlines()
    for line in lines:
        e = json.loads(line)
        if "rx" in e and e["rx"][0] == P.SENDFX and isinstance(e["rx"][1], str):
            rx.setdefault(e["f"], []).append(P.HDR + bytes([P.SENDFX]) + bytes.fromhex(e["rx"][1]) + b"\xf7")
    lib = h.core.lib
    for n in np.load(blocks):
        for m in rx.pop(h.frames, []):
            lib.core_c_sendfx_take(m, len(m))
        h.render(int(n), stem)
        h.frames += int(n)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--link", default="127.0.0.1:7301")
    ap.add_argument("--card", required=True)
    ap.add_argument("--log")
    ap.add_argument("--wav")
    ap.add_argument("--secs", type=float, default=600.0)
    ap.add_argument("--replay")
    ap.add_argument("--blocks", help="--replay: the live run's N sizes (its --wav + .blocks.npy)")
    ap.add_argument("--stem", default="impulse", choices=["impulse", "sine"])
    a = ap.parse_args()
    h = Host(a.card, a.log, a.wav)
    try:
        if a.replay:
            replay(h, a.replay, a.stem, a.blocks)
        else:
            serve(h, a.link, a.secs)
    finally:
        h.save()


if __name__ == "__main__":
    main()
