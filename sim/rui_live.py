#!/usr/bin/env python3
"""The remote UI's live pages (core/ui/extra.h: SCOPE, REC) driven through the real host (core/build/libcore.dylib's
remote_sysex / remote_tick, as rui_link.py does) with a synthetic Overbridge IN stream: PNG mockups of what the Rytm
shows, and the link's cost (bytes and tiles a second, frames a second) while the music plays.

    python3 sim/rui_live.py [--out DIR] [--secs S] [--scale N]     (default sim/build/rui_live, git-ignored)

The stream (12 channels, int32 full scale, the Rytm's order: MAIN L R, tracks BD SD RS BT LT MT CH CY, IN L R): a
120 BPM pattern, a kick on every beat (voice 1, BD), a snare on 2 and 4 (SD), closed hats on the eighths (CH), a bass
tone on BT; MAIN their sum. Fed in 7-frame blocks at the emulated time, the host ticked every millisecond.
"""
import argparse
import ctypes
import os
import sys
import tempfile
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from rui_render import frame_png, LIB  # noqa: E402

SR = 48000
CH = dict(BD=2, SD=3, RS=4, BT=5, LT=6, MT=7, CH=8, CY=9)


def pattern(secs, seed=1):
    """(n, 12) int32: the synthetic Rytm IN stream."""
    n = int(secs * SR)
    t = np.arange(n) / SR
    rng = np.random.default_rng(seed)
    x = np.zeros((n, 12))
    beat = 0.5
    ph = t % beat
    x[:, CH["BD"]] = 0.8 * np.sin(2 * np.pi * (50 * ph + 120 * (1 - np.exp(-ph * 30)) / 30)) * np.exp(-ph * 9)
    ph2 = (t - beat) % (2 * beat)
    x[:, CH["SD"]] = 0.5 * (rng.standard_normal(n) * 0.6 + np.sin(2 * np.pi * 190 * ph2)) * np.exp(-ph2 * 18)
    ph3 = t % (beat / 2)
    x[:, CH["CH"]] = 0.25 * rng.standard_normal(n) * np.exp(-ph3 * 60)
    x[:, CH["BT"]] = 0.3 * np.sin(2 * np.pi * 55 * t) * (0.6 + 0.4 * np.sin(2 * np.pi * 0.5 * t))
    mix = x[:, 2:10].sum(axis=1) * 0.5
    x[:, 0] = mix
    x[:, 1] = mix * 0.95
    x[:, 10] = 0.05 * np.sin(2 * np.pi * 440 * t)
    x[:, 11] = 0.04 * np.sin(2 * np.pi * 330 * t)
    return (np.clip(x, -1, 1) * 2147483647).astype(np.int32)


class Live:
    def __init__(self, lib=None):
        self.lib = lib or ctypes.CDLL(LIB)
        L = self.lib
        L.core_c_begin.argtypes = [ctypes.c_char_p]
        L.core_c_sysex.argtypes = [ctypes.c_char_p, ctypes.c_int]
        L.core_c_rui_tick.argtypes = [ctypes.c_uint32]
        L.core_c_take_sysex.argtypes = [ctypes.c_char_p, ctypes.c_int]
        L.core_c_rui_frame_preview.argtypes = [ctypes.c_char_p]
        L.core_c_tap_in.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
        L.core_c_rui_stats.argtypes = [ctypes.POINTER(ctypes.c_uint32), ctypes.c_int]
        self.ms = 0
        self.bytes = self.frames = self.tiles = 0
        self.buf = ctypes.create_string_buffer(8192)

    def begin(self, card=None):
        self.lib.core_c_begin((card or tempfile.mkdtemp(prefix="rui_live_")).encode())

    def send(self, cmd, *body):
        m = bytes([0xF0, 0x7D, 0x52, 0x54, cmd, 1] + list(body) + [0xF7])
        self.lib.core_c_sysex(m, len(m))

    def drain(self):
        while True:
            k = self.lib.core_c_take_sysex(self.buf, 8192)
            if not k:
                return
            m = self.buf.raw[:k]
            if m[:4] == b"\xF0\x7D\x52\x54" and 0x30 <= m[4] <= 0x3F:
                self.bytes += k
                if m[4] == 0x39:
                    self.frames += 1
                elif m[4] == 0x38:
                    self.tiles += m[9] * m[10]

    def open(self):
        self.send(0x30, 0, 1, 0)
        self.tick(30)

    def key(self, code):
        self.send(0x31, code >> 7, code & 0x7F, 1)
        self.send(0x31, code >> 7, code & 0x7F, 0)
        self.tick(30)

    def turn(self, enc, delta):
        self.send(0x32, enc, delta + 64, 0)
        self.tick(30)

    def tick(self, ms, audio=None, at=0):
        """ms of emulated time, 1 ms a step; audio: the stream, fed from frame `at` (7-frame blocks); returns the next."""
        for _ in range(ms):
            if audio is not None:
                n = 48
                blk = np.ascontiguousarray(audio[at:at + n])
                if len(blk) == n:
                    for i in range(0, n, 7):
                        b = np.ascontiguousarray(blk[i:i + 7])
                        self.lib.core_c_tap_in(b.ctypes.data, len(b), 12)
                at += n
            self.ms += 1
            self.lib.core_c_rui_tick(self.ms)
            self.drain()
        return at

    def frame(self):
        fb = ctypes.create_string_buffer(1024)
        self.lib.core_c_rui_frame_preview(fb)
        return fb.raw

    def stats(self):
        w = (ctypes.c_uint32 * 20)()
        self.lib.core_c_rui_stats(w, 20)
        return list(w)


