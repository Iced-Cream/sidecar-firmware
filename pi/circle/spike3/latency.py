#!/usr/bin/env python3
"""Trig-to-sound latency of the Pi's Plaits voice against the Rytm's own drum, from a spike3 capture.

spike3's 'w' capture (capture.py) has 4 int32 channels, all on the IN (Rytm -> host) clock:
  0, 1  IN MAIN L/R: the Rytm's mix, which includes our OUT MAIN L/R coming back through it
  2     OUT MAIN L as the Pi sent it (written at the IN position of its send time)
  3     note markers (default): 0x10000000 at the IN frame after a MIDI note-on arrived,
        0x20000000 at the OUT frame where the voice's trigger takes effect; or IN channel `capch`
A track that plays a drum and sends a MIDI note on the same step gives, per hit:
  * the drum onset (channel 3, and in MAIN with our voice subtracted);
  * the Plaits onset as sent (channel 2) plus the OUT -> IN loop delay D, found by cross-correlating
    channel 2 with MAIN: that is where the Plaits onset sits in MAIN.
The offset (Plaits in MAIN - drum) is what a listener hears. Also writes MAIN L/R as a stereo WAV.

    uv run --with numpy python latency.py CAPTURE.wav [-s STEREO.wav]
"""
import argparse
import sys
import wave

import numpy as np

RATE = 48000


def onsets(x, rel=0.05, refractory_ms=60, jump=4.0):
    """Sample indices where |x| jumps: above rel * peak and `jump` times the 10 ms before (1 ms gap)."""
    a = np.abs(x)
    peak = a.max()
    if peak <= 0:
        return []
    thr = rel * peak
    out, nxt = [], 480
    for k in np.nonzero(a > thr)[0]:
        if k < nxt:
            continue
        if a[k] > jump * a[k - 480:k - 48].max():
            out.append(int(k))
            nxt = k + refractory_ms * RATE // 1000
    return out


def loop_delay(x, y, max_ms=50):
    """Lag (samples) of y behind x with the largest correlation, and the gain at it."""
    n = 1 << int(np.ceil(np.log2(len(x) + len(y))))
    # GCC-PHAT: the cross-spectrum whitened, so the voice's pitch (and the drum, which shares
    # its timing but not its spectrum) does not pull the peak a period off
    X = np.fft.rfft(x, n)
    cs = np.fft.rfft(y, n) * np.conj(X)
    w = np.abs(X) > 1e-3 * np.abs(X).max()		# only bins the voice has energy in
    c = np.fft.irfft(np.where(w, cs / np.maximum(np.abs(cs), 1e-30), 0), n)
    m = max_ms * RATE // 1000
    lag = int(np.argmax(c[:m]))
    xs = np.zeros_like(x)
    xs[lag:] = x[:len(x) - lag]
    g = float(np.dot(y, xs) / max(np.dot(xs, xs), 1e-30))
    corr = float(np.dot(y, xs) / np.sqrt(max(np.dot(y, y) * np.dot(xs, xs), 1e-30)))
    return lag, g, corr, xs


