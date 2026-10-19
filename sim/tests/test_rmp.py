# sim/tests/test_rmp.py - a plugin's native build through the harness.
import ctypes, glob, math, os, re, subprocess, sys, unittest
import numpy as np
import rmpvoice

HERE = os.path.dirname(os.path.abspath(__file__))
PLUG = os.path.join(HERE, "..", "..", "plugins", "sine_test")

def pitch(x, sr=48000.0):
    z = np.where(np.diff(np.signbit(x)))[0]
    return sr / (2 * np.mean(np.diff(z))) if len(z) > 4 else 0.0

class SineTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(["make", "-C", PLUG, "libsine_test.dylib"], check=True)
        cls.lib = os.path.join(PLUG, "libsine_test.dylib")

    def test_pitch_and_tune(self):
        v = rmpvoice.RmpVoice(self.lib)
        p = [64.0] * 16; p[1] = 127.0; p[2] = 127.0
        v.params(p); v.note_on(69.0, 1.0)
        self.assertAlmostEqual(pitch(v.render(4800)), 440.0, delta=2.0)
        p[0] = 64.0 + 64.0 / 12.0          # +1 semitone
        v.params(p); v.note_on(69.0, 1.0)
        self.assertAlmostEqual(pitch(v.render(4800)), 440.0 * 2 ** (1 / 12), delta=2.0)

    def test_note_off_releases(self):
        v = rmpvoice.RmpVoice(self.lib)
        p = [64.0] * 16; p[1] = 127.0; p[2] = 127.0
        v.params(p); v.note_on(60.0, 1.0)
        self.assertGreater(np.max(np.abs(v.render(480))), 0.1)   # sounding before the note-off
        v.note_off()
        tail = v.render(48000)[-480:]
        self.assertLess(np.max(np.abs(tail)), 1e-3)

    def test_render_not_multiple_of_block(self):
        v = rmpvoice.RmpVoice(self.lib)
        p = [64.0] * 16; p[1] = 127.0; p[2] = 127.0
        v.params(p); v.note_on(69.0, 1.0)
        out = v.render(4805)                # 4805 = 400 * 12 + 5
        self.assertEqual(len(out), 4805)
        self.assertGreater(np.max(np.abs(out[-5:])), 0.0)

    def test_two_voices_share_one_plugin(self):
        a = rmpvoice.RmpVoice(self.lib)
        b = rmpvoice.RmpVoice(self.lib)
        p = [64.0] * 16; p[1] = 127.0; p[2] = 127.0
        a.params(p); b.params(p)
        a.note_on(69.0, 1.0); b.note_on(57.0, 1.0)
        xa, xb = a.render(4800), b.render(4800)
        self.assertAlmostEqual(pitch(xa), 440.0, delta=2.0)
        self.assertAlmostEqual(pitch(xb), 220.0, delta=2.0)
        del a                                  # the plugin stays loaded for b
        b.note_on(69.0, 1.0)
        self.assertAlmostEqual(pitch(b.render(4800)), 440.0, delta=2.0)

    def test_bad_path_raises(self):
        with self.assertRaises(RuntimeError):
            rmpvoice.RmpVoice("/nonexistent/plugin.dylib")

NP = os.path.join(HERE, "..", "..", "plugins", "noiseplethora")


def toolchain_skip(why):
    """A test that needs Teensyduino (its ARM compiler, its Audio library) skips without it - or fails with
    RMP_REQUIRE_TOOLCHAIN=1 (a machine that must run every test)."""
    if os.environ.get("RMP_REQUIRE_TOOLCHAIN") == "1":
        raise AssertionError(f"RMP_REQUIRE_TOOLCHAIN=1: {why}")
    raise unittest.SkipTest(why)


