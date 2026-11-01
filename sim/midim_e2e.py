#!/usr/bin/env python3
"""End to end, emulator only: the MIDI machine (core/midim/midim.h; rytm-mods 0021 / 0026, machine 50) against the real engine core (sim/um_e2e.py's Run:
libcore through sim/um_link.py): BD on MIDI, its default DST DIN: a pad's trig goes 44 -> the core -> 4F -> the Rytm's DIN port, note-on and note-off
on the emulator's MIDI out, and the core renders nothing on the voice. Nothing is flashed; no hardware.

    RYTM_EMU=... UM_TAG=<build> python sim/midim_e2e.py        (make -C core lib; rytm-mods tools/emu/us_base.sh <build>)
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault("UM_TAG", "mi2")
import um_e2e as E  # noqa: E402

DOWNS = 29


def main():
    out = os.path.join(E.OUT, "midim.midi")
    if os.path.exists(out):
        os.remove(out)
    ev, t = E.walk_list(100, DOWNS)
    ev = list(ev) + [(t + 300, "press", "BD:100M"), (t + 700, "press", "BD:100M"), (t + 1100, "press", "PLAY"), (t + 1700, "press", "STOP")]
    r = E.Run("midim", t + 2200, ev, packages=(), extra=["--midi-out", out])
    tx = r.tx(0x4F)
    print(f"  44 reports: {len(r.rx(0x44))}, 4F messages: {[m.hex() for f, m in tx][:8]}")
    E.check("midim: the core sends 4F for the trigs", len(tx) >= 2, str(len(tx)))
    b = open(out, "rb").read() if os.path.exists(out) else b""
    print(f"  DIN out: {b.hex()}")
    # (the Rytm's own pad notes on its auto channel and its PLAY / STOP go out too, by its GLOBAL settings: 9D .. FA FC here)
    want = bytes.fromhex("903c7f")
    off = bytes.fromhex("803c00")
    E.check("midim: DIN carries each note-on and its note-off, in that order", b.count(want) == 2 and b.count(off) == 2 and
            b.index(want) < b.index(off) and b.rindex(want) < b.rindex(off), b.hex())
    v = r.voices()
    E.check("midim: the voice renders nothing", abs(v[:, 0]).max() == 0, str(abs(v[:, 0]).max()))
    print(f"\n{'ALL PASS' if not E.fails else 'FAILED: ' + ', '.join(E.fails)}")
    sys.exit(1 if E.fails else 0)


if __name__ == "__main__":
    main()
