#!/usr/bin/env python3
"""End to end, emulator only: a package with 36-name lists (plugins/modele, branch modeld) on the real core against the Rytm's
mods: UM_TAG a 0.4.0 build holds all 36; UM_OLD a 0.3.x build gets 16 (core/user/um.cpp), the knob's long name says so, no fault.

    RYTM_EMU=... UM_TAG=<new build> UM_OLD=rel0-3-0 python sim/enum36_e2e.py     (make -C core lib; plugins/modele/modele.a64.rmp)
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import um_e2e as E  # noqa: E402

P = E.P
OLD = os.environ.get("UM_OLD", "rel0-3-0")


def run(tag, name):
    ev, t = E.open_menu(300)
    ev += [(t, "press", "YES"), (t + 300, "press", "NO"), (t + 400, "press", "NO")]
    ev2, t = E.walk_list(t + 600, 15)
    ev += ev2 + [(t + 200, "press", "BD"), (t + 500, "press", "BD")]
    r = E.Run(name, t + 900, ev, packages=("modele",), tag=tag)
    parts = [P.decode_meta(m) for f, m in r.tx(0x1A)]
    kn = [k for x in parts if x and "knobs" in x and x["slot"] == 1 for k in x["knobs"] if k["p"] == 1]
    ls = [P.decode_list(m) for f, m in r.tx(0x1B)]
    p1 = [x for x in ls if x and x["slot"] == 1 and x["list"] == 0]
    trigs = [m for f, m in r.rx(0x04) if m[0] & 0x78 == 0x50]
    v = r.voices()
    return kn, p1, trigs, abs(v[:, 0]).max()


def main():
    kn, p1, tr, pk = run(E.TAG, "enum36-new")
    names = [n for x in p1[-5:] for _, n in x["entries"]]
    E.check("new mods: 36 names served and asked (P1's 1A count 36, five 1Bs)", kn and kn[-1]["enum_count"] == 36 and len(names) == 36 and p1[-1]["total"] == 36, f"{kn[-1:]} {len(names)}")
    E.check("new mods: BD plays MODEL E", bool(tr) and pk > 0.01, f"{len(tr)} {pk}")
    kn, p1, tr, pk = run(OLD, "enum36-old")
    names = [n for x in p1[-2:] for _, n in x["entries"]]
    E.check("0.3.x mods: 16 names, every 2.25th, the long name says it", kn and kn[-1]["enum_count"] == 16 and kn[-1]["long"].endswith("16/36") and p1[-1]["total"] == 16 and len(names) == 16,
            f"{kn[-1:]} {names}")
    E.check("0.3.x mods: no fault, BD plays MODEL E", bool(tr) and pk > 0.01, f"{len(tr)} {pk}")
    print("  old list:", names)
    print(f"\n{'ALL PASS' if not E.fails else 'FAILED: ' + ', '.join(E.fails)}")
    print(f"  look at {E.OUT}/enum36-old.png")
    sys.exit(1 if E.fails else 0)


if __name__ == "__main__":
    main()