class NoisePlethoraSingle(unittest.TestCase):
    """Noise Plethora's Atari (GPL, fetched by plugins/noiseplethora/fetch.sh) through the Teensy Audio shim."""
    @classmethod
    def setUpClass(cls):
        if not os.path.isdir(os.path.join(NP, "vendor", "np")):
            raise unittest.SkipTest("run plugins/noiseplethora/fetch.sh first")
        if not glob.glob(os.path.expanduser("~/Library/Arduino15/packages/teensy/hardware/avr/*/libraries/Audio")):
            toolchain_skip("Teensyduino's Audio library is not installed")
        subprocess.run(["make", "-C", NP, "libnp_atari.dylib"], check=True)
        cls.lib = os.path.join(NP, "libnp_atari.dylib")
        cls.dbg = ctypes.CDLL(cls.lib)      # the same image the harness loads: its debug counters

    def tearDown(self):
        # no voice's graph ever ran out of blocks (summed over every render of the test)
        self.assertEqual(ctypes.c_uint32.in_dll(self.dbg, "np_debug_alloc_failed").value, 0)
        self.assertGreater(ctypes.c_uint32.in_dll(self.dbg, "np_debug_blocks_max").value, 0)

    @staticmethod
    def zc(x):
        return np.count_nonzero(np.diff(np.signbit(x)))

    def test_atari_makes_sound_and_follows_x(self):
        v = rmpvoice.RmpVoice(self.lib)
        p = [64.0] * 16; p[1] = 20.0; p[2] = 40.0; p[7] = 127.0
        v.params(p); v.note_on(60.0, 1.0)
        a = v.render(48000)
        self.assertGreater(float(np.sqrt(np.mean(a[4800:] ** 2))), 0.01)     # it sounds
        self.assertFalse(np.any(np.isnan(a)))
        p[1] = 120.0; v.params(p)
        b = v.render(48000)
        self.assertFalse(np.any(np.isnan(b)))
        # X sets waveformMod1's frequency (10 + 50 x X^2 Hz), the modulator of the output
        # oscillator waveformMod2 (whose base frequency, 10 + 200 x Y Hz, comes from Y): with
        # more modulation the output's zero-crossing rate rises
        self.assertGreater(self.zc(b[4800:]), self.zc(a[4800:]))

    def test_two_voices_are_independent(self):
        p = [64.0] * 16; p[2] = 40.0; p[7] = 127.0
        solo = rmpvoice.RmpVoice(self.lib)
        p[1] = 20.0; solo.params(p); solo.note_on(60.0, 1.0)
        ref = solo.render(24000)
        a, b = rmpvoice.RmpVoice(self.lib), rmpvoice.RmpVoice(self.lib)
        p[1] = 20.0; a.params(p); a.note_on(60.0, 1.0)
        p[1] = 120.0; b.params(p); b.note_on(60.0, 1.0)
        xa, xb = [], []
        for _ in range(2000):                      # interleaved, 12 samples at a time as voiceloop does
            xa.append(a.render(12)); xb.append(b.render(12))
        xa, xb = np.concatenate(xa), np.concatenate(xb)
        np.testing.assert_array_equal(xa, ref)     # b's graph never touched a's
        self.assertGreater(self.zc(xb), self.zc(xa))
        self.assertGreater(float(np.sqrt(np.mean(xb ** 2))), 0.01)

