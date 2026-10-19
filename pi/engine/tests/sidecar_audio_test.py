#!/usr/bin/env python3
"""The Sidecar's audio bridge (pi/engine/sidecar_gadget.h) without the Rytm: the Pi's self-test (:selftest=on) stands in
for the Rytm, its Overbridge OUT frames (the engine's output with the Mac's OUT added) come back as the Rytm's IN.
With :out=main the engine sends only on OUT 1-2 (MAIN), so IN 3-12 must be the Mac's OUT 3-12 bit-exact, at one
constant lag (a break only where the self-test's pacer slipped: ALLOW_BREAKS=N, its "slips" count); IN 1-2 carry the engine's MAIN plus the Mac's OUT 1-2. 20-in build: IN 13-20 are the 8 voices (the
self-test's notes play voice 0: IN 13 sounds, 14-20 silent with :drone=off). --mac-off: the Pi's :mac=off is set, so
IN 3-12 must be silent.

    uv run --with sounddevice --with numpy python -I pi/engine/tests/sidecar_audio_test.py SECS [--mac-off]

Device: UAC2_NAME (default "Sidecar | Expander (Pi)"), optionally of manufacturer UAC2_MAKER: teensy/uac2/coreaudio_pick.py.
"""
import os
import sys
import time

import numpy as np
import sounddevice as sd

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "teensy", "uac2"))
from coreaudio_pick import devices, pick  # noqa: E402

FS, FULL = 48000, 8388607
SECS = float(sys.argv[1]) if len(sys.argv) > 1 else 20
MAC_OFF = "--mac-off" in sys.argv
ALLOW = int(os.environ.get("ALLOW_BREAKS", "0"))   # the self-test's slips in the run (its log), each one break
PRIMES = [1009, 1103, 1201, 1301, 1399, 1499, 1601, 1699, 1801, 1901, 2003, 2099]
AMP = 10 ** (-12 / 20) * FULL

dev = pick(os.environ.get("UAC2_NAME", "Sidecar | Expander (Pi)"), os.environ.get("UAC2_MAKER"))
assert dev is not None, devices()
info = sd.query_devices(dev)
NI, NO = info["max_input_channels"], info["max_output_channels"]
print(f"{info['name']}: {NI} in / {NO} out")
pos, rec = 0, []


def sig(c, start, n):
    k = np.arange(start, start + n)
    return np.round(AMP * np.sin(2 * np.pi * PRIMES[c] * k / FS)).astype(np.int64)


def cb(indata, outdata, frames, t, status):
    global pos
    for c in range(NO):
        outdata[:, c] = sig(c, pos, frames) / 8388608.0
    pos += frames
    rec.append(np.rint(indata * 8388608.0).astype(np.int64))


with sd.Stream(device=(dev, dev), samplerate=FS, channels=(NI, NO), dtype="float32", callback=cb, blocksize=480,
               latency="low"):
    time.sleep(SECS)
x = np.concatenate(rec)
print(f"recorded {len(x)} frames ({len(x) / FS:.1f} s)")
ok = True
skip = FS  # the first second: priming, fade-in
lags = set()
for c in range(2, min(12, NI)):
    s = x[skip:, c]
    if MAC_OFF:
        good = not np.any(s)
        print(f"IN {c + 1:2d}: {'silent' if good else 'NOT silent'} (mac=off)")
        ok &= good
        continue
    ref = sig(c, 0, len(x) + 3 * FS)
    # bit-exact segments: find the lag on a 4800-frame window, compare until the first difference, find it again.
    # A break is a stand-in slip (the self-test's pacer, its log line "slips") or a real loss: counted, reported.
    p, breaks, unmatched, seg_lags = 0, 0, 0, []
    while p + 4800 <= len(s) and breaks <= 200:
        win = s[p:p + 4800]
        at = skip + p
        lag = next((l for l in range(0, 3 * FS) if at - l >= 0 and np.array_equal(ref[at - l:at - l + 4800], win)), None)
        if lag is None:
            p += 480
            unmatched += 480
            continue
        seg_lags.append(lag)
        exp = ref[at - lag:at - lag + len(s) - p]
        mism = np.nonzero(s[p:] != exp)[0]
        if not len(mism):
            break
        breaks += 1
        p += int(mism[0]) + 1
    lags.add(tuple(seg_lags))
    print(f"IN {c + 1:2d} = OUT {c + 1}: lags {seg_lags[:6]}, {breaks} breaks, {unmatched} frames unmatched over {len(s) / FS:.1f} s")
    ok &= unmatched <= 480 * 4 * (breaks + 1) and breaks <= ALLOW
if not MAC_OFF and len(lags) > 1:
    print("FAIL: the channels' lags differ", lags)
    ok = False
for c in range(min(2, NI)):
    rms = np.sqrt(np.mean(x[skip:, c].astype(float) ** 2)) / FULL
    print(f"IN {c + 1}: MAIN (engine + OUT {c + 1}) rms {20 * np.log10(rms + 1e-12):.1f} dBFS")
if NI > 12:
    for c in range(12, NI):
        pk = int(np.max(np.abs(x[skip:, c])))
        want = c == 12
        good = (pk > 0) == want
        print(f"IN {c + 1}: voice {c - 11} peak {pk} ({'sounds' if pk else 'silent'}){'' if good else '  FAIL'}")
        ok &= good
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
