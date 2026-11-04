"""DSP SAMPLE: ENG (P1, SMPL knob D since rytm-mods 7h) changed while a note plays, the 03 alone: the resonator follows, the voice
keeps sounding, nothing blows up. A card sample on row A001.

    make -C core engines lib
    cd sim/tests && PYTHONPATH=.. ../../.venv/bin/python -m unittest test_core_dsps_eng
"""
import json
import os
import shutil
import struct
import tempfile
import unittest
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
from test_core_um import scenario  # noqa: E402

SCRIPT = r"""
import ctypes
e.lib.core_c_nm_command.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
rb = ctypes.create_string_buffer(300)
e.report(P.HDR + bytes([0x10]) + b"ENGTEST\xf7")
for _ in range(300): e.lib.core_c_lib(64); e.lib.core_c_idle(10)
e.lib.core_c_nm_command(b"pool=smp:set:A001:/Samples/burst.wav", rb, 300)
for _ in range(300): e.lib.core_c_lib(64); e.lib.core_c_idle(10)
def p(eng):
    return [x << 7 for x in [eng, 64, 64, 64, 90, 64, 64, 100, 64, 64, 1, 0, 0, 120, 1, 100]]
e.report(P.trig_params(0x20, 60, 100, p(0))); e.idle()
import time
for _ in range(40): e.idle(); e.lib.core_c_idle(10); time.sleep(0.01)
e.report(P.trig_params(0x20, 60, 100, p(0))); e.idle()
out = []
for eng in (0, 43, 86, 0):
    e.report(P.params_report(0x20, p(eng))); e.idle()
    a = e.frames(14400)[:, 0].astype(float)
    e.lib.core_c_idle(10)
    b = a[2400:]
    out.append([float(abs(b).max()), float(abs(a[1:] - a[:-1]).max()), float(sum(abs(b[1:] - b[:-1])) / (1 + sum(abs(b))))])
print(json.dumps(out))
"""


HEAD = r"""
import ctypes
rb = ctypes.create_string_buffer(300)
e.lib.core_c_nm_command.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
e.report(P.HDR + bytes([0x10]) + b"ENGTEST\xf7")
for _ in range(300): e.lib.core_c_lib(64); e.lib.core_c_idle(10)
e.lib.core_c_nm_command(b"pool=smp:set:A001:/Samples/burst.wav", rb, 300)
for _ in range(300): e.lib.core_c_lib(64); e.lib.core_c_idle(10)
ps = [x << 7 for x in [0, 64, 64, 64, 0, 64, 64, 100, 64, 64, 1, 0, 0, 120, 0, 100]]
e.report(P.trig_params(0x20, 60, 100, ps)); e.idle()
import time
for _ in range(40): e.idle(); e.lib.core_c_idle(10); time.sleep(0.01)
e.report(P.trig_params(0x20, 60, 100, ps)); e.idle()
h = []
for _ in range(14):
    e.frames(9600); e.lib.core_c_idle(10)
    h.append(e.lib.core_c_dsp_playhead(0))
print(json.dumps(h))
"""


def burst(card):
    os.makedirs(os.path.join(card, "Samples"))
    os.makedirs(os.path.join(card, "Plugins"))
    with wave.open(os.path.join(card, "Samples", "burst.wav"), "wb") as w:
        w.setnchannels(1), w.setsampwidth(2), w.setframerate(48000)
        w.writeframes(b"".join(struct.pack("<h", int(9000 * ((i * 7919) % 97 / 48 - 1))) for i in range(96000)))


class TestHead(unittest.TestCase):
    def test_playhead(self):
        """MIX 0, a 2 s sample: the position climbs over the file at 0.1 of it a step, and ends at the sample's end."""
        card = tempfile.mkdtemp(prefix="dhead-")
        burst(card)
        try:
            h = scenario(card, HEAD)
        finally:
            shutil.rmtree(card, True)
        run = [x for x in h if x >= 0]
        self.assertGreater(len(run), 6, h)
        self.assertEqual(run, sorted(run), h)
        self.assertLess(abs((run[3] - run[1]) / 65534 - 0.2), 0.03, h)
        self.assertEqual(h[-1], -1, h)


class TestEng(unittest.TestCase):
    def test_eng_during_a_note(self):
        card = tempfile.mkdtemp(prefix="deng-")
        burst(card)
        try:
            r = scenario(card, SCRIPT)
        finally:
            shutil.rmtree(card, True)
        self.assertTrue(all(x[0] > 50 for x in r), r)              # it keeps sounding in every model
        self.assertTrue(all(x[1] < 32000 for x in r), r)           # no rail-to-rail step at the change
        self.assertGreater(abs(r[0][2] - r[1][2]), 0.1, r)   
