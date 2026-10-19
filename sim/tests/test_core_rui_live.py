"""The remote UI's live pages through the core's host (core/build/libcore.dylib, sim/rui_live.py): SCOPE with the
synthetic Overbridge stream at every timebase stays within its frame rate and tile budget and draws the wave; DSP
SAMPLE's page loads its pool slot's overview and turns into GRAIN with MOD.

    make -C core engines lib && uv run python -m unittest sim/tests/test_core_rui_live.py
"""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
from rui_live import Live, card_with_samples, measure, pattern, report03  # noqa: E402
import protocol as P  # noqa: E402


def px(fb, x, y):
    return fb[(7 - y // 8) + 8 * x] >> (y % 8) & 1


class TestRuiLive(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        os.environ.setdefault("CORE_SAMPLE_RAM_MB", "16")
        cls.lv = Live()
        cls.lv.begin(card_with_samples())
        cls.audio = pattern(30.0)
        cls.at = 0

    def test_scope(self):
        lv = self.lv
        lv.open()
        lv.key(0x40)   # LEFT on SIDECAR: SCOPE, the last page
        self.assertEqual(lv.stats()[14], 3)
        at = lv.tick(500, self.audio)
        for tb in range(7):   # TIME from 21 ms (all the way left) to 1.4 s
            lv.turn(1, -10 if tb == 0 else 1)
            at = lv.tick(300, self.audio, at)
            bps, tps, fps, at = measure(lv, self.audio, 2.0, at)
            with self.subTest(tb=tb):
                self.assertLessEqual(fps, 15.5)
                self.assertGreaterEqual(fps, 6)
                self.assertLessEqual(tps, 900)
                fb = lv.frame()
                wave = sum(px(fb, x, y) for x in range(128) for y in range(11, 32))
                self.assertGreater(wave, 100)
        lv.key(0x45)   # NO: SIDECAR
        self.assertEqual(lv.stats()[14], 0)

    def test_wave(self):
        lv = self.lv
        m = P.pool_assign(1, 1, 0)
        lv.lib.core_c_sysex(m, len(m))
        p = [0, 64, 64, 64, 0, 64, 64, 100, 64, 64, 1, 0, 0, 120, 0, 100]
        report03(lv, 0, 0x20, p)
        lv.open()
        lv.key(0x41), lv.key(0x41)   # SIDECAR, SYSTEM, voice 1
        st = lv.stats()
        self.assertEqual(st[14:16], [1, 0])
        lv.tick(300)
        fb = lv.frame()
        wave = sum(px(fb, x, y) for x in range(1, 101) for y in range(11, 25))
        self.assertGreater(wave, 150)
        p[11] = 56   # MOD: GRAIN
        report03(lv, 0, 0x20, p)
        lv.tick(60)
        fb2 = lv.frame()
        self.assertNotEqual(fb, fb2)
        lv.key(0x45)


if __name__ == "__main__":
    unittest.main()
