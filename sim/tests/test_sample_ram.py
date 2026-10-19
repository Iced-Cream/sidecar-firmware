"""The sample RAM's allocator (sampler/sample_ram.h, the Teensy's code):
holes, LRU eviction, pins, fragmentation; and the fallback rule (which way a
DSP SAMPLE trig plays, sample_modes.h's ChoosePath).

    cd sim && sampler/build.sh && uv run python -m unittest discover tests
"""
import os
import random
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from samplemodes import MODES, PATH_RAM, PATH_SILENT, PATH_STREAM, SampleRam, choose_path  # noqa: E402

K = 1024
M = 1024 * K


def check(t, r, pinned=()):
    """Blocks inside the arena, aligned, not overlapping; pinned ones alive."""
    blocks = sorted(r.block(i) for i in t.live(r))
    end = 0
    for off, size in blocks:
        t.assertGreaterEqual(off, end)
        t.assertEqual(off % 4096, 0)
        end = off + size
    t.assertLessEqual(end, 16 * M)
    for i in pinned:
        t.assertIsNotNone(r.block(i))


class TestSampleRam(unittest.TestCase):
    def live(self, r):
        return [i for i in self.ids if r.block(i) is not None]

    def setUp(self):
        self.ids = []

    def alloc(self, r, key, size, now):
        i, ev = r.alloc(key, size, now)
        if i >= 0:
            self.ids.append(i)
        return i, ev

    def test_fill_and_round(self):
        r = SampleRam(16 * M)
        i, ev = self.alloc(r, 1, 1000, 0)
        self.assertEqual(r.block(i), (0, 4096))
        i2, _ = self.alloc(r, 2, 5000, 1)
        self.assertEqual(r.block(i2), (4096, 8192))
        self.assertEqual(r.find(2), i2)
        self.assertEqual(r.find(3), -1)
        self.assertEqual(r.used, 12288)
        self.assertEqual(r.alloc(9, 17 * M, 2)[0], -1, "larger than the arena")

    def test_lru_eviction(self):
        r = SampleRam(16 * M)
        a = [self.alloc(r, k, 4 * M, k)[0] for k in range(4)]   # full
        self.assertEqual(r.largest_hole, 0)
        r.touch(a[0], 10)   # 0 used recently: 1 is now the oldest
        i, ev = self.alloc(r, 7, 4 * M, 11)
        self.assertGreaterEqual(i, 0)
        self.assertEqual(ev, [1])
        self.assertIsNone(r.block(a[1]))
        self.assertEqual(r.evictions, 1)
        check(self, r)

    def test_prefetch_takes_holes_only(self):
        r = SampleRam(16 * M)
        [self.alloc(r, k, 4 * M, k) for k in range(3)]
        i, ev = r.alloc(10, 4 * M, 5, evict=False)
        self.assertGreaterEqual(i, 0)
        self.assertEqual(ev, [])
        self.assertEqual(r.alloc(11, 1 * M, 6, evict=False), (-1, []), "full: a prefetch evicts nothing")
        self.assertEqual(r.count, 4)

    def test_block_table_full(self):
        """160 blocks (the table's size): a want still loads, evicting the
        oldest; a prefetch does not."""
        r = SampleRam(16 * M)
        for k in range(160):
            self.assertGreaterEqual(self.alloc(r, k, 48 * K, k)[0], 0)
        self.assertEqual(r.alloc(500, 48 * K, 200, evict=False), (-1, []))
        i, ev = self.alloc(r, 501, 48 * K, 201)
        self.assertGreaterEqual(i, 0)
        self.assertEqual(ev, [0])
        check(self, r)

    def test_pinned_never_evicted(self):
        r = SampleRam(16 * M)
        a = [self.alloc(r, k, 4 * M, k)[0] for k in range(4)]
        for i in a:
            r.pin(i)
        self.assertEqual(r.alloc(9, 1 * M, 10)[0], -1, "all pinned: no room")
        r.unpin(a[2])
        i, ev = self.alloc(r, 9, 1 * M, 11)
        self.assertEqual(ev, [2])
        check(self, r, pinned=[a[0], a[1], a[3]])

    def test_smallest_hole_first(self):
        r = SampleRam(16 * M)
        a = [self.alloc(r, k, 2 * M, k)[0] for k in range(8)]
        r.free(a[1])                       # a 2 MB hole
        r.free(a[4]); r.free(a[5])         # a 4 MB hole
        i, ev = self.alloc(r, 20, 2 * M, 9)
        self.assertEqual(ev, [])
        self.assertEqual(r.block(i)[0], 2 * M, "the 2 MB hole, not the 4 MB one")

    def test_fragmentation(self):
        """8 MB free in 1 MB holes between 1 MB blocks: no hole fits 3 MB, so a
        window is evicted - the least recently used one - and nothing moves."""
        r = SampleRam(16 * M)
        a = [self.alloc(r, k, 1 * M, k)[0] for k in range(16)]
        for k in range(0, 16, 2):
            r.free(a[k])
        self.assertEqual(r.used, 8 * M)
        self.assertEqual(r.largest_hole, 1 * M)
        before = {k: r.block(a[k]) for k in range(1, 16, 2)}
        for k in range(1, 16, 2):
            r.touch(a[k], 100 + k)
        r.touch(a[9], 50)    # block 9 (at 9 MB) the oldest
        r.pin(a[1])
        i, ev = self.alloc(r, 99, 3 * M, 200)
        self.assertGreaterEqual(i, 0)
        self.assertEqual(ev, [9], "one 1 MB block evicted, the oldest, joining two holes")
        off, size = r.block(i)
        self.assertTrue(8 * M <= off and off + size <= 11 * M)
        for k, b in before.items():   # no block moved
            if k != 9:
                self.assertEqual(r.block(a[k]), b)
        check(self, r, pinned=[a[1]])

    def test_random_stress(self):
        """Random loads, touches, pins: the invariants hold, pinned blocks
        survive, and a load fails only when pins leave no window."""
        rnd = random.Random(5)
        r = SampleRam(16 * M)
        pinned = set()
        for now in range(3000):
            op = rnd.random()
            live = self.live(r)
            if op < 0.6 or not live:
                size = rnd.choice([48 * K, 200 * K, 1 * M, 3 * M, 6 * M])
                i, ev = self.alloc(r, 1000 + now, size, now)
                if i < 0:
                    # only when pins block every window
                    self.assertTrue(pinned or size > 16 * M)
                for k in ev:
                    self.assertEqual(r.find(k), -1)
            elif op < 0.8:
                r.touch(rnd.choice(live), now)
            elif op < 0.9:
                i = rnd.choice(live)
                if i in pinned:
                    r.unpin(i); pinned.discard(i)
                elif len(pinned) < 3:
                    r.pin(i); pinned.add(i)
            else:
                i = rnd.choice(live)
                if i not in pinned:
                    r.free(i)
            check(self, r, pinned=pinned)
        self.assertGreater(r.evictions, 100)


class TestFallback(unittest.TestCase):
    def test_no_psram_streams_always(self):
        """No PSRAM (today's Teensy): every trig streams as today, MOD ignored."""
        for mode in range(len(MODES)):
            for resident in (False, True):
                self.assertEqual(choose_path(False, resident, mode), PATH_STREAM)

    def test_psram(self):
        for mode in range(len(MODES)):
            self.assertEqual(choose_path(True, True, mode), PATH_RAM)
            want = PATH_STREAM if MODES[mode] in ("ONE", "LOOP") else PATH_SILENT
            self.assertEqual(choose_path(True, False, mode), want, MODES[mode])


if __name__ == "__main__":
    unittest.main()
