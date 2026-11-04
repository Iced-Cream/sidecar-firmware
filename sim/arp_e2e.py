#!/usr/bin/env python3
"""End to end, emulator only: the arpeggiator (core/arp/arp.h; rytm-mods 0021 / 0026) against the real engine core (sim/um_e2e.py's Run: libcore through sim/um_link.py): BD on the
MIDI machine, a code written into its sound by 45, the Rytm says it back, PLAY, the pad held: the core asks for BD's ticks and pins nothing (MIDI), the trig's note comes out on the
Rytm's DIN port as a pattern over two octaves, at the sequencer's clock, nothing left open after STOP. Nothing is flashed; no hardware.

    RYTM_EMU=... UM_TAG=<build> python sim/arp_e2e.py        (make -C core lib; rytm-mods tools/emu/us_base.sh <build>)
"""
import json
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault("UM_TAG", "ar2")
os.environ["RUI_SEQ_TICK"] = "1"   # the control loop's sequencer tick: its 2E
import um_e2e as E  # noqa: E402

DOWNS = 29
CODE = 37      # UP, 1/16, 2 octaves, gate 50 %: core/arp/arp.h


def main():
    out = os.path.join(E.OUT, "arp.midi")
    if os.path.exists(out):
        os.remove(out)
    f = tempfile.NamedTemporaryFile(suffix=".syx", delete=False)
    f.write(bytes.fromhex("f07d525445") + bytes([1, 0, CODE, 0, 0xF7]))
    f.close()
    ev, t = E.walk_list(100, DOWNS)
    ev = list(ev) + [(t + 500, "press", "PLAY"), (t + 700, "press", "BD:300M"), (t + 1400, "press", "STOP")]
    clock = json.load(open(os.path.join(E.EMU, "sections", f"us_{E.TAG}.json")))["clock"]
    r = E.Run("arp", t + 2000, ev, packages=(), extra=["--midi-out", out, "--usb-midi-file", f"{f.name}@{clock + t + 200}M"])
    rx = r.rx(0x45)
    print(f"  45s from the Rytm: {[m.hex() for _, m in rx][:14]}")
    E.check("arp: the Rytm says the code back", any(m[1:4] == bytes([0, CODE, 0]) for _, m in rx), str(len(rx)))
    ticks = [f for f, m in r.rx(0x2C)]
    print(f"  2C ticks: {len(ticks)}")
    E.check("arp: BD's ticks come (the core's 2E asked for them)", len(ticks) >= 10, str(len(ticks)))
    tx = r.tx(0x4F)
    print(f"  4F: {len(tx)}: {[m.hex() for f, m in tx][:10]}")
    ons = [(f, m[2]) for f, m in tx if m[1] & 0x70 == 0x10]
    offs = [(f, m[2]) for f, m in tx if m[1] & 0x70 == 0x00]
    print(f"  on frames {[f for f, _ in ons][:12]} notes {[n for _, n in ons][:12]}")
    E.check("arp: a pattern, not one note", len(ons) >= 6, str(len(ons)))
    E.check("arp: both octaves, in turn", [n for _, n in ons][:4] == [60, 72, 60, 72], str([n for _, n in ons][:4]))
    d = [b[0] - a[0] for a, b in zip(ons[1:], ons[2:])]
    per = (ticks[-1] - ticks[0]) / (len(ticks) - 1)
    print(f"  intervals {d}, a tick's period {per:.1f}")
    E.check("arp: one note a step of the Rytm's own tempo", abs(sorted(d)[len(d) // 2] - per) < per * 0.01, str(d))
    E.check("arp: even (the link's chunks move the clock a little)", all(abs(x - per) < 300 for x in d), str(d))
    ph = [(on - ticks[0] + per / 2) % per - per / 2 for on, _ in ons[1:]]
    print(f"  note-ons against the ticks: {[round(x) for x in ph]}")
    E.check("arp: on the ticks' grid, not drifting", max(ph) - min(ph) < 960, str(ph))
    E.check("arp: every note-on has its note-off", len(offs) == len(ons), f"{len(ons)} on, {len(offs)} off")
    b = open(out, "rb").read() if os.path.exists(out) else b""
    E.check("arp: on the DIN port", b.count(bytes.fromhex("903c")) >= 3 and b.count(bytes.fromhex("9048")) >= 3, b.hex()[:120])
    print(f"\n{'ALL PASS' if not E.fails else 'FAILED: ' + ', '.join(E.fails)}")
    sys.exit(1 if E.fails else 0)


if __name__ == "__main__":
    main()
