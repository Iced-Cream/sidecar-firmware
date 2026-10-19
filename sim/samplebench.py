"""DSP SAMPLE's playback modes: a CPU and PSRAM estimate for 8 voices on the
Teensy (docs/psram.md), from the native build on this Mac.

    cd sim && sampler/build.sh && uv run python samplebench.py

For each mode, 8 voices render 2 s of a 10 s sample (the costliest knob
settings: all grains busy, pitched up an octave where it matters). Printed:

- host ns a sample a voice, and the Teensy's share for 8 voices estimated by
  scaling with JUNO's and PLAITS WAVETABLE's ratio of host time to their
  measured Teensy load (sim/stress.py b and pw: 8 JUNO 27.3 %, 8 WAVETABLE
  49.0 %; docs/PLAN.md 3c). The compute part only: PSRAM reads cost as if from
  RAM here.
- PSRAM: the sample reads a second for 8 voices rendering block by block,
  interleaved as on the Teensy, through a model of the Teensy's 32 KB data
  cache (sampler_c.cc); each miss a 32-byte PSRAM read, and the stall if each
  costs T_MISS (the cost of a miss is to be measured: teensy/psram_test).
  Nothing else competes for the cache in the model: misses are a lower bound.
"""
import time

import numpy as np

from junovoice import JunoVoice
from plaitsvoice import Voice as PlaitsVoice
from samplemodes import MODES, ModeVoice, cache_reset, cache_stats, fast_lib, mode_value, to_mono16

SR = 48000
SECS = 2.0
T_MISS = 0.80e-6     # s: a 32-byte QPI read at 105.6 MHz is 78 clocks (0.74 us) + AHB (inferred)
MEASURED = {"JUNO": 27.3, "WAVETABLE": 49.0}   # % of the Teensy, 8 voices (PLAN.md 3c)

CASES = [   # (name, mode, TUN FIN STA END LOP)
    ("ONE", "ONE", 64, 64, 0, 120, 0),
    ("ONE +12", "ONE", 76, 64, 0, 120, 0),
    ("LOOP pingpong", "LOOP", 64, 64, 20, 100, 100),
    ("SLICE 16", "SLICE", 64, 64, 0, 120, 80),
    ("GRAIN dense", "GRAIN", 64, 64, 60, 64, 127),
    ("GRAIN dense +12", "GRAIN", 76, 64, 60, 64, 127),
    ("STRCH 1x", "STRCH", 64, 64, 0, 120, 64),
    ("STRCH 0.5x +12", "STRCH", 76, 64, 0, 120, 32),
    ("PLUCK", "PLUCK", 64, 64, 10, 110, 100),
    ("WAVE 600", "WAVE", 64, 64, 60, 0, 50),
    ("WAVE 1024", "WAVE", 64, 64, 60, 0, 70),
    ("WAVE 2048", "WAVE", 64, 64, 60, 0, 100),
    ("WAVE 2048 +24", "WAVE", 88, 64, 60, 0, 100),
]


def timed(fn, reps=3):
    best = 1e9
    for _ in range(reps):
        t = time.perf_counter()
        fn()
        best = min(best, time.perf_counter() - t)
    return best


def host_ns_per_sample(make, n):
    voices = [make(i) for i in range(8)]
    dt = timed(lambda: [r(n) for r in voices])
    return dt / (8 * n) * 1e9


def main():
    n = int(SECS * SR)
    budget_ns = 1e9 / SR   # a sample's time on one core

    # calibration: host time against the Teensy's measured load
    def juno(i):
        j = JunoVoice()
        j.trigger(48 + i, 1.0)
        return j.render

    def wavetable(i):
        p = PlaitsVoice()
        p.engine, p.note, p.decay = 13, 48 + i, 1.0
        p.trigger(48 + i)
        return p.render

    scale = {}
    for name, make in (("JUNO", juno), ("WAVETABLE", wavetable)):
        ns = host_ns_per_sample(make, n)
        scale[name] = MEASURED[name] / (8 * ns / budget_ns * 100)
        print(f"calibration {name}: host {ns:.1f} ns a sample a voice; Teensy {MEASURED[name]} % for 8 "
              f"-> x{scale[name]:.1f}")
    k = sum(scale.values()) / len(scale)

    rng = np.random.default_rng(3)
    t = np.arange(10 * SR)
    sig = (np.sin(2 * np.pi * 220 * t / SR) * 8000 + rng.normal(0, 2000, len(t))).astype(np.int16)
    # a sample a voice, at staggered cache sets (as the sample RAM packs them
    # on 32-byte lines, sample_ram.h; 4 KB-aligned blocks would map the same
    # stretch of eight samples onto the same sets)
    monos = []
    for i in range(8):
        m = to_mono16(np.roll(sig, 4801 * i).reshape(-1, 1))
        buf = np.zeros(len(m) + 4096, np.int16)
        o = 528 * i + 16 * (i * i % 7)
        buf[o:o + len(m)] = m
        monos.append(buf[o:o + len(m)])

    print(f"\n{'case':18} {'host ns':>8} {'Teensy 8 voices':>16} {'reads/s':>8} {'misses/s':>9} "
          f"{'stall':>6} {'total':>12}")
    lo_k, hi_k = min(scale.values()), max(scale.values())
    for name, mode, tun, fin, sta, end, lop in CASES:
        sp = [tun, fin, 1, mode_value(MODES.index(mode)), sta, end, lop, 100]

        def fresh(lib=None):
            vs = []
            for i in range(8):
                v = ModeVoice(i + 1, lib)
                v.set(monos[i], SR)
                v.trigger(60 + (i % 4), sp)
                vs.append(v)
            return vs

        vs = fresh(fast_lib())   # timing: the build without counts and cache model
        ns = host_ns_per_sample(lambda i: (lambda m, v=vs[i]: v.render(m, sp)), n)
        del vs
        vs = fresh()   # the cache: 0.5 s, 12-sample blocks, voice after voice
        cache_reset()
        blocks = SR // 2 // 12
        for _ in range(blocks):
            for v in vs:
                v.render(12, sp)
        reads, misses = cache_stats()
        secs = blocks * 12 / SR
        del vs
        lo, hi = (8 * ns / budget_ns * 100 * k for k in (lo_k, hi_k))
        stall = misses / secs * T_MISS * 100
        print(f"{name:18} {ns:8.1f} {lo:7.1f}..{hi:5.1f} % {reads / secs / 1e6:7.2f}M {misses / secs / 1e3:8.0f}k "
              f"{stall:5.1f}% {lo + stall:5.1f}..{hi + stall:4.1f}%")
    print(f"\n(Teensy: host time x{lo_k:.0f}..x{hi_k:.0f}, the calibrations; stall: each miss {T_MISS * 1e6:.2f} us)")


if __name__ == "__main__":
    main()
