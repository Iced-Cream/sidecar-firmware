#!/usr/bin/env python3
"""End to end, emulator only: the Rytm's MD page (rytm-mods 0021, machine 42; docs/MD-BUILTIN.md) against the real engine
core (sim/um_e2e.py's Run: libcore through sim/um_link.py, core/md's on_ask and voices): the Rytm asks, the core answers,
the Rytm's records carry MD's labels, a trig plays MD. Nothing is flashed; no hardware.

    RYTM_EMU=... UM_TAG=<build> python sim/md_e2e.py        (make -C core lib; rytm-mods tools/emu/us_base.sh <build>)
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault("UM_TAG", "mdpage")
import um_e2e as E  # noqa: E402

DOWNS_MD = 22
BASE, COUNT = (0, 8, 17), (8, 9, 13)


def peek(snap, addr, n):
    r = subprocess.run([os.path.join(E.EMU, "target", "release", "dtpeek"), snap, "mem", f"{addr:x}", str(n)], cwd=E.EMU,
                       env=dict(os.environ, DTEMU_FW=E.FW), capture_output=True, text=True, check=True)
    return bytes(int(x, 16) for line in r.stdout.splitlines() for x in line.split(":", 1)[1].split())


def rec_short(snap, r):
    p = int.from_bytes(peek(snap, 0x401bf918 + 0x34 * r + 48, 4), "big")
    return peek(snap, p, 5).split(b"\0")[0].decode()


def main():
    snap = os.path.join(E.OUT, "md.snap")
    ev, t = E.walk_list(100, DOWNS_MD)
    ev = list(ev) + [(t + 300, "press", "SMPL"), (t + 500, "press", "BD")]
    r = E.Run("md", t + 800, ev, packages=(), save=snap)
    ask = r.rx(0x19)
    tx = r.tx(0x1A)
    md = [m for f, m in tx if m[1] == 0x40]
    E.check("md: the Rytm asks on slot 0x40, the core answers head + 4 parts", any(m[1] == 0x40 for f, m in ask) and len(md) >= 5,
            f"{len(ask)} asks, {len(md)} 1A parts")
    lists = [m for f, m in r.tx(0x1B) if m[1] == 0x40]
    E.check("md: both name lists answered (ENGINE, TYPE)", {m[2] for m in lists} >= {8, 9}, str([m[:6].hex() for m in lists]))
    got = [rec_short(snap, 390 + k) for k in range(8)]
    E.check("md: BD's SRC records carry FM BD's labels from the core", got == ["PTCH", "DEC", "RAMP", "RDEC", "MOD", "MFRQ", "MDEC", "MFB"], str(got))
    tr = [m for f, m in r.rx(0x04) if m[0] & 0x78 == 0x40]
    E.check("md: BD's trig: 0x40 with the MD tag", bool(tr) and tr[-1][-2:] == b"\x44\x4d", tr[-1].hex() if tr else "none")
    v = r.voices()
    E.check("md: the core plays it: voice 0 sounds", abs(v[:, 0]).max() > 0.01, str(abs(v[:, 0]).max()))
    print(f"\n{'ALL PASS' if not E.fails else 'FAILED: ' + ', '.join(E.fails)}")
    print(f"  look at {E.OUT}/md.png (the MD ENGINE page with the core's lists)")
    sys.exit(1 if E.fails else 0)


if __name__ == "__main__":
    main()
