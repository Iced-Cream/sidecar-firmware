"""teensy/uac2/uac2_rytm_test.py SECS [--tone]: the host build's UAC2 device (voiceloop with RMP_CORE=uac2) while the
Rytm streams Overbridge: records the Teensy's 12 IN channels (the Rytm's MAIN L/R, BD SD RS BT LT MT CH CY,
AUDIO IN L/R) and checks they are there and continuous; a 20-in build's IN 13..20 (the Teensy's 8 voices, vBD..vCY, in
the same track order) are listed too (a voice is silent between its notes: not part of the dropout check).
    uv run --with sounddevice python -I teensy/uac2/uac2_rytm_test.py 660
Per channel: rms, peak, the share of exactly-zero samples. Dropouts: the Teensy fills an IN underrun with
zeros, and the Rytm's channels carry a noise floor (a frame with all 12 channels exactly zero does not happen
otherwise): runs of >= 3 such frames are counted and listed.
OUT plays silence (the computer's channels are added to the Rytm's Overbridge inputs: silence changes nothing),
except with --tone: a 1009 Hz sine at -40 dBFS on OUT 1/2 (the Rytm's MAIN L/R input) from 2 s to 4 s, looked for
in IN 1/2 (the Rytm's MAIN mix) against the seconds before. It is audible on the Rytm's outputs, quietly.
UAC2_NAME="<name as CoreAudio lists it>" picks one device when the Pi's Sidecar | Expander is connected too.
Needs microphone permission for the app running it."""
import math
import os
import sys
import time

import numpy as np
import sounddevice as sd

SECS = float(sys.argv[1]) if len(sys.argv) > 1 else 30
TONE = "--tone" in sys.argv
FS = 48000
NAMES = "MAIN-L MAIN-R BD SD RS BT LT MT CH CY IN-L IN-R".split() + ["v" + t for t in "BD SD RS BT LT MT CH CY".split()]

# UAC2_NAME=NAME: the device of exactly that name as CoreAudio lists it (both connected: "Sidecar | Expander (Mini)"
# is the Teensy, "Sidecar | Expander (Pi)" the Pi; macOS may show the punctuation changed, e.g. "_Mini_"). Without it:
# a device named with Sidecar and Expander (or the older Sidecar / UAC2), the Teensy's (Mini) first.
want = os.environ.get("UAC2_NAME")
names = [d["name"] for d in sd.query_devices()]
if want:
    dev = [i for i, n in enumerate(names) if n == want and sd.query_devices(i)["max_input_channels"] > 0]
else:
    dev = sorted((i for i, n in enumerate(names) if ("Sidecar" in n or "UAC2" in n) and sd.query_devices(i)["max_input_channels"] > 0),
                 key=lambda i: ("Expander" not in names[i], "Mini" not in names[i]))
assert dev, f"no such device ({want or 'Sidecar / UAC2'}) among {names}"
dev = dev[0]
info = sd.query_devices(dev)
NI, NO = info["max_input_channels"], info["max_output_channels"]
print(f"{info['name']}: {NI} in / {NO} out")
assert NI >= 12 and NO >= 2

rec, pos = [], 0
flags = {"input_overflow": 0, "input_underflow": 0, "output_overflow": 0, "output_underflow": 0}
T0, T1, AMP = 2 * FS, 4 * FS, 10 ** (-40 / 20)


def cb(indata, outdata, frames, t, status):
    global pos
    for f in flags:
        if getattr(status, f):
            flags[f] += 1
    outdata[:] = 0
    if TONE:
        k = np.arange(pos, pos + frames)
        on = (k >= T0) & (k < T1)
        s = (AMP * np.sin(2 * np.pi * 1009 * k / FS) * on).astype(np.float32)
        outdata[:, 0] = s
        outdata[:, 1] = s
    pos += frames
    rec.append(indata[:, :min(NI, 20)].copy())


with sd.Stream(device=(dev, dev), samplerate=FS, channels=(NI, NO), dtype="float32", callback=cb, blocksize=480,
               latency="low"):
    t0 = time.time()
    while time.time() - t0 < SECS:
        time.sleep(0.5)
x = np.concatenate(rec)
print(f"recorded {len(x)} frames ({len(x) / FS:.2f} s); PortAudio flags {flags}")
if not np.any(x):
    sys.exit("IN all zero: no microphone permission for the app running this, or the Rytm is not streaming")
start = int(np.nonzero(np.any(x != 0, axis=1))[0][0])
x = x[start:]
print(f"first non-zero frame {start}")
for c in range(x.shape[1]):
    s = x[:, c].astype(np.float64)
    rms = math.sqrt(np.mean(s ** 2))
    pk = np.abs(s).max()
    db = lambda v: 20 * math.log10(v) if v > 0 else -999.0
    print(f"IN {c + 1:2d} {NAMES[c]:6s} rms {db(rms):7.1f} dBFS, peak {db(pk):6.1f} dBFS, zero samples {np.mean(s == 0) * 100:5.2f} %")
allz = np.all(x[:, :12] == 0, axis=1).astype(np.int8)
d = np.diff(np.concatenate(([0], allz, [0])))
runs = [(int(a), int(b - a)) for a, b in zip(np.nonzero(d == 1)[0], np.nonzero(d == -1)[0]) if b - a >= 3]
print(f"dropouts (>= 3 frames with all 12 channels exactly 0): {len(runs)}"
      + ("" if not runs else f", total {sum(r[1] for r in runs)} frames; first: " +
         ", ".join(f"{a / FS:.3f} s x{n}" for a, n in runs[:10])))
ok = not runs and not any(flags.values())
if TONE:
    def tone_db(seg):   # 1009 Hz magnitude by correlation, dBFS
        k = np.arange(len(seg))
        z = np.exp(-2j * np.pi * 1009 * k / FS)
        return 20 * math.log10(2 * abs(np.mean(seg * z)) + 1e-12)
    # the stream's frame index of x[0] is `start`; the tone ran T0..T1 of the played stream (plus the round trip)
    a, b = T0 - start + FS // 4, T1 - start - FS // 4
    for c in (0, 1):
        on, off = tone_db(x[a:b, c]), tone_db(x[max(0, a - 2 * FS):max(0, a - 2 * FS) + (b - a), c])
        print(f"tone in IN {c + 1}: {on:.1f} dBFS during, {off:.1f} dBFS before (played -40 dBFS on OUT {c + 1})")
        ok &= on - off > 10
print("PASS" if ok else "FAIL")