def measure(live, audio, secs, at=0):
    """The link while the page shows the music: (bytes/s, tiles/s, frames/s, the next frame of audio)."""
    b0, t0, f0 = live.bytes, live.tiles, live.frames
    at = live.tick(int(secs * 1000), audio, at)
    return ((live.bytes - b0) / secs, (live.tiles - t0) / secs, (live.frames - f0) / secs, at)


def write_wav(path, x, rate=48000):
    """x: (n,) or (n, ch) floats -1..1 -> 16-bit PCM."""
    x = np.asarray(x)
    ch = 1 if x.ndim == 1 else x.shape[1]
    with wave.open(path, "wb") as w:
        w.setnchannels(ch), w.setsampwidth(2), w.setframerate(rate)
        w.writeframes((np.clip(x, -1, 1) * 32767).astype("<i2").tobytes())


def card_with_samples():
    """A card folder: /Samples/Drums/break.wav (2 s of the pattern's MAIN), /Samples/Drums/pad.wav (3 s, a swelling
    chord): the pool's slots 1 and 2 once assigned (folder 1)."""
    d = tempfile.mkdtemp(prefix="rui_live_card_")
    os.makedirs(os.path.join(d, "Samples", "Drums"))
    write_wav(os.path.join(d, "Samples", "Drums", "break.wav"), pattern(2.0)[:, 0] / 2147483647.0)
    t = np.arange(3 * SR) / SR
    pad = (np.sin(2 * np.pi * 220 * t) + 0.6 * np.sin(2 * np.pi * 277 * t) + 0.5 * np.sin(2 * np.pi * 330 * t)) / 2.2
    write_wav(os.path.join(d, "Samples", "Drums", "pad.wav"), pad * np.sin(np.pi * t / 3) ** 2)
    return d


def report03(lv, vi, machine, p):
    m = bytes([0xF0, 0x7D, 0x52, 0x54, 0x03, machine | vi] + [b for k in p for b in (k, 0)] + [0xF7])
    lv.lib.core_c_sysex(m, len(m))


def wave_shots(lv, out, scale):
    """DSP SAMPLE / GRAIN on voice 1 (slot 1, the break): the page in several modes."""
    import protocol as P
    for slot, f in ((1, 0), (2, 1)):
        m = P.pool_assign(slot, 1, f)
        lv.lib.core_c_sysex(m, len(m))
    lv.drain()
    base = [0, 64, 64, 64, 0, 64, 64, 100, 64, 64, 1, 0, 0, 120, 0, 100]
    report03(lv, 0, 0x20, base)
    lv.key(0x45)                  # NO: SIDECAR
    lv.key(0x41), lv.key(0x41)    # SYSTEM, voice 1
    lv.tick(300)
    shots = [("dsps_one", dict(p12=10, p13=100)), ("dsps_loop", dict(p11=20, p12=30, p13=90, p14=70)),
             ("dsps_slice", dict(p11=40, p12=0, p13=120, p14=50)), ("dsps_reverse", dict(p11=0, p12=110, p13=20, p14=0)),
             ("dsps_pad_one", dict(p10=2, p11=0, p12=5, p13=115, p14=1)),
             ("grain", dict(p10=1, p11=56, p12=40, p13=70, p14=100)), ("grain_small", dict(p12=90, p13=20, p14=40))]
    p = list(base)
    for name, kv in shots:
        for k, v in kv.items():
            p[int(k[1:])] = v
        report03(lv, 0, 0x20, p)
        lv.tick(300)
        path = os.path.join(out, name + ".png")
        frame_png(path, lv.frame(), scale)
        print(path)


