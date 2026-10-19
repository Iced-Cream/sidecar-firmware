#!/usr/bin/env python3
"""Convert samples to the format the Teensy streams best, as Elektron Transfer
converts for the Rytm: mono, 16-bit PCM, 48 kHz WAV (docs/sampler-plan.md).

Why: the Teensy plays any PCM WAV, but 24-bit stereo reads three times the data
of mono 16-bit from the card (8 voices of 24-bit stereo: 60 % of the Teensy with
MIX 0; the card time drops to a third in mono 16-bit), and 48 kHz needs no
resampling.

    uv run python tools/card_convert.py SRC DST          copy SRC's samples,
                                                          converted, into DST
    uv run python tools/card_convert.py --in-place DIR   convert DIR's samples
                                                          that do not match yet
                                                          (replaces them)
Both only report what they would do unless --yes is given. Options: --stereo
(keep stereo files stereo), --rate N (default 48000), --jobs N.

WAV, AIFF, FLAC and OGG are read (libsndfile); the result is always .wav. Files
already in the target format are copied as they are (DST mode) or left alone.
macOS "._" files and other dot-files are skipped. Resampling is polyphase
(scipy's resample_poly); 16-bit output is TPDF-dithered.
"""
import argparse
import os
import shutil
import sys
import tempfile
from concurrent.futures import ProcessPoolExecutor
from fractions import Fraction

import numpy as np
import soundfile as sf
from scipy.signal import resample_poly

EXTS = (".wav", ".aif", ".aiff", ".flac", ".ogg")


def wants(path):
    n = os.path.basename(path)
    return not n.startswith(".") and n.lower().endswith(EXTS)


def conforms(info, rate, stereo):
    ch_ok = info.channels == 1 or (stereo and info.channels == 2)
    return (info.format == "WAV" and info.subtype == "PCM_16" and info.samplerate == rate and ch_ok)


def convert(src, dst, rate, stereo):
    """Read src, write dst (a .wav). Returns (bytes in, bytes out)."""
    x, sr = sf.read(src, dtype="float64", always_2d=True)
    if x.shape[1] > 1 and not (stereo and x.shape[1] == 2):
        x = x.mean(axis=1, keepdims=True)          # to mono (stereo: first two kept)
    if stereo and x.shape[1] > 2:
        x = x[:, :2]
    if sr != rate:
        f = Fraction(rate, sr).limit_denominator(1000)
        x = resample_poly(x, f.numerator, f.denominator, axis=0)
    # TPDF dither to 16 bits, then clip
    rng = np.random.default_rng(len(x))
    x = x * 32767.0 + (rng.random(x.shape) - rng.random(x.shape))
    y = np.clip(np.round(x), -32768, 32767).astype(np.int16)
    os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
    fd, tmp = tempfile.mkstemp(suffix=".wav", dir=os.path.dirname(dst) or ".")
    os.close(fd)
    try:
        sf.write(tmp, y, rate, subtype="PCM_16", format="WAV")
        os.replace(tmp, dst)
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)
    return os.path.getsize(src), os.path.getsize(dst)


def job(args):
    src, dst, rate, stereo, in_place = args
    try:
        info = sf.info(src)
    except Exception as e:              # not readable as audio
        return ("bad", src, str(e).splitlines()[0][:80], 0, 0)
    if conforms(info, rate, stereo):
        if in_place:
            return ("ok", src, "", 0, 0)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(src, dst)
        size = os.path.getsize(src)
        return ("copied", src, "", size, size)
    try:
        a, b = convert(src, dst, rate, stereo)
    except Exception as e:
        return ("bad", src, str(e).splitlines()[0][:80], 0, 0)
    if in_place and os.path.abspath(dst) != os.path.abspath(src):
        os.remove(src)                  # e.g. x.aif replaced by x.wav
    desc = f"{info.channels} ch {info.subtype} {info.samplerate} Hz"
    return ("converted", src, desc, a, b)


def plan(src_root, dst_root, in_place):
    for d, dirs, files in os.walk(src_root):
        dirs[:] = sorted(x for x in dirs if not x.startswith("."))
        for f in sorted(files):
            p = os.path.join(d, f)
            if not wants(p):
                continue
            rel = os.path.relpath(p, src_root)
            out = os.path.splitext(os.path.join(dst_root, rel))[0] + ".wav"
            yield p, out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src")
    ap.add_argument("dst", nargs="?")
    ap.add_argument("--in-place", action="store_true")
    ap.add_argument("--stereo", action="store_true", help="keep stereo files stereo")
    ap.add_argument("--rate", type=int, default=48000)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--yes", action="store_true", help="do it (without: a report only)")
    a = ap.parse_args()
    if a.in_place == bool(a.dst):
        sys.exit("give SRC DST, or --in-place DIR")
    dst_root = a.src if a.in_place else a.dst
    items = list(plan(a.src, dst_root, a.in_place))
    if not a.yes:
        todo = bad = ok = 0
        before = 0
        for p, _ in items:
            try:
                info = sf.info(p)
            except Exception:
                bad += 1
                continue
            if conforms(info, a.rate, a.stereo):
                ok += 1
            else:
                todo += 1
                before += os.path.getsize(p)
        print(f"{len(items)} sample files under {a.src}: {ok} already mono/16-bit/{a.rate} Hz, "
              f"{todo} to convert ({before / 1e9:.2f} GB now), {bad} unreadable")
        print("nothing written; add --yes to " + ("convert them in place (originals replaced)"
                                                  if a.in_place else f"write the converted set to {dst_root}"))
        return
    counts = {"converted": 0, "copied": 0, "ok": 0, "bad": 0}
    bin_, bout = 0, 0
    with ProcessPoolExecutor(max_workers=a.jobs) as ex:
        for i, (what, src, note, x, y) in enumerate(
                ex.map(job, [(p, o, a.rate, a.stereo, a.in_place) for p, o in items], chunksize=8)):
            counts[what] += 1
            bin_ += x
            bout += y
            if what == "bad":
                print(f"  skipped (unreadable): {os.path.relpath(src, a.src)}: {note}")
            if (i + 1) % 500 == 0:
                print(f"  {i + 1}/{len(items)} ...", flush=True)
    print(f"done: {counts['converted']} converted, {counts['copied']} copied as they were, "
          f"{counts['ok']} already fine, {counts['bad']} unreadable; "
          f"{bin_ / 1e9:.2f} GB in -> {bout / 1e9:.2f} GB out")


if __name__ == "__main__":
    main()
