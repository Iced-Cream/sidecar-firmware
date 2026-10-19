"""teensy/uac2/uac2_test.py SECS: full-duplex check of the Teensy UAC2 device (teensy/uac2_probe) at 48 kHz.
    uv run --with sounddevice python -I teensy/uac2/uac2_test.py 30
    uv run --with sounddevice python -I teensy/uac2/uac2_test.py --selftest NI NO   (no device: synthetic input
        with one dropped packet on IN ch 1 and on the first loopback, to check the checker)
The channel counts are read from the device (2/2, 12/12, 20/12 builds). UAC2_NAME=NAME picks the device of exactly
that name, UAC2_MAKER=MAKER also that manufacturer (the Pi: UAC2_NAME="Sidecar | Expander (Pi)", its test source on:
the Pi's console :uac2test=on; it shows its OUT side in its serial log's sidecar: lines).
OUT: plays a sine on every channel at -12 dBFS, ch c at PRIMES[c] Hz (1009, 1103, 1201 ...). The Teensy's serial shows each
     channel's zero crossings per second, peak and discontinuities.
IN:  checks every channel the Teensy sends. Tone channels against their table, every sample (+-TOL LSB): T2 layout
     ch 1..N-2 at 48000/TONE_PERIOD Hz, -12 dBFS; T1 layout ch 1 at 1 kHz, -6 dBFS. Loopback channels bit-exact
     against what was played, with the round-trip latency: T2 ch N-1 = OUT ch 1, ch N = OUT ch M; T1 ch 2 = OUT ch 1.
Needs microphone permission for the app running it (System Settings > Privacy & Security > Microphone): without it
CoreAudio delivers zeros for every input, and the script says so."""
import math
import os
import sys
import time

import numpy as np
import sounddevice as sd

SELFTEST = len(sys.argv) > 1 and sys.argv[1] == "--selftest"
SECS = 10 if SELFTEST else (float(sys.argv[1]) if len(sys.argv) > 1 else 10)
FS = 48000
FULL = 8388607
TOL = 0   # LSB: the Teensy's table is computed in double, the same integers as here (measured: exact)
TONE_PERIOD = [480, 400, 320, 300, 240, 200, 192, 160, 150, 128, 120, 100, 96, 80, 75, 64, 60, 50]   # uac2_probe.ino

if SELFTEST:
    NI, NO = int(sys.argv[2]), int(sys.argv[3])
else:
    # UAC2_NAME (exact name; UAC2_MAKER, exact manufacturer, optional): the Pi is "Sidecar | Expander (Pi)", the
    # Teensy "Sidecar | Expander (Mini)" (coreaudio_pick.py). Without them: the first device named like
    # "Sidecar"/"UAC2", the Teensy's (Mini, or maker Teensyduino) first when there are more.
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from coreaudio_pick import devices, pick
    if os.environ.get("UAC2_NAME"):
        dev = pick(os.environ["UAC2_NAME"], os.environ.get("UAC2_MAKER"))
        assert dev is not None, devices()
    else:
        found = [i for i, (n, m) in enumerate(devices()) if "UAC2" in n or "Sidecar" in n]
        assert found, devices()
        dev = sorted(found, key=lambda i: not ("(Mini)" in devices()[i][0] or devices()[i][1] == "Teensyduino"))[0]
    info = sd.query_devices(dev)
    NI, NO = info["max_input_channels"], info["max_output_channels"]
    print(f"{info['name']}: {NI} in / {NO} out")

if NI == 2:
    tones = [(0, 48, 0.5)]                     # (IN channel, period, amplitude)
    loops = [(1, 0)]                           # (IN channel, OUT channel)
else:
    tones = [(c, TONE_PERIOD[c], 0.25) for c in range(NI - 2)]
    loops = [(NI - 2, 0), (NI - 1, NO - 1)]
# primes: every channel's period is 1 s, so the loopback latency is unambiguous
PRIMES = [1009, 1103, 1201, 1301, 1399, 1499, 1601, 1699, 1801, 1901, 2003, 2099, 2203, 2297, 2399, 2503, 2609, 2699, 2801, 2903]
OUT_F = PRIMES[:NO]
OUT_A = 10 ** (-12 / 20) * FULL

play_pos = 0
rec = []
flags = {"input_overflow": 0, "input_underflow": 0, "output_overflow": 0, "output_underflow": 0}


def out_signal(c, start, n):
    k = np.arange(start, start + n)
    return np.round(OUT_A * np.sin(2 * np.pi * OUT_F[c] * k / FS)).astype(np.int64)


def cb(indata, outdata, frames, t, status):
    global play_pos
    for f in flags:
        if getattr(status, f):
            flags[f] += 1
    for c in range(NO):
        outdata[:, c] = out_signal(c, play_pos, frames) / 8388608.0   # float32 holds 24-bit samples exactly
    play_pos += frames
    # float32, not int32: PortAudio's float -> int32 scales by 2^31 - 1 and truncates, so positive 24-bit samples
    # arrive 1 LSB low (measured); from float32 the 24-bit value is exact. 4 B per sample (660 s x 12 ch = 1.5 GB)
    rec.append(np.rint(indata * 8388608.0).astype(np.int32))


