"""The project's pool and its JUNO / FM lists as the Pi's web page edits them (core/user/lists.h, on the Mac core lib):
pool=S:path assigns and empties slots (the pool file and 14 to the Rytm), juno=ren/del/swap/save and fm=ren/del/save
edit the lists (their files, 16 / 17 to the Rytm), and the status JSON shows it all.

    make -C core engines lib
    cd sim/tests && PYTHONPATH=.. ../../.venv/bin/python -m unittest test_core_lists
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
sys.path.insert(0, os.path.join(HERE, ".."))
import protocol as P  # noqa: E402

LIB = os.path.join(HERE, "..", "..", "core", "build", "libcore.dylib")


def scenario(card, script):
    pre = f"""
import json, sys
sys.path.insert(0, {os.path.join(HERE, '..')!r})
import protocol as P
from coreengine import CoreEngine
e = CoreEngine({card!r})
def tx(m):
    e.report(m); e.idle()
    return e.take_sysex()
def cmd(c):
    a = e.lists_command(c); e.idle()
    return a, e.take_sysex()
def of(msgs, cmd): return [x for x in msgs if x[4] == cmd]
def names(msgs, c):
    return [n for x in of(msgs, c) for (_, n) in P.decode_juno_names(x[5:-1])[2]] if of(msgs, c) else None
def pool(msgs):
    return [r for x in of(msgs, 0x14) for r in P.decode_pool_table(x[5:-1])[1]]
def ps(**k):
    v = [64] * 16
    for n, x in k.items(): v[int(n[1:]) - 1] = x
    return [x << 7 for x in v]
tx(P.HDR + bytes([0x10]) + b"LISTS" + b"\\xf7")
"""
    r = subprocess.run([sys.executable, "-c", pre + script], capture_output=True, text=True)
    if r.returncode:
        raise AssertionError(r.stderr[-3000:])
    return json.loads(r.stdout.strip().splitlines()[-1])


def wav(path, n=4800):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(48000)
        w.writeframes(np.zeros(n, np.int16).tobytes())


def juno_line(b, name):
    return "".join(f"{x:02x}" for x in b) + "\t" + name + "\n"


@unittest.skipUnless(os.path.exists(LIB), "make -C core engines lib")
class TestCoreLists(unittest.TestCase):
    def setUp(self):
        self.card = tempfile.mkdtemp(prefix="lists-")
        os.makedirs(os.path.join(self.card, "Samples", "a"))
        wav(os.path.join(self.card, "Samples", "a", "kick.wav"))
        wav(os.path.join(self.card, "Samples", "snare.wav"))
        os.makedirs(os.path.join(self.card, ".rytm", "juno"))
        with open(os.path.join(self.card, ".rytm", "juno", "LISTS.txt"), "w") as f:
            f.write(juno_line(range(18), "Brass") + juno_line([100] * 18, "Strings"))

    def tearDown(self):
        shutil.rmtree(self.card, True)

    def rd(self, *p):
        with open(os.path.join(self.card, *p)) as f:
            return f.read()

    def test_pool(self):
        r = scenario(self.card, """
out = {"j0": e.lists_json()}
a, m = cmd("pool=1:a/kick.wav"); out["a1"] = a; out["m1"] = pool(m)
a, m = cmd("pool=3:snare.wav"); out["a3"] = a
out["j1"] = e.lists_json()
a, m = cmd("pool=2:nothere.wav"); out["a2"] = a
a, m = cmd("pool=3:"); out["c3"] = a; out["m3"] = pool(m)
out["j2"] = e.lists_json()
out["bad"] = cmd("pool=200:x.wav")[0]
out["other"] = e.lists_command("notacommand")
print(json.dumps(out))
""")
        self.assertEqual((r["j0"]["project"], r["j0"]["pool"], r["j0"]["npool"]), ("LISTS", [], 0))
        self.assertIn("ok", r["a1"])
        self.assertEqual([(x[0], x[3]) for x in r["m1"]], [(1, "kick")])
        self.assertEqual([(x["slot"], x["path"], x["ok"]) for x in r["j1"]["pool"]],
                         [(1, "a/kick.wav", True), (3, "snare.wav", True)])
        self.assertEqual(r["j1"]["npool"], 3)
        self.assertIn("not on the card", r["a2"])
        self.assertIn("empty", r["c3"])
        self.assertEqual([x[0] for x in r["m3"]], [1])
        self.assertEqual((r["j2"]["npool"], len(r["j2"]["pool"])), (1, 1))
        self.assertIn("usage", r["bad"])
        self.assertEqual(r["other"], "")
        self.assertEqual(self.rd(".rytm", "pools", "LISTS.txt"), "a/kick.wav\n")

    def test_juno(self):
        r = scenario(self.card, """
