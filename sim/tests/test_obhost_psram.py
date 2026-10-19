"""The simulated host's DSP SAMPLE with and without --psram (obhost.Host):
without, MOD is ignored and the voice is today's (sample_voice.h) - the
Teensy's no-PSRAM fallback; with, MOD picks the playback mode from RAM.

    cd sim && sampler/build.sh && uv run python -m unittest discover tests
"""
import os
import sys
import unittest

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from obhost import Host  # noqa: E402
from samplemodes import MODES, mode_value  # noqa: E402

SR = 48000


def trig(host, v, note, sp, mix=0):
    """A DSP SAMPLE 0x04: P1..P8 ENG STR BRI DMP MIX FB TUN LEV, P9..P16 sp."""
    p = [0, 64, 64, 64, mix, 0, 64, 100] + list(sp)
    d = [0x20 | v, note, 100]
    for x in p:
        d += [int(x), 0]
    host.report(0x04, d)


def render(host, v, n):
    return np.concatenate([host.render_dsps(v, 12, np.zeros(12, np.float32)) for _ in range(n // 12)])


class TestHostPsram(unittest.TestCase):
    def host(self, psram):
        h = Host()
        t = np.arange(SR)
        pcm = (np.sin(2 * np.pi * 440 * t / SR) * 12000).astype(np.int16).reshape(-1, 1)
        h.pool = ["test/sine.wav"]
        h.lib.cache["test/sine.wav"] = (pcm, SR, "test/sine.wav")
        if psram:
            h.use_psram()
        return h

    def sp(self, mode, lop=0):
        return [64, 64, 1, mode_value(MODES.index(mode)), 0, 120, lop, 100]   # SMP 1

    def test_fallback_ignores_mod(self):
        """Without PSRAM every MOD plays as today's voice, sample for sample."""
        ref = None
        for mode in MODES:
            h = self.host(False)
            trig(h, 0, 60, self.sp(mode))
            o = render(h, 0, 4800)
            self.assertFalse(h.from_ram[0])
            if ref is None:
                ref = o
            np.testing.assert_array_equal(o, ref, err_msg=mode)
        self.assertGreater(np.max(np.abs(ref)), 0.2)

    def test_psram_modes(self):
        h0 = self.host(False)
        trig(h0, 0, 60, self.sp("ONE"))
        ref = render(h0, 0, 4800)
        h = self.host(True)
        trig(h, 0, 60, self.sp("ONE"))
        self.assertTrue(h.from_ram[0])
        o = render(h, 0, 4800)
        self.assertLess(np.max(np.abs(o[:100] - ref[:100])), 1e-4, "ONE from RAM: today's sound")
        # SLICE (8 slices, LOP 56): note 61 plays slice 1 alone, then silence
        trig(h, 0, 61, self.sp("SLICE", lop=56))
        o = render(h, 0, 9600)
        self.assertGreater(np.max(np.abs(o[:5900])), 0.2)
        self.assertEqual(float(np.max(np.abs(o[6100:]))), 0.0)
        # GRAIN plays until the note-off, then fades
        trig(h, 0, 60, self.sp("GRAIN", lop=80))
        render(h, 0, 4800)
        self.assertTrue(h.ram[0].playing)
        h.note_off(0)
        render(h, 0, 1200)
        self.assertFalse(h.ram[0].playing)

    def test_psram_empty_slot_silent(self):
        h = self.host(True)
        sp = self.sp("GRAIN")
        sp[2] = 0   # SMP 0: off
        trig(h, 0, 60, sp)
        self.assertFalse(h.from_ram[0])
        self.assertEqual(float(np.max(np.abs(render(h, 0, 1200)))), 0.0)


if __name__ == "__main__":
    unittest.main()
