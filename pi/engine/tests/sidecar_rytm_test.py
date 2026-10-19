#!/usr/bin/env python3
"""The Sidecar | Expander (Pi) with the Rytm on its USB-A (pi/engine/sidecar_gadget.h): what the Rytm sends reaches the
Mac. Records every IN channel for SECS with OUT silent (or, with --tone, a 1009 Hz sine at -40 dBFS on OUT 1/2 for
the middle 2 s (--tone-db=DB: another level; under a loud pattern -40 can drown), which must come back in the Rytm's MAIN mix on IN 1/2: Overbridge audio only), and listens to the
Pi's MIDI port at the same time (read-only: nothing is ever sent to the Rytm's MIDI).

Checks: PortAudio flags 0; no dropout on IN 1-12 (a run of >= 3 frames with all 12 exactly zero: the Rytm's noise
floors make one impossible otherwise, as in T3); per channel rms and peak; 20-in build: IN 13-20 (the Pi's voices)
reported; MIDI: messages by kind, and none of the host link (F0 7D 52 54) or MIDI-CI reach the Mac.

    uv run --with sounddevice --with numpy --with python-rtmidi python -I pi/engine/tests/sidecar_rytm_test.py SECS [--tone]
"""
import os
import sys
import time

import numpy as np
import rtmidi
import sounddevice as sd

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "teensy", "uac2"))
from coreaudio_pick import devices, midi_pick, pick  # noqa: E402

NAME = os.environ.get("UAC2_NAME", "Sidecar | Expander (Pi)")
FS, FULL = 48000, 8388607
SECS = float(sys.argv[1]) if len(sys.argv) > 1 else 30
TONE = any(a.startswith("--tone") for a in sys.argv)
CH = ["MAIN L", "MAIN R", "BD", "SD", "RS", "BT", "LT", "MT", "CH", "CY", "AUDIO L", "AUDIO R"] + \
     [f"VOICE {v}" for v in range(1, 9)]

dev = pick(NAME)
assert dev is not None, devices()
info = sd.query_devices(dev)
NI, NO = info["max_input_channels"], info["max_output_channels"]
print(f"{info['name']}: {NI} in / {NO} out")

midi = {"note": 0, "cc": 0, "clock": 0, "sysex": 0, "other": 0, "hostlink": 0, "ci": 0}
mi = rtmidi.MidiIn()
mi.ignore_types(sysex=False, timing=False, active_sense=True)
mp = midi_pick(None, False, NAME)
if mp is not None:
    def on_midi(m, d):
        b = m[0]
        if b[0] == 0xF0:
            midi["sysex"] += 1
            if b[1:4] == [0x7D, 0x52, 0x54]:
                midi["hostlink"] += 1
            if len(b) > 4 and b[1] in (0x7E, 0x7F) and b[3] == 0x0D:
                midi["ci"] += 1
        elif b[0] == 0xF8:
            midi["clock"] += 1
        elif b[0] & 0xF0 in (0x90, 0x80):
            midi["note"] += 1
        elif b[0] & 0xF0 == 0xB0:
            midi["cc"] += 1
        else:
            midi["other"] += 1
    mi.set_callback(on_midi)
    mi.open_port(mp)

pos, rec = 0, []
flags = {"input_overflow": 0, "input_underflow": 0, "output_overflow": 0, "output_underflow": 0}
t_on, t_off = int((SECS / 2 - 1) * FS), int((SECS / 2 + 1) * FS)
TONE_DB = float(next((a.split("=")[1] for a in sys.argv if a.startswith("--tone-db=")), -40))
amp = 10 ** (TONE_DB / 20)


def cb(indata, outdata, frames, t, status):
    global pos
    for f in flags:
        if getattr(status, f):
            flags[f] += 1
    outdata[:] = 0
    if TONE:
        k = np.arange(pos, pos + frames)
        s = np.where((k >= t_on) & (k < t_off), amp * np.sin(2 * np.pi * 1009 * k / FS), 0.0)
        outdata[:, 0] = s
        outdata[:, 1] = s
    pos += frames
    rec.append(np.rint(indata * 8388608.0).astype(np.int32))


with sd.Stream(device=(dev, dev), samplerate=FS, channels=(NI, NO), dtype="float32", callback=cb, blocksize=480,
               latency="low"):
    time.sleep(SECS)
mi.close_port()
x = np.concatenate(rec).astype(np.int64)
print(f"recorded {len(x)} frames ({len(x) / FS:.1f} s); PortAudio flags {flags}")
ok = all(v == 0 for v in flags.values())
y = x[FS // 2:]                      # the stream's first half second: priming
z = np.all(y[:, :12] == 0, axis=1)
runs, run = 0, 0
for v in z:
    run = run + 1 if v else 0
    if run == 3:
        runs += 1
print(f"dropouts (>= 3 frames, all 12 zero): {runs}")
ok &= runs == 0
for c in range(NI):
    s = y[:, c].astype(float)
    rms = np.sqrt(np.mean(s ** 2)) / FULL
    print(f"IN {c + 1:2d} {CH[c]:8s} rms {20 * np.log10(rms + 1e-12):7.1f} dBFS  peak {20 * np.log10(np.max(np.abs(s)) / FULL + 1e-12):6.1f}")
if TONE:
    def tone_level(a, b):
        seg = x[a:b, 0].astype(float) / FULL
        k = np.arange(a, b)
        return 20 * np.log10(abs(np.mean(seg * np.exp(-2j * np.pi * 1009 * k / FS))) * 2 + 1e-12)
    during = tone_level(t_on + FS // 4, t_off - FS // 4)
    before = tone_level(t_on - FS - FS // 2, t_on - FS // 2)
    print(f"tone: 1009 Hz on IN 1 {during:.1f} dBFS during, {before:.1f} dBFS before")
    ok &= during > before + 10
print("MIDI from the Rytm:", midi if mp is not None else "no MIDI port")
ok &= midi["hostlink"] == 0 and midi["ci"] == 0
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
