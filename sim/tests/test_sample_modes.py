"""DSP SAMPLE's playback modes (sampler/sample_modes.h, the Teensy's code):
output sanity per mode - pitch, slice selection, reverse, loop points - and
ONE against today's voice (sampler/sample_voice.h).

    cd sim && sampler/build.sh && uv run python -m unittest discover tests
"""
import os
import sys
import unittest

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from samplemodes import MODES, ModeVoice, mode_value, to_mono16  # noqa: E402

SR = 48000


def sine(freq, secs=1.0, rate=SR, amp=16000):
    t = np.arange(int(secs * rate))
    return (np.sin(2 * np.pi * freq * t / rate) * amp).astype(np.int16).reshape(-1, 1)


def ramp(n=48000):
    """0 .. 30000 rising: a frame's value tells where in the sample it is."""
    return np.linspace(0, 30000, n).astype(np.int16).reshape(-1, 1)


def page(mode, tun=64, fin=64, sta=0, end=120, lop=0, lev=100):
    """The SD SAMPLE page, P9..P16: TUN FIN SMP MOD STA END LOP LEV."""
    return [tun, fin, 1, mode_value(MODES.index(mode)), sta, end, lop, lev]


def freq_of(x, rate=SR):
    """The dominant frequency (FFT peak, parabolic interpolation)."""
    x = x - x.mean()
    w = np.hanning(len(x))
    s = np.abs(np.fft.rfft(x * w))
    k = int(np.argmax(s[1:])) + 1
    a, b, c = np.log(s[k - 1] + 1e-12), np.log(s[k] + 1e-12), np.log(s[k + 1] + 1e-12)
    p = 0.5 * (a - c) / (a - 2 * b + c)
    return (k + p) * rate / len(x)


def freq_ac(x, rate=SR, fmin=40.0):
    """The fundamental by autocorrelation (the first peak past a lag of
    rate/2000; a plucked string's harmonics can outweigh its fundamental)."""
    x = x - x.mean()
    n = len(x)
    s = np.fft.rfft(x, 2 * n)
    ac = np.fft.irfft(s * np.conj(s))[:n]
    lo, hi = int(rate / 2000), int(rate / fmin)
    k = lo + int(np.argmax(ac[lo:hi]))
    a, b, c = ac[k - 1], ac[k], ac[k + 1]
    return rate / (k + 0.5 * (a - c) / (a - 2 * b + c))


def play(pcm, rate, sp, note=60, n=SR, seed=1):
    v = ModeVoice(seed)
    v.set(to_mono16(pcm), rate)
    v.trigger(note, sp)
    return v.render(n, sp), v