out = {"j0": e.lists_json()["juno"]}
a, m = cmd("juno=ren:0:Fat Brass"); out["ren"] = a; out["n_ren"] = names(m, 0x16)
a, m = cmd("juno=swap:0:1"); out["swap"] = a; out["n_swap"] = names(m, 0x16)
a, m = cmd("juno=del:1"); out["del"] = a; out["n_del"] = names(m, 0x16)
out["nojuno"] = cmd("juno=save:1")[0]
# voice 1 plays JUNO: the first report sends the patch's six knobs (15) and holds them until a report carries them
k = of(tx(P.params_report(0x09, ps(p1=0))), 0x15)
six = P.decode_juno_knobs(k[0][5:-1])[1] if k else []
out["six"] = six
q = dict(p1=0); q.update({f"p{2 + i}": v for i, v in enumerate(six)})
tx(P.params_report(0x09, ps(**q)))
q["p2"] = 99   # CUT turned
tx(P.params_report(0x09, ps(**q)))
a, m = cmd("juno=save:1:My Pad"); out["save"] = a; out["n_save"] = names(m, 0x16)
out["j1"] = e.lists_json()
print(json.dumps(out))
""")
        self.assertEqual([(x["slot"], x["name"]) for x in r["j0"]["list"]], [(0, "Brass"), (1, "Strings")])
        self.assertEqual(r["n_ren"], ["Fat Brass", "Strings"])
        self.assertEqual(r["n_swap"], ["Strings", "Fat Brass"])
        self.assertEqual(r["n_del"], ["Strings"])
        self.assertIn("does not play JUNO", r["nojuno"])
        self.assertEqual(len(r["six"]), 6)
        self.assertIn("juno slot 1", r["save"])
        self.assertEqual(r["n_save"], ["Strings", "My Pad"])
        lines = self.rd(".rytm", "juno", "LISTS.txt").splitlines()
        self.assertEqual(lines[1].split("\t")[1], "My Pad")
        b = bytes.fromhex(lines[1].split("\t")[0])
        self.assertEqual(b[5], 99)   # CUT as the knob is now
        self.assertEqual([b[x] for x in (6, 7, 11, 12, 14)], r["six"][1:])
        self.assertEqual(r["j1"]["voices"][1]["m"], "JUNO")

    def test_fm(self):
        r = scenario(self.card, """
out = {"j0": e.lists_json()["fm"]}
out["nofm"] = cmd("fm=save:0")[0]
tx(P.params_report(0x48, ps(p1=5)))   # voice 0 plays 6-OP FM: PAT picks a ROM voice (the list is empty)
a, m = cmd("fm=save:0"); out["save"] = a; out["n_save"] = names(m, 0x17)
out["again"] = cmd("fm=save:0")[0]
a, m = cmd("fm=ren:0:MY EP"); out["n_ren"] = names(m, 0x17)
out["j1"] = e.lists_json()["fm"]
out["file"] = open(CARD + "/.rytm/fm/LISTS.txt").read()
a, m = cmd("fm=del:0"); out["n_del"] = names(m, 0x17)
out["j2"] = e.lists_json()["fm"]
print(json.dumps(out))
""".replace("CARD", repr(self.card)))
        self.assertEqual((r["j0"]["rom"], r["j0"]["total"], r["j0"]["list"]), (True, 96, []))
        self.assertIn("does not play FM", r["nofm"])
        self.assertIn("fm slot 0 = ROM voice", r["save"])
        self.assertEqual(len(r["n_save"]), 1)
        self.assertTrue(r["n_save"][0])
        self.assertIn("already", r["again"])
        self.assertEqual(r["n_ren"], ["MY EP"])
        self.assertIn("\tMY EP", r["file"])
        self.assertEqual((r["j1"]["rom"], r["j1"]["list"][0]["name"], r["j1"]["list"][0]["found"]), (False, "MY EP", True))
        self.assertEqual(len(r["n_del"]), 96)   # the list empty again: the ROM's names
        self.assertTrue(r["j2"]["rom"])
        self.assertEqual(self.rd(".rytm", "fm", "LISTS.txt"), "")


if __name__ == "__main__":
    unittest.main()
