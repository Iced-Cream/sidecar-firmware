#!/usr/bin/env python3
# plugins/sophie/tests/demo.py - a listening demo of SOPHIE: libsophie.dylib (the native build) playing Sophie's
# factory kit (her 16 pads, GM / Drum Rack order) as knob settings, one state a track, 48 kHz, 12-frame blocks: each
# pad alone, then a two-bar pattern (kick, snare, hats, toms, ride) at 120 BPM.
#   python tests/demo.py [OUT.wav]      (make -C plugins/sophie demo DEMO=OUT.wav; default build/sophie-demo.wav)
#   python tests/demo.py --table        the kit as P1..P16 values (README's table)
# MIT, Copyright 2026 trigcondition and contributors.
import ctypes as C
import math
import pathlib
import sys

import numpy as np

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent))
import sc_render as R   # noqa: E402  (its Host / Plugin ctypes tables and write_wav)

FS, BLOCK = R.FS, R.BLOCK

# Sophie's set_factory_kit (v0.5.1): model, tune, color, metal, feedback, sweep, decay s, crush, drive; every pad
# with level 100, cutoff 100, resonance 0, LPF, ring 6 ms / 0 / 0 / 70. Pad i sounds at note 36 + i, so here TUN
# carries the pad's offset (note 60 plays as Sophie's 36); pad 1 is the kick topology (MODL KICK).
KIT = [
    ("Kick",      0, -5, 24,  8,  6,  55, 0.28,  0, 58),
    ("Rim",       3,  0, 76, 72, 34,  16, 0.09, 48, 58),
    ("Snare",     3,  0, 52, 76, 18,  28, 0.24, 30, 52),
    ("Clap",      2,  0, 84, 88, 42,  -8, 0.19, 56, 68),
    ("Snare 2",   1,  0, 63, 82, 28,  24, 0.18, 38, 58),
    ("Low Tom",   0,  0, 24, 44, 16,  58, 0.42,  8, 48),
    ("Closed HH", 3,  0, 94, 98, 58,   0, 0.055, 52, 62),
    ("Floor Tom", 0,  0, 31, 48, 20,  54, 0.47, 10, 50),
    ("Pedal HH",  3,  0, 88, 94, 50,   2, 0.10, 46, 58),
    ("Mid Tom",   0,  0, 38, 52, 22,  48, 0.36, 12, 52),
    ("Open HH",   3,  0, 91, 96, 62,  -4, 0.48, 42, 60),
    ("Low-Mid",   0,  0, 45, 56, 25,  44, 0.31, 14, 54),
    ("High-Mid",  0,  0, 51, 60, 28,  40, 0.27, 16, 56),
    ("Crash",     1,  0, 78, 92, 72, -12, 1.65, 28, 54),
    ("High Tom",  0,  0, 58, 64, 30,  36, 0.23, 18, 58),
    ("Ride",      2,  0, 69, 86, 66,  -6, 1.15, 22, 52),
]
SHORT = ["TUN", "MODL", "COLR", "METL", "FDBK", "SWP", "DEC", "CRSH", "DRV", "CUT", "RES", "FLT", "RTIM", "RFB", "RMIX", "RTON"]


def pct(x):
    return round(x * 127 / 100)


def knobs(i):
    """pad i of the kit as P1..P16 (sophie.c's mappings, inverted)"""
    _, model, tune, color, metal, fb, sweep, decay, crush, drive = KIT[i]
    z = model + (4 if i == 0 else 0)
    return [64 + 2 * (tune + i), z * 16 + 8, pct(color), pct(metal), pct(fb), round(64 + sweep * 0.63),
            round(127 * math.log(decay / 0.03) / math.log(4 / 0.03)), pct(crush), pct(drive), 127, 0, 12,
            round(127 * math.log(6 / 0.5) / math.log(60)), 0, 0, pct(70)]


def load():
    lib = C.CDLL(str(HERE.parent / "libsophie.dylib"))
    pl = R.Plugin.in_dll(lib, "rmp_entry")
    assert pl.init(C.byref(R.HOST)) == 0
    return pl


def track(pl, i, hits, seconds):
    """one track (one state) playing pad i at the given (time s, velocity) hits"""
    st = C.create_string_buffer(pl.state_bytes + 16)
    sp = C.cast(st, C.c_void_p)
    pl.voice_init(sp)
    pl.params(sp, (C.c_float * 16)(*[float(v) for v in knobs(i)]))
    total = int(seconds * FS) // BLOCK * BLOCK
    out = np.zeros(total, dtype=np.float32)
    buf = (C.c_float * BLOCK)()
    at = {int(t * FS) // BLOCK * BLOCK: v for t, v in hits}
    for k in range(0, total, BLOCK):
        if k in at:
            pl.note_on(sp, 60.0, float(at[k]))
        pl.render(sp, buf, BLOCK)
        out[k:k + BLOCK] = np.frombuffer(buf, dtype=np.float32)
    return out


def main(argv):
    if argv[:1] == ["--table"]:
        print("| Pad | " + " | ".join(SHORT) + " |")
        print("|---|" + "---|" * 16)
        for i in range(16):
            print(f"| {KIT[i][0]} | " + " | ".join(str(v) for v in knobs(i)) + " |")
        return 0
    out = pathlib.Path(argv[0]) if argv else HERE.parent / "build" / "sophie-demo.wav"
    out.parent.mkdir(parents=True, exist_ok=True)
    pl = load()
    parts = []
    for i in range(16):   # each pad alone, 0.9 s apart (the crash and ride ring on)
        x = track(pl, i, [(0.0, 0.9)], 0.9 if i not in (13, 15) else 1.8)
        print(f"pad {i + 1:2d} {KIT[i][0]:<10} peak {np.max(np.abs(x)):.3f} rms {R.rms(x[:FS // 4]):.3f}")
        parts.append(x)
    step = 60 / 120 / 4   # sixteenths at 120 BPM, two bars
    pat = {0: [0, 6, 8, 10], 2: [6], 4: [2, 6, 12], 6: [6], 8: [0, 6], 10: [0, 6], 12: [2, 6], 14: [6, 10],
           16: [0, 6, 13], 18: [6], 20: [2, 6], 22: [6, 9], 24: [0, 6], 26: [5, 6], 28: [2, 7, 15], 30: [6, 14]}
    lanes = {}
    for s, pads in pat.items():
        for p in pads:
            lanes.setdefault(p, []).append((s * step, 0.6 + 0.4 * ((s % 4) == 0)))
    mix = sum(track(pl, p, hits, 32 * step + 1.5) for p, hits in lanes.items())
    print(f"pattern    peak {np.max(np.abs(mix)):.3f}")
    parts.append(np.clip(mix * 0.6, -1, 1))
    R.write_wav(out, np.concatenate(parts))
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