class TestModes(unittest.TestCase):
    def test_mode_zones(self):
        for i, m in enumerate(MODES):
            _, v = play(sine(440), SR, page(m), n=12)
            self.assertEqual(v.mode, i, m)

    def test_one_pitch_and_rate(self):
        for rate in (48000, 44100):
            for tun, ratio in ((64, 1.0), (76, 2.0), (52, 0.5)):
                o, _ = play(sine(440, rate=rate), rate, page("ONE", tun=tun), n=8192)
                self.assertAlmostEqual(freq_of(o), 440 * ratio, delta=440 * ratio * 0.005, msg=(rate, tun))
        # the note transposes: 72 an octave up
        o, _ = play(sine(440), SR, page("ONE"), note=72, n=8192)
        self.assertAlmostEqual(freq_of(o), 880, delta=5)

    def test_one_matches_todays_voice(self):
        """ONE plays as sample_voice.h does (the simulator's voice today): the
        same frames, the same end. sample_voice.h keeps its position in a
        float, which drifts over thousands of steps (the Teensy's stream_voice
        keeps frame + fraction, as precise as the double here), so it is
        compared over its first 100 samples; the whole output against the
        exact interpolation."""
        from samplevoice import SampleVoice   # sampler/sample_voice.h via libdaisy
        pcm = sine(330, 0.5)
        x = pcm[:, 0].astype(np.float64) / 32768
        for sp in (page("ONE"), page("ONE", tun=70, sta=10, end=90), page("ONE", sta=100, end=20),
                   page("ONE", sta=30, end=40, lop=1)):
            old = SampleVoice()
            old.set((pcm, SR, "x"))
            old.trigger()
            semis = (sp[0] - 64) + (sp[1] - 64) / 64
            ref = np.concatenate([old.render(12, semis, sp[4] / 120, sp[5] / 120, sp[6] >= 1, sp[7] / 100)
                                  for _ in range(2000)])
            o, _ = play(pcm, SR, sp, n=24000)
            self.assertLess(np.max(np.abs(o[:100] - ref[:100])), 1e-4, sp)
            if sp[6] < 1:   # one shot: it ends where today's ends
                self.assertLessEqual(abs(np.count_nonzero(o) - np.count_nonzero(ref)), 2, sp)
                a, b = sp[4] / 120 * (len(x) - 1), sp[5] / 120 * (len(x) - 1)
                step = 2 ** (semis / 12) * (1 if b >= a else -1)
                k = np.count_nonzero(o)
                ideal = np.interp(a + step * np.arange(k), np.arange(len(x)), x)
                self.assertLess(np.max(np.abs(o[:k] - ideal)), 1e-3, sp)

    def test_one_reverse(self):
        o, v = play(ramp(), SR, page("ONE", sta=120, end=0), n=SR + 100)
        x = o[:SR - 10]
        self.assertTrue(np.all(np.diff(x) <= 1e-6), "reversed: falling")
        self.assertGreater(x[0], 0.9)
        self.assertFalse(v.playing)

    def test_loop_points(self):
        # STA 30 .. END 60 of 120: the ramp's values stay in that stretch, wrapping
        o, v = play(ramp(), SR, page("LOOP", sta=30, end=60), n=3 * SR - 100)
        lo, hi = 30000 * 30 / 120 / 32768, 30000 * 60 / 120 / 32768
        self.assertTrue(v.playing)
        self.assertGreater(o.min(), lo - 1e-3)
        self.assertLess(o.max(), hi + 1e-3)
        wraps = np.sum(np.diff(o) < -0.1)
        self.assertEqual(wraps, 11, "12 000 frames a lap: 12 laps in 144 000 samples")
        # ping-pong: no jumps, turning at both ends
        o, _ = play(ramp(), SR, page("LOOP", sta=30, end=60, lop=64), n=3 * SR)
        self.assertLess(np.max(np.abs(np.diff(o))), 1e-3)
        self.assertGreater(np.sum(np.diff(np.sign(np.diff(o[::50]))) != 0), 8)
        # reversed loop: falling between the jumps
        o, _ = play(ramp(), SR, page("LOOP", sta=60, end=30), n=SR)
        d = np.diff(o)
        self.assertGreater(np.sum(d <= 0), len(d) - 20)

    def test_loop_step_longer_than_loop(self):
        """A 4-frame loop at +36 semitones: the step (8 frames) jumps the
        loop; the output stays a sample's values, never runaway."""
        pcm = ramp(1000)
        o, v = play(pcm, SR, page("LOOP", tun=100, sta=60, end=60.5), n=4800)
        self.assertTrue(v.playing)
        self.assertLessEqual(float(np.max(np.abs(o))), 30000 / 32768 + 1e-3)
        o, v = play(pcm, SR, page("LOOP", tun=100, sta=60.5, end=60), n=4800)
        self.assertLessEqual(float(np.max(np.abs(o))), 30000 / 32768 + 1e-3)

    def test_loop_release(self):
        sp = page("LOOP", sta=0, end=60)
        o, v = play(sine(440), SR, sp, n=4800)
        v.note_off()
        tail = v.render(1200, sp)
        self.assertFalse(v.playing)
        self.assertLess(np.max(np.abs(tail[600:])), 1e-6)

    def test_slice_by_note(self):
        n = 48000
        for note, k in ((60, 0), (62, 2), (67, 7), (68, 0), (59, 7)):
            o, v = play(ramp(n), SR, page("SLICE", lop=56), note=note, n=n // 8 + 100)   # LOP 56: 8 slices
            x = o[200:n // 8 - 200]
            lo, hi = 30000 * k / 8 / 32768, 30000 * (k + 1) / 8 / 32768
            self.assertGreater(x.min(), lo - 1e-3, (note, k))
            self.assertLess(x.max(), hi + 1e-3, (note, k))
            self.assertFalse(v.playing, "one slice, then silent")
        # reversed: the same slice, falling
        o, _ = play(ramp(n), SR, page("SLICE", sta=120, end=0, lop=56), note=62, n=n // 8)
        x = o[200:n // 8 - 200]
        self.assertTrue(np.all(np.diff(x) <= 1e-6))
        self.assertGreater(x.min(), 30000 * 2 / 8 / 32768 - 1e-3)
        self.assertLess(x.max(), 30000 * 3 / 8 / 32768 + 1e-3)
        # the fades: no click at either end
        o, _ = play(sine(440), SR, page("SLICE", lop=56), note=61, n=n // 8 + 50)
        self.assertLess(abs(o[0]), 0.02)
        self.assertLess(np.max(np.abs(o[n // 8 - 5:])), 0.03)

    def test_grain_position_and_pitch(self):
        # four DC levels: STA picks which the cloud plays
        levels = [4000, 8000, 16000, 24000]
        pcm = np.repeat(np.array(levels, np.int16), 24000).reshape(-1, 1)
        means = []
        for k in range(4):
            o, v = play(pcm, SR, page("GRAIN", sta=(k + 0.5) / 4 * 120, end=30, lop=60), n=SR)
            self.assertTrue(v.playing, "a cloud plays until the note-off")
            means.append(float(np.mean(o[SR // 2:])))
        # the cloud's level tracks the level at STA (normalised to ~1x by the overlap)
        self.assertTrue(all(means[j] < means[j + 1] for j in range(3)), means)
        ratios = [m / (lev / 32768) for m, lev in zip(means, levels)]
        self.assertLess(max(ratios) / min(ratios), 1.3, ratios)
        # pitch: a sine's grains keep its pitch, TUN +12 doubles it (random
        # grain phases smear the peak a little: 2 %)
        for tun, f in ((64, 440), (76, 880)):
            o, _ = play(sine(440, 2.0), SR, page("GRAIN", sta=60, end=80, lop=60, tun=tun), n=SR)
            self.assertAlmostEqual(freq_of(o[SR // 4:]), f, delta=f * 0.02)
        # density: about LOP's grains a second
        _, v = play(sine(440), SR, page("GRAIN", end=10, lop=0), n=SR)
        self.assertTrue(1 <= v.ops()[1] <= 4, v.ops())
        _, v = play(sine(440), SR, page("GRAIN", end=10, lop=127), n=SR)
        self.assertGreater(v.ops()[1], 200)

    def test_grain_budget(self):
        """8 dense clouds share 32 grains (the CPU budget); each still sounds,
        and the count comes back to 0 when they stop."""
        from samplemodes import active_grains
        sp = page("GRAIN", sta=60, end=90, lop=127)
        vs = []
        for i in range(8):
            v = ModeVoice(i + 1)
            v.set(to_mono16(sine(440, 2.0)), SR)
            v.trigger(60, sp)
            vs.append(v)
        peak = 0
        outs = [[] for _ in vs]
        for _ in range(400):   # 0.1 s, block by block, as the Teensy runs them
            for k, v in enumerate(vs):
                outs[k].append(v.render(12, sp))
            peak = max(peak, active_grains())
        self.assertEqual(peak, 32)
        for o in outs:
            self.assertGreater(np.max(np.abs(np.concatenate(o)[2400:])), 0.05)
        for v in vs:
            v.note_off()
            v.render(600, sp)
        self.assertEqual(active_grains(), 0)

    def test_grain_release(self):
        sp = page("GRAIN", sta=60, end=40, lop=80)
        o, v = play(sine(440), SR, sp, n=4800)
        v.note_off()
        v.render(600, sp)
        self.assertFalse(v.playing)

    def test_stretch_time_and_pitch(self):
        pcm = sine(440, 1.0)
        for lop, secs in ((64, 1.0), (32, 2.0), (127, 0.504)):
            o, v = play(pcm, SR, page("STRCH", lop=lop), n=int(2.5 * SR))
            last = np.nonzero(np.abs(o) > 1e-4)[0][-1] / SR
            self.assertAlmostEqual(last, secs, delta=0.06, msg=lop)
            self.assertFalse(v.playing)
            self.assertAlmostEqual(freq_of(o[: int(secs * SR * 0.8)]), 440, delta=4)
        # the pitch apart from the time: TUN +12, same length
        o, _ = play(pcm, SR, page("STRCH", lop=64, tun=76), n=int(1.5 * SR))
        self.assertAlmostEqual(freq_of(o[: int(0.8 * SR)]), 880, delta=8)
        last = np.nonzero(np.abs(o) > 1e-4)[0][-1] / SR
        self.assertAlmostEqual(last, 1.0, delta=0.06)
        # frozen: LOP 0 keeps sounding
        _, v = play(pcm, SR, page("STRCH", lop=0), n=3 * SR)
        self.assertTrue(v.playing)
        # reversed: the ramp falls on the whole
        o, _ = play(ramp(), SR, page("STRCH", sta=120, end=0, lop=64), n=SR)
        self.assertGreater(np.mean(o[:SR // 4]), np.mean(o[SR // 2:3 * SR // 4]))

    def test_pluck_pitch_and_decay(self):
        noise = (np.random.default_rng(1).uniform(-1, 1, 4800) * 20000).astype(np.int16).reshape(-1, 1)
        for note, f in ((69, 440.0), (57, 220.0), (45, 110.0)):
            o, v = play(noise, SR, page("PLUCK", end=100, lop=100), note=note, n=SR)
            self.assertAlmostEqual(freq_ac(o[2400:24000]), f, delta=f * 0.01, msg=note)
        # END: a short decay dies sooner; it stops by itself
        o_short, v = play(noise, SR, page("PLUCK", end=10, lop=100), note=69, n=SR)
        o_long, _ = play(noise, SR, page("PLUCK", end=110, lop=100), note=69, n=SR)
        e = lambda x: float(np.sqrt(np.mean(x[SR // 2:] ** 2)))
        self.assertLess(e(o_short), 0.1 * e(o_long))
        self.assertFalse(v.playing)

    def test_wave_pitch_and_scan(self):
        w = 256
        cyc0 = np.sin(2 * np.pi * np.arange(w) / w) * 16000          # sine
        cyc1 = np.where(np.arange(w) < w // 2, 16000.0, -16000.0)     # square
        pcm = np.concatenate([cyc0, cyc1]).astype(np.int16).reshape(-1, 1)
        o, _ = play(pcm, SR, page("WAVE", sta=0, lop=0), note=60, n=SR)   # LOP 0: 256-frame cycles
        self.assertAlmostEqual(freq_of(o), 261.63, delta=1.0)
        o, _ = play(pcm, SR, page("WAVE", sta=0, lop=0), note=72, n=SR)
        self.assertAlmostEqual(freq_of(o), 523.25, delta=2.0)
        # STA scans: the sine is pure, the square has a third harmonic
        def h3(x):
            s = np.abs(np.fft.rfft(x * np.hanning(len(x))))
            k = int(round(261.63 * len(x) / SR))
            return s[3 * k - 3:3 * k + 4].max() / s[k - 3:k + 4].max()
        s0, _ = play(pcm, SR, page("WAVE", sta=0, lop=0), n=SR)
        s1, _ = play(pcm, SR, page("WAVE", sta=120, lop=0), n=SR)
        sm, _ = play(pcm, SR, page("WAVE", sta=60, lop=0), n=SR)
        self.assertLess(h3(s0), 0.01)
        self.assertAlmostEqual(h3(s1), 1 / 3, delta=0.03)
        self.assertTrue(h3(s0) < h3(sm) < h3(s1))

    def test_silent_without_sample(self):
        v = ModeVoice()
        v.set(None, SR)
        for m in MODES:
            v.trigger(60, page(m))
            self.assertFalse(v.playing)
            self.assertEqual(float(np.abs(v.render(48, page(m))).max()), 0.0)


if __name__ == "__main__":
    unittest.main()