def rec_shots(lv, out, scale, audio, at):
    """REC with a recorder's states (the Pi's; core_c_rui_rec): idle, armed, recording an hour in, an error."""
    L = lv.lib
    L.core_c_rui_rec.argtypes = [ctypes.c_int, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32,
                                 ctypes.c_int, ctypes.c_char_p, ctypes.c_char_p]
    L.core_c_rui_rec(1, 0, 23 * 1024 + 400, 30 * 1024, 1152, 0x3FF, b"take-0007", b"")
    lv.key(0x45)
    lv.key(0x40), lv.key(0x40)   # SIDECAR -> SCOPE -> REC
    for name, st, secs, err in (("rec_idle", 1, 0, b""), ("rec_armed", 2, 0, b""), ("rec_recording", 3, 3725, b""),
                                ("rec_error", 1, 95, b"no channel selected")):
        L.core_c_rui_rec(st, secs, 23 * 1024 + 400, 30 * 1024, 1152, 0x3FF, b"take-0008" if secs else b"take-0007", err)
        at = lv.tick(400, audio, at)
        path = os.path.join(out, name + ".png")
        frame_png(path, lv.frame(), scale)
        print(path)
    L.core_c_rui_rec(0, 0, 0, 0, 0, 0, b"", b"")
    return at


def gfx_shots(lv, out, scale):
    """JUNO (voice 2) and 6-OP FM (voice 3) pages: envelopes and algorithms as the knobs move."""
    for name, vi, m, p in (("juno_slow", 1, 0x08, [5, 70, 30, 64, 90, 100, 110, 127]),
                           ("juno_pluck", 1, 0x08, [12, 90, 80, 100, 0, 30, 20, 40]),
                           ("fm_pat0", 2, 0x48, [0, 64, 64, 100, 64, 64, 100, 127]),
                           ("fm_pat5", 2, 0x48, [5, 64, 64, 60, 64, 64, 100, 127]),
                           ("fm_pat20", 2, 0x48, [20, 64, 64, 30, 64, 64, 100, 127])):
        report03(lv, vi, m, p + [64] * 8)
        lv.key(0x45)
        for _ in range(2 + vi):   # SYSTEM, voice 1, .. voice vi + 1
            lv.key(0x41)
        lv.tick(100)
        path = os.path.join(out, name + ".png")
        frame_png(path, lv.frame(), scale)
        print(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(HERE, "build", "rui_live"))
    ap.add_argument("--secs", type=float, default=4.0)
    ap.add_argument("--scale", type=int, default=4)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    audio = pattern(a.secs * 4 + 2)
    os.environ.setdefault("CORE_SAMPLE_RAM_MB", "16")   # the sample RAM (the Pi): DSP SAMPLE's modes, GRAIN
    lv = Live()
    lv.begin(card_with_samples())
    lv.open()
    lv.key(0x40)   # LEFT on SIDECAR: the last page, SCOPE
    st = lv.stats()
    if st[14] != 3:
        sys.exit(f"not on SCOPE: page {st[14]}")
    at = lv.tick(300, audio)
    rows = []
    for name, turns in (("scope_main_43ms", []), ("scope_main_21ms", [(1, -1)]), ("scope_v1_85ms", [(0, 1), (1, 2)]),
                        ("scope_main_341ms", [(0, -1), (1, 2)])):
        for e, d in turns:
            lv.turn(e, d)
        at = lv.tick(1200, audio, at)
        bps, tps, fps, at = measure(lv, audio, a.secs / 4, at)
        p = os.path.join(a.out, name + ".png")
        frame_png(p, lv.frame(), a.scale)
        print(f"{p}: {bps:.0f} B/s, {tps:.0f} tiles/s, {fps:.1f} frames/s ({bps / max(fps, 0.1):.0f} B a frame)")
        rows.append((name, bps, tps, fps))
    at = rec_shots(lv, a.out, a.scale, audio, at)
    wave_shots(lv, a.out, a.scale)
    gfx_shots(lv, a.out, a.scale)
    return rows


if __name__ == "__main__":
    main()
