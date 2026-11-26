#!/usr/bin/env python3
# plugins/bp303/tests/demo.py - listening demos of BP303: libbp303.dylib (the native build) played as a mono track plays
# it (one state, note_on on every trig, note_off when the gate closes; an overlapping trig is a slide; velocity 127 an
# accent), 48 kHz, 12-frame blocks. One 16-step acid line at 126 BPM (accents, slides, a rest), four bars each:
#   bp303-1-line.wav        the defaults (saw, CUT 44, RES 64, ENV 32, the soft drive at VOL 99)
#   bp303-2-squelch.wav     RES 110, ENV 80, ACC 127, DEC 60; CUT swept 20 > 90 > 35
#   bp303-3-square.wav      WAVE 127, CUT 50, RES 90, ENV 64
#   bp303-4-knobs.wav       eight bars: CUT, then RES, ENV, DEC and WAVE turned in turn, back to the defaults
#   bp303-5-clean-low.wav   clean VOL 64, CUT 15, RES 100, ENV 100, DEC 90: the accent sweep building over accents in a row
#   python tests/demo.py [OUTDIR]      (make -C plugins/bp303 demo DEMO=OUTDIR; default build/demo)
# The files are as the voice plays them (no normalising): its own level and drive.
# MIT, Copyright 2026 trigcondition and contributors.
import ctypes as C
import json
import pathlib
import sys

import numpy as np

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent))
import sc_render as R   # noqa: E402  (its Host / Plugin ctypes tables and write_wav)

FS, BLOCK = R.FS, R.BLOCK
MAN = json.loads((HERE.parent / "manifest.json").read_text())
BY = {k["short"]: k["p"] for k in MAN["knobs"]}
STEP = 60 / 126 / 4
# (note, accent, slides into the next note); 0 = a rest
LINE = [(36, 0, 0), (36, 0, 0), (48, 1, 0), (36, 0, 1), (39, 0, 0), (36, 1, 0), (0, 0, 0), (46, 1, 0),
        (36, 0, 0), (48, 0, 1), (36, 1, 0), (36, 0, 0), (43, 0, 1), (41, 1, 0), (36, 0, 0), (34, 1, 0)]
ACCENTS = [(36, 0, 0), (36, 1, 0), (36, 1, 0), (36, 1, 0), (48, 1, 0), (36, 0, 1), (39, 1, 0), (36, 0, 0),
           (36, 1, 0), (36, 1, 0), (43, 1, 1), (36, 1, 0), (0, 0, 0), (36, 0, 0), (46, 1, 1), (48, 1, 0)]


def knobs(**over):
    p = R.defaults(MAN)
    for k, v in over.items():
        p[BY[k] - 1] = float(v)
    return p


def play(pl, line, bars, prm_at):
    """line: 16 steps; the gate is 58 % of a step (the 303's 3.5 of 6 clocks), a sliding note is held into the next"""
    step = int(STEP * FS) // BLOCK * BLOCK
    gate = int(step * 0.58) // BLOCK * BLOCK
    total = bars * 16 * step + int(0.4 * FS) // BLOCK * BLOCK
    out = np.zeros(total, dtype=np.float64)
    st = C.create_string_buffer(pl.state_bytes + 16)
    sp = C.cast(st, C.c_void_p)
    pl.voice_init(sp)
    buf = (C.c_float * BLOCK)()
    off = -1
    for i in range(0, total, BLOCK):
        pl.params(sp, (C.c_float * 16)(*prm_at(i / (bars * 16 * step))))
        if i == off:
            pl.note_off(sp)
        if i % step == 0 and i // step < bars * 16:
            n, acc, slide = line[(i // step) % 16]
            if n:
                pl.note_on(sp, float(n), 1.0 if acc else 0.6)
                off = i + (step + gate if slide else gate)
        pl.render(sp, buf, BLOCK)
        out[i:i + BLOCK] = np.frombuffer(buf, dtype=np.float32)
    return out


def tri(x, a, top, b, at=0.55):
    return a + (top - a) * (x / at) if x < at else top + (b - top) * ((x - at) / (1 - at))


def knob_tour(x):
    x = min(x, 0.9999) * 5
    seg, f = int(x), x - int(x)
    bump = 0.5 - 0.5 * np.cos(2 * np.pi * f)             # 0 > 1 > 0 over the segment
    return [knobs(CUT=44 + 60 * bump), knobs(RES=64 + 63 * bump), knobs(ENV=32 + 95 * bump),
            knobs(DEC=72 - 72 * bump), knobs(WAVE=127 * bump)][seg]


def main(outdir):
    lib = C.CDLL(str(HERE.parent / "libbp303.dylib"))
    pl = R.Plugin.in_dll(lib, "rmp_entry")
    assert pl.init(C.byref(R.HOST)) == 0
    outdir.mkdir(parents=True, exist_ok=True)
    renders = [
        ("bp303-1-line", LINE, 4, lambda x: knobs()),
        ("bp303-2-squelch", LINE, 4, lambda x: knobs(RES=110, ENV=80, ACC=127, DEC=60, CUT=tri(min(x, 1.0), 20, 90, 35))),
        ("bp303-3-square", LINE, 4, lambda x: knobs(WAVE=127, CUT=50, RES=90, ENV=64)),
        ("bp303-4-knobs", LINE, 8, knob_tour),
        ("bp303-5-clean-low", ACCENTS, 4, lambda x: knobs(VOL=64, CUT=15, RES=100, ENV=100, DEC=90, ACC=127)),
    ]
    for name, line, bars, prm in renders:
        x = play(pl, line, bars, prm)
        assert np.all(np.isfinite(x)) and float(np.max(np.abs(x))) <= 1.0, name
        R.write_wav(outdir / f"{name}.wav", x.astype(np.float32))
        print(f"{outdir / name}.wav: {len(x) / FS:.1f} s, rms {float(np.sqrt(np.mean(x * x))):.3f}, peak {float(np.max(np.abs(x))):.3f}")


if __name__ == "__main__":
    main(pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else HERE.parent / "build" / "demo")