def dbfs(v):
    return 20 * np.log10(v) if v > 0 else float("-inf")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("wav")
    ap.add_argument("-s", "--stereo", help="write IN MAIN L/R here (32-bit stereo WAV)")
    ap.add_argument("--jump", type=float, default=4.0, help="without trigger markers: the onset jump in OUT as sent "
                    "(default 4; a voice retriggered before it decays, as DIGIDRUM at 8 notes a second, needs about 1.5)")
    args = ap.parse_args()

    with wave.open(args.wav, "rb") as w:
        ch, sw, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    if ch != 4 or sw != 4 or rate != RATE:
        sys.exit(f"expected a 4-channel 32-bit 48 kHz spike3 capture, got {ch} ch {8 * sw}-bit {rate} Hz")
    a = np.frombuffer(raw, dtype="<i4").reshape(-1, 4)
    if args.stereo:
        with wave.open(args.stereo, "wb") as w:
            w.setnchannels(2)
            w.setsampwidth(4)
            w.setframerate(RATE)
            w.writeframes(np.ascontiguousarray(a[:, :2]).tobytes())
        print(f"wrote {args.stereo} (IN MAIN L/R, {len(a) / RATE:.2f} s)")

    f = a.astype(np.float64) / 2**31
    main_ = (f[:, 0] + f[:, 1]) / 2
    sent, drum = f[:, 2], f[:, 3]
    tr = None
    for name, v in (("MAIN L", f[:, 0]), ("MAIN R", f[:, 1]), ("OUT sent", sent), ("channel 3", drum)):
        print(f"  {name:12s} peak {dbfs(np.abs(v).max()):6.1f} dBFS  RMS {dbfs(np.sqrt(np.mean(v * v))):6.1f} dBFS")

    if np.abs(sent).max() == 0:
        sys.exit("the Pi sent silence during the capture (no MIDI notes?)")
    lag, g, corr, xs = loop_delay(sent, main_)
    print(f"OUT -> IN loop: {lag} samples = {lag / RATE * 1000:.3f} ms, gain {g:.3f} "
          f"({dbfs(abs(g)):.1f} dB), correlation {corr:.3f}")
    resid = main_ - g * xs

    raw3 = a[:, 3]
    markers = set(np.unique(raw3).tolist()) <= {0, 0x10000000, 0x20000000, 0x30000000}
    tr = onsets(resid)
    if markers:
        tp = [int(k) for k in np.nonzero(raw3 & 0x20000000)[0]]
        tm = [int(k) for k in np.nonzero(raw3 & 0x10000000)[0]]
        if not tp:   # pi/engine marks only the MIDI arrivals: the voice's onsets as sent stand for its triggers
            tp = onsets(sent, jump=args.jump)
            print(f"no trigger markers: the {len(tp)} onsets in OUT as sent are the triggers")
        print(f"markers: {len(tm)} MIDI note-ons, {len(tp)} triggers; drum onsets in MAIN minus Plaits: {len(tr)}")
        if tm and tp:
            d = [(p - m) / RATE * 1000 for m, p in zip(tm, tp) if 0 <= p - m < RATE // 10]
            if d:
                print(f"MIDI arrival -> trigger sample sent: median {np.median(d):.3f} ms, max {max(d):.3f} ms")
        ref, label = tr, "the Rytm's drum on the same step (MAIN, Plaits removed)"
        offs, pre = [], []
        for t in tp:
            near = [r for r in tr if -10 * RATE // 1000 <= (t + lag) - r <= 40 * RATE // 1000]
            if near:
                r = min(near, key=lambda r: abs(t + lag - r))
                offs.append((t + lag - r) / RATE * 1000)
                m = [m for m in tm if 0 <= t - m < RATE // 10]
                if m:
                    pre.append((m[-1] - r) / RATE * 1000)
        if offs:
            o = np.array(offs)
            print(f"Plaits in MAIN after {label}: median {np.median(o):.2f} ms, min {o.min():.2f}, "
                  f"max {o.max():.2f}, n {len(o)} of {len(tp)} notes")
            print("   per hit (ms): " + " ".join(f"{v:.2f}" for v in o[:24]))
        else:
            print(f"no drum onset within -10..+40 ms of a Plaits onset in MAIN")
        if pre:
            print(f"MIDI note-on arrival after that drum onset: median {np.median(pre):.2f} ms "
                  f"(min {min(pre):.2f}, max {max(pre):.2f})")
        print("drum onsets (s): " + " ".join(f"{t / RATE:.3f}" for t in tr[:24]))
        print("triggers + loop (s): " + " ".join(f"{(t + lag) / RATE:.3f}" for t in tp[:24]))
        return
    tp = onsets(sent)
    td = onsets(drum)
    print(f"onsets: Plaits sent {len(tp)}, drum (voice ch) {len(td)}, MAIN minus Plaits {len(tr)}")
    for label, ref in (("drum on its voice channel", td), ("drum in MAIN (Plaits removed)", tr)):
        offs = []
        for t in tp:
            if not ref:
                break
            j = int(np.argmin([abs(t + lag - r) for r in ref]))
            d = t + lag - ref[j]
            if abs(d) <= 30 * RATE // 1000:
                offs.append(d / RATE * 1000)
        if offs:
            o = np.array(offs)
            print(f"Plaits in MAIN after the {label}: median {np.median(o):.2f} ms, "
                  f"min {o.min():.2f}, max {o.max():.2f}, n {len(o)}")
            print("   per hit (ms): " + " ".join(f"{v:.2f}" for v in o[:24]))
        else:
            print(f"no Plaits onset within 30 ms of a {label} onset")
    print("Plaits sent (s): " + " ".join(f"{t / RATE:.3f}" for t in tp[:16]))
    print("drum ch    (s): " + " ".join(f"{t / RATE:.3f}" for t in td[:16]))


if __name__ == "__main__":
    main()
