#!/usr/bin/env python3
# plugins/modele/tests/demo.py - listening demos of MODEL E: libmodele.dylib (the native build) played as a mono track
# plays it (one state, its notes in turn), 48 kHz, 12-frame blocks. Four WAVs: a bass line on the defaults, a lead with
# glide, a filter sweep (into self-oscillation), osc 3 modulation (vibrato, a square trill, audio rate).
#   python tests/demo.py [OUTDIR]     (make -C plugins/modele demo DEMO=OUTDIR; default build/demos)
# GPL-2.0-or-later, Copyright 2026 trigcondition and contributors.
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
KNOB = {k["short"]: k for k in MAN["knobs"]}
BASS = [36, 36, 48, 36, 39, 36, 43, 36, 36, 46, 36, 48, 41, 36, 43, 34]


def val(short, v):
    """a knob value: a number, or the name of a list entry"""
    if isinstance(v, str):
        return 8 * KNOB[short]["names"].index(v) + 4
    return float(v)


def knobs(**over):
    p = R.defaults(MAN)
    for k, v in over.items():
        k = {"GA": "G<>A"}.get(k, k)
        p[KNOB[k]["p"] - 1] = val(k, v)
    return p


def play(pl, events, seconds, prm, sweep=None):
    """events: (start s, length s, note) on one state; sweep(t) -> {short: value} each block"""
    total = int(seconds * FS) // BLOCK * BLOCK
    out = np.zeros(total, dtype=np.float64)
    buf = (C.c_float * BLOCK)()
    st = C.create_string_buffer(pl.state_bytes + 16)
    sp = C.cast(st, C.c_void_p)
    pl.voice_init(sp)
    pl.params(sp, (C.c_float * 16)(*prm))
    ons = {int(t * FS) // BLOCK * BLOCK: n for t, _, n in events}
    offs = {}
    for t, d, n in events:
        offs.setdefault(int((t + d) * FS) // BLOCK * BLOCK, []).append(n)
    held = None
    for i in range(0, total, BLOCK):
        if sweep:
            p = list(prm)
            for k, v in sweep(i / FS).items():
                p[KNOB[k]["p"] - 1] = val(k, v)
            pl.params(sp, (C.c_float * 16)(*p))
        if i in offs and held in offs[i] and i not in ons:   # the Rytm's gate: off unless a new note takes over
            pl.note_off(sp)
            held = None
        if i in ons:
            pl.note_on(sp, float(ons[i]), 0.8)
            held = ons[i]
        pl.render(sp, buf, BLOCK)
        out[i:i + BLOCK] = np.frombuffer(buf, dtype=np.float32)
    return out


def save(outdir, name, x):
    assert np.all(np.isfinite(x))
    pk = float(np.max(np.abs(x)))
    R.write_wav(outdir / f"modele-{name}.wav", x.astype(np.float32))   # as rendered: the level is the voice's own
    print(f"modele-{name}.wav: {len(x) / FS:.1f} s, peak {pk:.3f}, rms {float(np.sqrt(np.mean(x * x))):.3f}")


def main(outdir):
    lib = C.CDLL(str(HERE.parent / "libmodele.dylib"))
    pl = R.Plugin.in_dll(lib, "rmp_entry")
    assert pl.init(C.byref(R.HOST)) == 0
    outdir.mkdir(parents=True, exist_ok=True)
    six = 60 / 124 / 4
    # the bass line on the defaults, then an octave higher in the oscillators, the contour wider
    ev = [((b * 16 + k) * six, six * 0.6, n) for b in range(4) for k, n in enumerate(BASS)]
    a = play(pl, ev, 64 * six + 0.4, knobs())
    b = play(pl, ev, 64 * six + 0.4, knobs(OSC1="16 SAW", OSC2="8 SAW", OSC3="8 SQR", CUT=36, EMPH=70, CONT=92, DEC=54,
                                           SUS=48))
    save(outdir, "bass", np.concatenate([a, b]))
    # a lead: overlapping notes (the glide sings), a fifth-free stack of saws, the contour held open
    q = 60 / 124 / 2
    notes = [(0, 2, 60), (2, 2, 67), (4, 1, 65), (5, 1, 63), (6, 2, 62), (8, 3, 70), (11, 1, 67), (12, 4, 72)]
    ev = [(s * q, d * q + 0.03, n + 12) for s, d, n in notes]
    lead = dict(OSC1="8 SAW", OSC2="8 SQR", OSC3="4 SAW", TUN2=69, O3NZ=96, CUT=62, EMPH=62, CONT=38, DEC=80, SUS=92,
                MODE="K3 AN R", GA=36)
    save(outdir, "lead-glide", play(pl, ev, 16 * q + 1.2, knobs(**lead)))
    # a filter sweep: a held saw stack up and down, then with EMPH into self-oscillation (no oscillators), played on K3
    dur = 8.0
    up = play(pl, [(0, dur, 36)], dur + 0.3, knobs(CONT=0, SUS=127, EMPH=88, MODE="K0 AN"),
              sweep=lambda t: {"CUT": 14 + 100 * (0.5 - 0.5 * np.cos(2 * np.pi * t / dur))})
    so = play(pl, [(0, 2.0, 60), (2.0, 1.0, 67), (3.0, 1.0, 72), (4.0, 2.0, 60)], 6.4,
              knobs(MIX=0, O3NZ=64, CUT=58, EMPH=127, CONT=0, SUS=127, GA=44, MODE="K3 AN R", DEC=70))
    save(outdir, "filter-sweep", np.concatenate([up, np.zeros(FS // 4), so]))
    # osc 3 as a modulator: a free LO triangle's vibrato, a LO square's trill, then audio-rate modulation sweeping up
    v = play(pl, [(0, 3.0, 60)], 3.3, knobs(OSC1="8 SAW", OSC2="8 SAW", OSC3="LO TRI", O3NZ=64, MOD=46, TUN3=114, CUT=80,
                                            CONT=20, SUS=127))
    t = play(pl, [(0, 3.0, 60)], 3.3, knobs(OSC1="8 TRI", OSC2="8 SQR", OSC3="LO SQR", O3NZ=64, MOD=96, TUN3=100, CUT=80,
                                            CONT=20, SUS=127))
    f = play(pl, [(0, 5.0, 48)], 5.3, knobs(OSC1="8 SAW", OSC2="8 TRI", OSC3="8 TRI", O3NZ=78, CUT=88, EMPH=50, CONT=10,
                                            SUS=127, TUN3=71),
             sweep=lambda s: {"MOD": 64 + 60 * min(1.0, s / 4.5)})
    gap = np.zeros(FS // 4)
    save(outdir, "osc3-mod", np.concatenate([v, gap, t, gap, f]))


if __name__ == "__main__":
    main(pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else HERE.parent / "build" / "demos")
