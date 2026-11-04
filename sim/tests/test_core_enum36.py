"""List knobs of up to 36 names on the card hosts (core/user/um.cpp): a Rytm that says it holds 36 (0021 since 0.4.0: 19 what 0,
arg 1) gets them all, in five 1Bs; an older one (arg 0, 16 names) gets 16, each the name of the zone its step stores 8 k reads
in (k x 36 / 16), and the knob's long name says 16/36. A list of 16 is the same for both.

    make -C core engines lib && make -C plugins/sine_test a64
    cd sim/tests && PYTHONPATH=.. ../../.venv/bin/python -m unittest test_core_enum36
"""
import json
import os
import shutil
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
from test_core_labels import SINE, relabel  # noqa: E402
from test_core_um import scenario  # noqa: E402

NAMES = [f"N{i:02d}" for i in range(36)]
MAN = {"id": "rytm.long_test", "version": "1.0.0", "name": "LONG TEST", "short": "LONG", "licence": "MIT", "page2": "params",
       "knobs": [{"p": 1, "long": "Oscillator", "short": "OSC", "look": "list", "text": "enum", "names": NAMES, "default": 72},
                 {"p": 2, "long": "Short", "short": "SHRT", "look": "list", "text": "enum", "names": ["AA", "BB", "CC"]}]}
SCRIPT = r"""
gen, names = inst()
tx(P.HDR + bytes([0x10]) + b"LONGTEST" + b"\xf7")
tx(P.assign(1, P.OP_ASSIGN, names.index("LONG TEST"), gen))
for _ in range(20): e.idle()
def ask(cap):
    m = [P.decode_meta(x[5:-1]) for x in of(tx(P.ask(1, 0, P.ASK_META, cap)), P.META)]
    return [(k["p"], k["long"], k["enum_count"]) for x in m[1:] for k in x["knobs"] if k["p"] <= 2]
def enum(knob):
    ls = of(tx(P.ask(1, 0, P.ASK_ENUM, knob)), P.LIST)
    return len(ls), [n for x in ls for _, n in P.decode_list(x[5:-1])["entries"]], P.decode_list(ls[0][5:-1])["total"]
out = {}
out["new"] = ask(1), enum(0), enum(1)
out["old"] = ask(0), enum(0), enum(1)
print(json.dumps(out))
"""


@unittest.skipUnless(os.path.exists(SINE), "make -C plugins/sine_test a64; make -C core engines lib")
class TestEnum36(unittest.TestCase):
    def setUp(self):
        self.card = tempfile.mkdtemp(prefix="e36-")
        os.makedirs(os.path.join(self.card, "Samples"))
        os.makedirs(os.path.join(self.card, "Plugins"))
        with open(SINE, "rb") as f:
            with open(os.path.join(self.card, "Plugins", "long.rmp"), "wb") as o:
                o.write(relabel(f.read(), MAN))

    def tearDown(self):
        shutil.rmtree(self.card, True)

    def test_serving(self):
        r = scenario(self.card, SCRIPT)
        (k, e0, e1) = r["new"]
        self.assertEqual(k[:2], [[1, "Oscillator", 36], [2, "Short", 3]])
        self.assertEqual((e0[0], e0[1], e0[2]), (5, NAMES, 36))
        self.assertEqual(e1[1:], [["AA", "BB", "CC"], 3])
        (k, e0, e1) = r["old"]
        self.assertEqual(k[:2], [[1, "Oscill 16/36", 16], [2, "Short", 3]])
        want = [NAMES[(8 * z) * 36 // 128] for z in range(16)]   # the zone a 16-step Rytm's stored value reads in
        self.assertEqual((e0[1], e0[2]), (want, 16))
        self.assertEqual(e1[1:], [["AA", "BB", "CC"], 3])