class NoisePlethoraPackages(unittest.TestCase):
    """NP's 21 sound generators as the packages of plugins/noiseplethora/banks.h (the small ones and A, B, C): P1 ALG, P2 X, P3 Y, P8 LEV."""
    # algorithms that use random() or a noise object: their output depends on the voice's seed
    RANDOM = {"PrimeCluster", "PrimeCnoise", "FibonacciCluster", "partialCluster", "basurilla", "resonoise",
              "WalkingFilomena", "arrayOnTheRocks", "existencelsPain"}

    @classmethod
    def banks(cls):
        """{pkg: [(class, gain, name)]} from banks.h's X-macro lines."""
        out = {}
        with open(os.path.join(NP, "banks.h")) as f:
            for line in f:
                m = re.match(r"#define NP_PKG_([A-Z0-9]+)\(X\) (.*)", line)
                if m:
                    out[m.group(1).lower()] = re.findall(r'X\((\w+), ([\d.]+)f, "([^"]*)"\)', m.group(2))
        return out

    @classmethod
    def setUpClass(cls):
        if not os.path.isdir(os.path.join(NP, "vendor", "np")):
            raise unittest.SkipTest("run plugins/noiseplethora/fetch.sh first")
        if not glob.glob(os.path.expanduser("~/Library/Arduino15/packages/teensy/hardware/avr/*/libraries/Audio")):
            toolchain_skip("Teensyduino's Audio library is not installed")
        cls.algs = cls.banks()
        cls.ALGS = {k: len(v) for k, v in cls.algs.items()}
        cls.lib, cls.dbg = {}, {}
        for k in cls.ALGS:
            subprocess.run(["make", "-C", NP, f"PKG={k}", f"libnp_{k}.dylib"], check=True)
            cls.lib[k] = os.path.join(NP, f"libnp_{k}.dylib")
            cls.dbg[k] = ctypes.CDLL(cls.lib[k])

    def tearDown(self):
        for k, d in self.dbg.items():       # no voice's graph ever ran out of blocks
            with self.subTest(package=k):
                self.assertEqual(ctypes.c_uint32.in_dll(d, "np_debug_alloc_failed").value, 0)

    def zone(self, k, alg):                 # the middle of ALG's zone (NP_N equal zones over 0..127)
        return float(int((alg + 0.5) * 128 / self.ALGS[k]))

    def voice(self, k, alg, x=64.0, y=64.0):
        v = rmpvoice.RmpVoice(self.lib[k])
        p = [64.0] * 16; p[0] = self.zone(k, alg); p[1] = x; p[2] = y; p[7] = 127.0
        v.params(p); v.note_on(60.0, 1.0)
        return v, p

    def test_manifest_matches_banks_h(self):
        # the .rmp's ALG names are banks.h's, in its order, one per algorithm (NP_N)
        sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
        import rmp_pack
        for k, algs in self.algs.items():
            with self.subTest(package=k):
                r = subprocess.run(["make", "-C", NP, f"PKG={k}", f"np_{k}.rmp"], capture_output=True, text=True)
                if r.returncode:
                    toolchain_skip("no ARM toolchain to build the package: " + r.stderr[-200:])
                with open(os.path.join(NP, f"np_{k}.rmp"), "rb") as f:
                    m = rmp_pack.parse_manifest(f.read())
                alg = [kn for kn in m["knobs"] if kn["p"] == 1][0]
                self.assertEqual(alg["names"], [n for _, _, n in algs])
                self.assertEqual(alg["enum_count"], len(algs))

    def test_small_packages_hold_every_algorithm_once(self):
        # the small packages (make packages) together are A + B + C, each algorithm once, same gain and name
        legacy = [x for k in "abc" for x in self.algs[k]]
        small = [x for k, v in self.algs.items() if k not in "abc" for x in v]
        self.assertEqual(sorted(small), sorted(legacy))
        self.assertEqual(len(set(c for c, _, _ in small)), 21)

    def test_every_algorithm_sounds_without_nan(self):
        # all 21 sound at X = Y = 0.5 (none is silent there by design); each ALG zone gives its own output
        for k, n in self.ALGS.items():
            outs = []
            for alg in range(n):
                v, _ = self.voice(k, alg)
                a = v.render(24000)
                outs.append(a)
                with self.subTest(package=k, alg=alg):
                    self.assertFalse(np.any(np.isnan(a)))
                    self.assertGreater(float(np.sqrt(np.mean(a[12000:] ** 2))), 0.01)   # RMS, last half
                    self.assertLessEqual(float(np.max(np.abs(a))), 1.0)
            for i in range(n):
                for j in range(i + 1, n):
                    with self.subTest(package=k, pair=(i, j)):
                        self.assertFalse(np.array_equal(outs[i], outs[j]))

    def test_swap_equals_fresh_voice(self):
        # after an ALG change the voice plays exactly what a fresh voice of that algorithm plays
        # (algorithms without random()/noise: those depend on the voice's seed)
        for k, algs in self.algs.items():
            n = len(algs)
            for alg in range(n):
                if algs[alg][0] in self.RANDOM:
                    continue
                prev = (alg + 1) % n
                v, p = self.voice(k, prev, 30.0, 90.0)
                v.render(4805)                       # mid-block, the old graph full of blocks
                p[0] = self.zone(k, alg); p[1] = 70.0; p[2] = 20.0
                v.params(p)
                got = v.render(12000)
                fresh, _ = self.voice(k, alg, 70.0, 20.0)
                with self.subTest(package=k, alg=algs[alg][0]):
                    np.testing.assert_array_equal(got, fresh.render(12000))

    def test_alg_param_nan_is_zone_0(self):
        v, p = self.voice("b", 3)
        p[0] = float("nan"); v.params(p)
        ref, _ = self.voice("b", 0)
        np.testing.assert_array_equal(v.render(4800), ref.render(4800))

    def test_two_voices_are_independent(self):
        # B's clusterSaw (ALG index 1; no random()) alone, then interleaved with a voice on another algorithm
        p = [64.0] * 16; p[0] = self.zone("b", 1); p[1] = 20.0; p[7] = 127.0
        solo = rmpvoice.RmpVoice(self.lib["b"]); solo.params(p); solo.note_on(60.0, 1.0)
        ref = solo.render(24000)
        a, b = rmpvoice.RmpVoice(self.lib["b"]), rmpvoice.RmpVoice(self.lib["b"])
        a.params(p); a.note_on(60.0, 1.0)
        q = list(p); q[0] = self.zone("b", 3); q[1] = 120.0; b.params(q); b.note_on(60.0, 1.0)
        xa, xb = [], []
        for _ in range(2000):
            xa.append(a.render(12)); xb.append(b.render(12))
        np.testing.assert_array_equal(np.concatenate(xa), ref)
        self.assertGreater(float(np.max(np.abs(np.concatenate(xb)))), 1e-3)

    def test_alg_sweep(self):
        # ALG changes every 0.1 s for 5 s (every algorithm, random X/Y): no NaN, no allocation failure
        rng = np.random.default_rng(7)
        for k, n in self.ALGS.items():
            v = rmpvoice.RmpVoice(self.lib[k])
            p = [64.0] * 16; p[7] = 127.0
            v.note_on(60.0, 1.0)
            out = []
            for i in range(50):
                p[0] = self.zone(k, (i * 7 + i // n) % n) if i % 5 else float(rng.integers(0, 128))
                p[1], p[2] = (float(x) for x in rng.uniform(0, 127, 2))
                v.params(p)
                out.append(v.render(4800))
            out = np.concatenate(out)
            with self.subTest(package=k):
                self.assertFalse(np.any(np.isnan(out)))
                self.assertGreater(float(np.sqrt(np.mean(out ** 2))), 0.01)

V2 = os.path.join(HERE, "..", "..", "plugins", "v2_test")
ROLES = [0] * 16
ROLES[1] = ROLES[2] = 1   # LEV, TON: role preset (plugins/v2_test/manifest.json)
ROLES[3] = 2              # DET: role offset

class V2Test(unittest.TestCase):
    """Plugin API 2 through the harness: enter/leave, native and generic presets, p-locks, the pool."""
    @classmethod
    def setUpClass(cls):
        subprocess.run(["make", "-C", V2, "libv2_test.dylib"], check=True)
        subprocess.run(["make", "-C", PLUG, "libsine_test.dylib"], check=True)
        cls.lib = os.path.join(V2, "libv2_test.dylib")
        cls.dl = ctypes.CDLL(cls.lib)          # the image the harness loads: v2_leaves

    def setUp(self):
        rmpvoice.pool_clear()

    @staticmethod
    def p(pre=0, lev=100, smp=0, mode=0, jump=0):
        p = [0.0] * 16
        p[0], p[1], p[3], p[4], p[5], p[6], p[7] = pre, lev, 64, smp, mode, jump, 127
        return [float(x) for x in p]

    def test_v1_plugin_unchanged(self):
        rmpvoice.call_log()
        v = rmpvoice.RmpVoice(os.path.join(PLUG, "libsine_test.dylib"))
        self.assertEqual(v.api(), 1)
        self.assertEqual(rmpvoice.call_log(), "I")                # voice_init only
        self.assertEqual(v.calls(), (0, 0))                       # no voice_enter for an API-1 plugin
        self.assertEqual(v.load_entry(b"V2P 1 2 3")[0], -1000)    # it has no load_entry
        p = [64.0] * 16; p[1] = 127.0; p[2] = 127.0
        v.params(p); v.note_on(69.0, 1.0)
        self.assertAlmostEqual(pitch(v.render(4800)), 440.0, delta=2.0)

    def test_enter_and_leave(self):
        leaves = ctypes.c_uint32.in_dll(self.dl, "v2_leaves")
        before = leaves.value
        rmpvoice.call_log()
        v = rmpvoice.RmpVoice(self.lib)
        self.assertEqual((v.api(), v.calls()), (2, (1, 0)))
        self.assertEqual(rmpvoice.call_log(), "IE")               # voice_init, then voice_enter
        del v
        self.assertEqual(leaves.value, before + 1)
        self.assertEqual(rmpvoice.call_log(), "LC")               # voice_leave, then the state freed

    def test_first_trig_does_not_write_back(self):
        v = rmpvoice.RmpVoice(self.lib)
        p = self.p(pre=1)                                         # the voice's first report is a trig: PRE may be p-locked
        self.assertEqual(v.preset_decide(0, 3, True, True, p), (1, False))
        self.assertEqual(v.preset_loaded(1, False, rmpvoice.preset_parse("P2 40"), ROLES, 0, p), [])   # played, not written back
        self.assertEqual(v.preset_apply(p, True, 0)[1], 40.0)
        self.assertEqual(v.preset_decide(0, 3, False, False, p), (1, True))   # the next knob report syncs
        self.assertEqual(v.preset_loaded(1, True, rmpvoice.preset_parse("P2 40"), ROLES, 10, p), [(1, 40)])

    def test_native_preset(self):
        v = rmpvoice.RmpVoice(self.lib)
        r, vals = v.load_entry(b"V2P 90 40 200")               # LEV 90, TON 40, +100 cents
        self.assertEqual((r, vals[1], vals[2], vals[0], vals[3]), (0, 90, 40, -1, -1))
        p = self.p(lev=90)
        v.params(p); v.note_on(69.0, 1.0)
        self.assertAlmostEqual(pitch(v.render(9600)), 440.0 * 2 ** (1 / 12), delta=2.0)
        self.assertEqual(v.load_entry(b"XYZ")[0], 1)            # not its format: refused, nothing changed

    def test_generic_preset_written_back_and_held(self):
        vals = rmpvoice.preset_parse("P2 30\nP3 99\nP4 10\n")    # P4 is an offset knob: not the preset's
        self.assertEqual(vals[1:4], [30, 99, 10])
        v = rmpvoice.RmpVoice(self.lib)
        p = self.p(pre=0)
        self.assertEqual(v.preset_decide(0, 3, False, True, p), (0, True))  # the voice's first report, a knob report
        self.assertEqual(v.preset_loaded(0, True, vals, ROLES, 1000, p), [(1, 30), (2, 99)])   # the 1C pairs
        q = v.preset_apply(self.p(pre=0), False, 1100)          # the reports still carry the old values: held
        self.assertEqual((q[1], q[2], q[3]), (30.0, 99.0, 64.0))
        v.params(q); v.note_on(69.0, 1.0)
        self.assertAlmostEqual(float(np.max(np.abs(v.render(4800)))), 30 / 127, delta=0.02)

    def test_plock_without_write_back(self):
        v = rmpvoice.RmpVoice(self.lib)
        e, sync = v.preset_decide(0, 3, True, True, self.p(pre=2))
        v.preset_loaded(e, sync, rmpvoice.preset_parse("P2 50"), ROLES, 0, self.p(pre=2))
        p = self.p(pre=0)                                         # a trig whose PRE is p-locked to entry 0
        self.assertEqual(v.preset_decide(0, 3, True, False, p), (0, False))
        self.assertEqual(v.preset_loaded(0, False, rmpvoice.preset_parse("P2 120"), ROLES, 500, p), [])   # nothing written back
        self.assertEqual(v.preset_apply(p, True, 500)[1], 120.0)
        self.assertEqual(v.preset_apply(self.p(pre=2, lev=110), False, 600)[1], 110.0)   # LEV turned since: the turn wins

    def test_pool_without_psram(self):
        sr, f = 48000, 1000.0
        pcm = (np.sin(2 * np.pi * f * np.arange(20000) / sr) * 30000).astype("<i2")
        rmpvoice.pool_set(3, pcm.tobytes(), 1, 16, sr)
        v = rmpvoice.RmpVoice(self.lib)
        v.params(self.p(lev=127, smp=3, mode=127)); v.note_on(60.0, 1.0)
        self.assertEqual(float(np.max(np.abs(v.render(120)))), 0.0)   # not prepared: silent, its prepare asked for
        rmpvoice.pool_job()                                        # the Teensy's loop job: the head
        out = []
        for _ in range(1500):                                      # 18000 frames, a job between requests
            out.append(v.render(12))
            rmpvoice.pool_job()
        a = np.concatenate(out)
        # the head (4096 frames) and then the ring, without a gap: exactly the sample
        np.testing.assert_allclose(a, pcm[:18000] / 32768.0, atol=1e-6)

    def test_pool_jump_outside_the_head(self):
        pcm = ((np.arange(20000) % 2000 - 1000) * 30).astype("<i2")
        rmpvoice.pool_set(4, pcm.tobytes(), 1, 16, 48000)
        v = rmpvoice.RmpVoice(self.lib)
        v.params(self.p(lev=127, smp=4, mode=127, jump=64 + 15)); v.note_on(60.0, 1.0)   # from frame 15000
        v.render(12); rmpvoice.pool_job()                          # the prepare (the head: 0..4095)
        self.assertEqual(float(np.max(np.abs(v.render(12)))), 0.0)   # 15000 is not in RAM: silence, the stream moves there
        rmpvoice.pool_job()                                        # the refill reads from 15000
        np.testing.assert_allclose(v.render(12), pcm[15000:15012] / 32768.0, atol=1e-6)

    def test_pool_reregistered_under_a_voice(self):
        a = (np.arange(20000) * 3 % 30000).astype("<i2")
        b = (-(np.arange(20000) * 5 % 30000)).astype("<i2")
        rmpvoice.pool_set(6, a.tobytes(), 1, 16, 48000)
        v = rmpvoice.RmpVoice(self.lib)
        v.params(self.p(lev=127, smp=6, mode=127)); v.note_on(60.0, 1.0)
        v.render(12); rmpvoice.pool_job()
        np.testing.assert_allclose(v.render(12), a[:12] / 32768.0, atol=1e-6)
        rmpvoice.pool_set(6, b.tobytes(), 1, 16, 48000)            # the slot's sample replaced: the voice's reader cleared
        self.assertEqual(float(np.max(np.abs(v.render(12)))), 0.0)   # not prepared again: silent, nothing of the old sample
        rmpvoice.pool_job()
        np.testing.assert_allclose(v.render(12), b[12:24] / 32768.0, atol=1e-6)

    def test_pool_bad_format_refused(self):
        pcm = np.zeros(100, dtype="<i2").tobytes()
        for ch, bits in ((0, 16), (1, 8), (2, 20), (1, 0)):
            with self.subTest(channels=ch, bits=bits), self.assertRaises(ValueError):
                rmpvoice.pool_set(7, pcm, ch, bits, 48000)
        rmpvoice.pool_set(7, pcm, 1, 32, 48000)                   # 32-bit integer is fine

    def test_pool_psram_random_access(self):
        pcm = ((np.arange(30000) % 3000) * 10).astype("<i2")
        rmpvoice.pool_set(5, pcm.tobytes(), 1, 16, 48000, psram=True)
        v = rmpvoice.RmpVoice(self.lib)
        v.params(self.p(lev=127, smp=5, mode=127, jump=64 + 25)); v.note_on(60.0, 1.0)
        np.testing.assert_allclose(v.render(12), pcm[25000:25012] / 32768.0, atol=1e-6)   # resident: no job needed

class V2Glue(unittest.TestCase):
    """Presets as voiceloop's glue runs them: v2_test's package describes the slot (PRE P1, LEV/TON preset,
    DET offset, preset_ext v2p), a preset folder scanned into the shared names and the slot's index, a
    report through check/decide/load/apply/effective into the plugin's params."""
    @classmethod
    def setUpClass(cls):
        if subprocess.run(["make", "-C", V2, "v2_test.rmp", "libv2_test.dylib"]).returncode:
            toolchain_skip("v2_test.rmp needs Teensyduino's compiler")
        cls.lib = os.path.join(V2, "libv2_test.dylib")
        cls.pkg = os.path.join(V2, "v2_test.rmp")
    FOLDER = {"Lead.v2p": b"V2P 90 40 200", "Bass.txt": b"P2 30\nP4 80\nP10 99\n", "._Bass.txt": b"junk", "Bad.v2p": b"XYZ"}

    def voice(self):
        v = rmpvoice.RmpVoice(self.lib)
        v.set_package(self.pkg)
        v.preset_files(self.FOLDER)
        return v

    def test_names(self):
        self.assertEqual(self.voice().preset_names(), ["Bad", "Bass", "Lead"])   # byte order, '._' rejected

    def test_offset_knob_effective_value_reaches_the_plugin(self):
        v = self.voice()
        e, pairs, got = v.report(V2Test.p(pre=1), first=True)    # Bass: P2 preset (written back), P4 offset (not)
        self.assertEqual((e, pairs), (1, [(1, 30)]))
        self.assertEqual((got[1], got[3], got[9]), (30.0, 80.0, 0.0))   # DET at 64 plays the entry's 80; P10 free: untouched
        v.note_on(69.0, 1.0)
        self.assertAlmostEqual(pitch(v.render(9600)), 440.0 * 2 ** ((80 - 64) / 64 / 12), delta=1.0)
        q = V2Test.p(pre=1, lev=30); q[3] = 70.0                  # DET turned up 6
        e, pairs, got = v.report(q, now_ms=100)
        self.assertEqual((e, pairs, got[3]), (-1, [], 86.0))

    def test_native_preset_through_load_entry(self):
        v = self.voice()
        e, pairs, got = v.report(V2Test.p(pre=2), first=True)    # Lead: V2P 90 40 200
        self.assertEqual((e, pairs, got[1], got[2]), (2, [(1, 90), (2, 40)], 90.0, 40.0))
        v.note_on(69.0, 1.0)
        self.assertAlmostEqual(pitch(v.render(9600)), 440.0 * 2 ** (1 / 12), delta=2.0)   # +100 cents

    def test_failed_entry_not_asked_again(self):
        v = self.voice()
        self.assertEqual(v.report(V2Test.p(pre=0), first=True)[0], -2)   # Bad: load_entry refuses
        self.assertEqual(v.loads(), 1)
        self.assertEqual(v.report(V2Test.p(pre=0), now_ms=10)[0], -1)
        self.assertEqual(v.loads(), 1)

    def test_new_list_generation_reloads(self):
        v = self.voice()
        self.assertEqual(v.report(V2Test.p(pre=1), first=True)[0], 1)
        self.assertEqual(v.report(V2Test.p(pre=1, lev=30), now_ms=10)[0], -1)   # loaded: not again
        v.preset_files(dict(self.FOLDER, **{"Zed.txt": b"P2 7"}))   # the folder written: the index rescans, a new generation
        self.assertEqual(v.preset_names(), ["Bad", "Bass", "Lead", "Zed"])
        e, pairs, _ = v.report(V2Test.p(pre=1, lev=30), now_ms=20)
        self.assertEqual((e, pairs, v.loads()), (1, [(1, 30)], 2))

if __name__ == "__main__":
    unittest.main()