def synth():
    """what the Teensy would send: silence, then the tones from a random phase and the loopbacks 150 frames late;
    one 12-frame packet dropped on IN ch 1 at 3 s and on the first loopback at 5 s"""
    n, start, lag = SECS * FS, 9000, 150
    y = np.zeros((n, NI), dtype=np.int64)
    for ch, per, amp in tones:
        tab = np.array([round(amp * FULL * math.sin(2 * math.pi * i / per)) for i in range(per)], dtype=np.int64)
        k = np.arange(n - start) + 7
        k[3 * FS:] += 12 if ch == 0 else 0
        y[start:, ch] = tab[k % per]
    for i, (ch, oc) in enumerate(loops):
        k = np.arange(start - lag, n - lag)
        if i == 0:
            k[5 * FS:] += 12
        y[start:, ch] = out_signal(oc, 0, n + FS)[k]
    return y


if SELFTEST:
    x = synth()
else:
    with sd.Stream(device=(dev, dev), samplerate=FS, channels=(NI, NO), dtype="float32", callback=cb, blocksize=480,
                   latency="low"):
        t0 = time.time()
        while time.time() - t0 < SECS:
            time.sleep(0.5)
    x = np.concatenate(rec)
if len(sys.argv) > 2 and sys.argv[2].endswith(".npy") and not SELFTEST:
    np.save(sys.argv[2], x)   # uac2_test.py SECS FILE.npy also keeps the recording
print(f"recorded {len(x)} frames ({len(x) / FS:.2f} s); PortAudio flags {flags}")
if not np.any(x):
    sys.exit("IN all zero: no microphone permission for the app running this (System Settings > Privacy & Security >"
             " Microphone)? The OUT side is on the Teensy's serial (zc, d2max, gl per channel).")


def freq_level(s):
    s = s[: FS * 4].astype(float)
    sp = np.abs(np.fft.rfft(s * np.hanning(len(s))))
    k = int(np.argmax(sp[1:])) + 1
    a, b, c = np.log(sp[k - 1] + 1e-9), np.log(sp[k]), np.log(sp[k + 1] + 1e-9)
    f = (k + 0.5 * (a - c) / (a - 2 * b + c)) * FS / len(s)
    rms = math.sqrt(np.mean(s ** 2))
    return f, (20 * math.log10(rms / FULL) if rms else -999.0)


ok = True
for ch, per, amp in tones:
    s = x[:, ch].astype(np.int64)
    nz = np.nonzero(s)[0]
    if not len(nz):
        print(f"IN ch{ch + 1}: silent")
        ok = False
        continue
    s = s[nz[0]:]
    tab = np.array([round(amp * FULL * math.sin(2 * math.pi * i / per)) for i in range(per)], dtype=np.int64)
    # phase from one period, then every sample against the table; after a break, resync and go on
    breaks, unmatched, pos = 0, 0, 0
    while pos + per <= len(s) and breaks <= 1000:
        seg = s[pos:pos + per]
        offs = [o for o in range(per) if np.all(np.abs(seg - np.roll(tab, -o)) <= TOL)]
        if not offs:
            pos += per
            unmatched += per
            continue
        exp = tab[(np.arange(len(s) - pos) + offs[0]) % per]
        mism = np.nonzero(np.abs(s[pos:] - exp) > TOL)[0]
        if not len(mism):
            break
        breaks += 1
        pos += int(mism[0]) + 1
    f, lvl = freq_level(s)
    good = breaks == 0 and unmatched == 0
    ok &= good
    print(f"IN ch{ch + 1:2d} tone: {f:8.3f} Hz (expect {FS / per:7.3f}), rms {lvl:6.2f} dBFS, "
          f"{'all samples match' if good else f'{breaks} breaks, {unmatched} unmatched'} over {len(s) / FS:.1f} s")

for ch, oc in loops:
    s = x[:, ch].astype(np.int64)
    nz = np.nonzero(s)[0]
    if not len(nz):
        print(f"IN ch{ch + 1}: silent (no loopback of OUT ch{oc + 1})")
        ok = False
        continue
    s = s[nz[0]:]
    ref = out_signal(oc, 0, len(s) + 2 * FS)
    # find the lag (0..1 s) that matches a 4800-frame window bit-exactly, compare from there; after a break,
    # find it again and go on
    pos, breaks, lags, unmatched = FS // 2, 0, [], 0
    off = nz[0]   # stream frame of s[0]
    while pos + 4800 <= len(s) and breaks <= 1000:
        win = s[pos:pos + 4800]
        lag = next((l for l in range(0, min(off + pos, FS) + 1)
                    if np.array_equal(ref[off + pos - l:off + pos - l + 4800], win)), None)
        if lag is None:
            pos += 480
            unmatched += 480
            continue
        if not lags or lags[-1] != lag:
            lags.append(lag)
        exp = ref[off + pos - lag: off + len(s) - lag]
        mism = np.nonzero(s[pos:] != exp)[0]
        if not len(mism):
            break
        breaks += 1
        pos += int(mism[0]) + 1
    f, lvl = freq_level(s)
    good = breaks == 0 and unmatched == 0 and len(lags) == 1
    ok &= good
    if not lags:
        print(f"IN ch{ch + 1}: no bit-exact match with OUT ch{oc + 1}")
        continue
    print(f"IN ch{ch + 1:2d} loopback of OUT ch{oc + 1}: {f:8.3f} Hz, rms {lvl:6.2f} dBFS, round trip {lags[0]} frames "
          f"({lags[0] / FS * 1000:.2f} ms), " + ("bit-exact" if good else f"{breaks} breaks (lags {lags[:8]}), "
          f"{unmatched} frames unmatched") + f" over {len(s) / FS:.1f} s")
print("PASS" if ok else "FAIL")
