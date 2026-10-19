"""User-machine presets and pool samples on a card store (core/user/um.cpp: the Pi's and the Mac core lib's), with the
v2_test package run in place: /PRESETS/<id>/ scanned (a generic .txt and a native .v2p), 1A's presets flag, 1B 0x20
names, the PRE knob's loads with their 1C write-back, a native preset's hidden state heard (its detune), um=save
writing a generic preset of what plays, and API 2's pool_info / pool_read served from the sample RAM (POOL mode plays
the pool slot's WAV). The rules are the Teensy's (teensy/libraries/RytmPlugins/src/rmp_presets.h).

    make -C core engines lib && make -C plugins/v2_test a64
    cd sim/tests && PYTHONPATH=.. ../../.venv/bin/python -m unittest test_core_presets
One engine per process (the core keeps module state): each test runs its scenario in a subprocess.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(HERE, ".."))
import protocol as P  # noqa: E402

PKG = os.path.join(REPO, "plugins", "v2_test", "v2_test.a64.rmp")
HAVE = os.path.exists(PKG)
ID = "rytm.v2_test"


def scenario(card, script, env=None):
    pre = f"""
import json, sys, numpy as np
sys.path.insert(0, {os.path.join(HERE, '..')!r})
import protocol as P
from coreengine import CoreEngine
e = CoreEngine({card!r}, um=True)
def tx(m):
    e.report(m); e.idle()
    return [x for x in e.take_sysex()]
def of(msgs, cmd): return [x for x in msgs if x[4] == cmd]
def inst():
    m = of(tx(P.ask(0, 0, P.ASK_INSTALLED)), P.LIST)
    names = [n for x in m for (_, n) in P.decode_list(x[5:-1])["entries"]]
    return m[0][8], names
def meta(s):
    return [P.decode_meta(x[5:-1]) for x in of(tx(P.ask(s, 0, P.ASK_META)), P.META)]
def names(s, page=0):
    m = [P.decode_list(x[5:-1]) for x in of(tx(P.ask(s, 0, P.ASK_PRESETS, page)), P.LIST)]
    return dict(total=m[0]["total"], names=[n for x in m for (_, n) in x["entries"]], lists=[x["list"] for x in m])
def knobs(msgs): return [P.decode_knobs(x[5:-1])["knobs"] for x in of(msgs, P.KNOBS)]
def ps(**k):   # P1..P16 as 14-bit values (value << 7)
    v = [64] * 16
    for n, x in k.items(): v[int(n[1:]) - 1] = x
    return [x << 7 for x in v]
def freq(a):
    f = np.abs(np.fft.rfft(a * np.hanning(len(a))))
    i = int(np.argmax(f))
    return float(i * 48000 / len(a))
gen, nm = inst()
tx(P.HDR + bytes([0x10]) + b"PRESETS" + b"\\xf7")
tx(P.assign(1, P.OP_ASSIGN, nm.index("V2 TEST"), gen))
"""
    r = subprocess.run([sys.executable, "-c", pre + script], capture_output=True, text=True,
                       env=dict(os.environ, **(env or {})))
    if r.returncode:
        raise AssertionError(r.stderr[-3000:])
    return json.loads(r.stdout.strip().splitlines()[-1])


def write_sine(path, hz, secs=1.0, sr=48000):
    t = np.arange(int(secs * sr)) / sr
    a = (0.5 * 32767 * np.sin(2 * np.pi * hz * t)).astype(np.int16)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(a.tobytes())


@unittest.skipUnless(HAVE, "build the RMPA package: make -C plugins/v2_test a64")
class TestCorePresets(unittest.TestCase):
    def setUp(self):
        self.card = tempfile.mkdtemp(prefix="pre-")
        for d in ("Samples", "Plugins", os.path.join("PRESETS", ID)):
            os.makedirs(os.path.join(self.card, d))
        shutil.copy(PKG, os.path.join(self.card, "Plugins", "v2_test.rmp"))
        pre = os.path.join(self.card, "PRESETS", ID)
        with open(os.path.join(pre, "Bass.txt"), "w") as f:
            f.write("P2 50\nP3 20\n")
        with open(os.path.join(pre, "Lead.v2p"), "w") as f:
            f.write("V2P 90 10 150")   # level 90, tone 10, +50 cents (hidden)
        with open(os.path.join(pre, "._Bass.txt"), "w") as f:
            f.write("P2 1\n")          # macOS junk: not listed
        os.makedirs(os.path.join(pre, "sub"))

    def tearDown(self):
        shutil.rmtree(self.card, True)

    def test_presets_names_load_writeback_save(self):
        r = scenario(self.card, """
out = {}
out["meta"] = meta(1)[0]
out["names"] = names(1)
# the sound's first non-trig report: PRE 0 (Bass) loads and is written back (1C P2 = 50, P3 = 20)
out["kb0"] = knobs(tx(P.params_report(0x50, ps(p1=0, p6=0))))
# PRE 1 (Lead, native): its values written back, its hidden detune heard
out["kb1"] = knobs(tx(P.params_report(0x50, ps(p1=1, p6=0, p2=50, p3=20))))
e.report(P.trig_params(0x50, 69, 100, ps(p1=1, p6=0, p2=90, p3=10))); e.idle()
a = e.frames(9600)[:, 0].astype(float)
out["lead"] = [float(np.abs(a).max()), freq(a)]
out["json"] = [x for x in e.um_json() if x.get("t") == "slot" and x["slot"] == 1][0]
out["save"] = e.um_command("um=save:1:My Sound")
out["save_bad"] = e.um_command("um=save:2:X")
import time; time.sleep(0.4); e.idle()
out["names2"] = names(1)
out["meta2"] = meta(1)[0]
print(json.dumps(out))
""")
        self.assertTrue(r["meta"]["flags"] & P.FLAG_PRESETS)
        self.assertEqual(r["names"]["names"], ["Bass", "Lead"])
        self.assertEqual(r["names"]["total"], 2)
        self.assertEqual(r["names"]["lists"], [P.LIST_PRESETS])
        self.assertEqual([sorted(k) for k in r["kb0"]], [[[1, 50], [2, 20]]])
        self.assertEqual([sorted(k) for k in r["kb1"]], [[[1, 90], [2, 10]]])
        peak, f = r["lead"]
        self.assertGreater(peak, 1000)
        self.assertAlmostEqual(f, 440.0 * 2 ** (50 / 1200), delta=4)   # the native preset's +50 cents
        self.assertEqual((r["json"]["presets"], r["json"]["pre_ok"], r["json"]["pre_ext"]), (2, True, "v2p"))
        self.assertIn("saved", r["save"])
        self.assertIn("plays nothing", r["save_bad"])
        with open(os.path.join(self.card, "PRESETS", ID, "My Sound.txt")) as f:
            saved = f.read().split("\n")
        self.assertIn("P2 90", saved)   # the preset-role knobs as the voice plays them
        self.assertIn("P3 10", saved)
        self.assertFalse(any(x.startswith("P1 ") for x in saved))   # never PRE itself
        self.assertEqual(r["names2"]["names"], ["Bass", "Lead", "My Sound"])
        self.assertNotEqual(r["meta2"]["gen"], r["meta"]["gen"])   # the list changed: a new generation

    def test_no_folder_no_presets(self):
        shutil.rmtree(os.path.join(self.card, "PRESETS"))
        r = scenario(self.card, """
out = {"meta": meta(1)[0], "names": names(1), "kb": knobs(tx(P.params_report(0x50, ps(p1=3))))}
print(json.dumps(out))
""")
        self.assertFalse(r["meta"]["flags"] & P.FLAG_PRESETS)
        self.assertEqual((r["names"]["total"], r["names"]["names"]), (0, []))
        self.assertEqual(r["kb"], [])

    def test_pool_read_from_sample_ram(self):
        os.makedirs(os.path.join(self.card, "Samples", "t"))
        write_sine(os.path.join(self.card, "Samples", "t", "tone.wav"), 1000.0)
        os.makedirs(os.path.join(self.card, ".rytm", "pools"))
        with open(os.path.join(self.card, ".rytm", "pools", "PRESETS.txt"), "w") as f:
            f.write("t/tone.wav\n")
        r = scenario(self.card, """
out = {}
e.report(P.trig_params(0x50, 60, 100, ps(p1=0, p5=1, p6=127, p2=100))); e.idle()
e.frames(240)   # the first blocks ask for the slot (NOT_READY): the control core loads and pins it
for _ in range(40): e.idle()   # (one sample-RAM read a call here; the Pi's control core loops)
e.report(P.trig_params(0x50, 60, 100, ps(p1=0, p5=1, p6=127, p2=100))); e.idle()
a = e.frames(9600)[:, 0].astype(float)
out["pool"] = [float(np.abs(a).max()), freq(a)]
e.report(P.trig_params(0x50, 60, 100, ps(p1=0, p5=2, p6=127, p2=100))); e.idle()
e.frames(240); e.idle()
b = e.frames(4800)[:, 0].astype(float)
out["empty"] = float(np.abs(b).max())
print(json.dumps(out))
""", env={"CORE_SAMPLE_RAM_MB": "16"})
        peak, f = r["pool"]
        self.assertGreater(peak, 1000)
        self.assertAlmostEqual(f, 1000.0, delta=10)
        self.assertEqual(r["empty"], 0.0)   # slot 2 is not in the pool: EMPTY, silence

    def test_pool_without_sample_ram_is_empty(self):
        os.makedirs(os.path.join(self.card, "Samples", "t"))
        write_sine(os.path.join(self.card, "Samples", "t", "tone.wav"), 1000.0)
        os.makedirs(os.path.join(self.card, ".rytm", "pools"))
        with open(os.path.join(self.card, ".rytm", "pools", "PRESETS.txt"), "w") as f:
            f.write("t/tone.wav\n")
        r = scenario(self.card, """
e.report(P.trig_params(0x50, 60, 100, ps(p1=0, p5=1, p6=127, p2=100))); e.idle()
e.frames(240); e.idle()
a = e.frames(4800)[:, 0].astype(float)
print(json.dumps({"peak": float(np.abs(a).max())}))
""", env={"CORE_SAMPLE_RAM_MB": "0"})
        self.assertEqual(r["peak"], 0.0)


if __name__ == "__main__":
    unittest.main()
