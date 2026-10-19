#!/usr/bin/env python3
"""The Pi's USB-C MIDI gadget ("Sidecar", pi/engine/gadget.h) from the Mac: the Pi echoes every USB MIDI event packet,
so everything sent must come back unchanged and in order.

    uv run --with mido --with python-rtmidi python -I pi/engine/tests/gadget_midi_test.py [SECS] [--port NAME]

Phases: (1) round-trip latency, one note-on at a time (N=500); (2) SECS seconds (default 10) of mixed traffic,
notes, CCs, pitch bends and SysEx of 1..300 bytes, at about 1000 messages/s, checked message by message; (3) a burst
of 2000 note messages as fast as rtmidi sends them (8000 B: within the gadget's 8 KB IN queue). Exit status 0 when
every check passed (PASS / FAIL lines).

macOS 26 sends MIDI-CI (universal SysEx F0 7E xx 0D ...) to a MIDI device when a port opens, and the echo brings it
back as if the device had sent it: those are dropped here. A MIDI-CI message coming back from the device also wedges
CoreMIDI's port (measured: no more input, and output stops), so the test's SysEx carries the non-commercial ID 0x7D,
never a universal one.
"""
import argparse
import random
import sys
import time

import mido


def find(names, want):
    hits = [n for n in names if want.lower() in n.lower()]
    return hits[0] if hits else None


def is_ci(b):
    return len(b) > 4 and b[0] == 0xF0 and b[1] in (0x7E, 0x7F) and b[3] == 0x0D


def pending(inp):
    return [bytes(r.bytes()) for r in inp.iter_pending() if not is_ci(bytes(r.bytes()))]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("secs", nargs="?", type=float, default=10.0)
    ap.add_argument("--port", default="Sidecar")
    a = ap.parse_args()
    iname, oname = find(mido.get_input_names(), a.port), find(mido.get_output_names(), a.port)
    if not iname or not oname:
        print("FAIL no MIDI port named", a.port, mido.get_input_names(), mido.get_output_names())
        return 1
    print("ports:", iname, "/", oname)
    ok = True
    with mido.open_input(iname) as inp, mido.open_output(oname) as out:
        time.sleep(0.5)                   # macOS's MIDI-CI discovery on the open, and its echo
        for _ in inp.iter_pending():
            pass
        # (1) latency
        lat = []
        for i in range(500):
            m = mido.Message("note_on", channel=i % 16, note=i % 128, velocity=1 + i % 127)
            t0 = time.perf_counter()
            out.send(m)
            r = None
            while time.perf_counter() - t0 < 0.5:
                r = inp.poll()
                if r is not None and not is_ci(bytes(r.bytes())):
                    break
                r = None
            if r is None or bytes(r.bytes()) != bytes(m.bytes()):
                print("FAIL latency probe", i, "sent", m, "got", r)
                ok = False
                break
            lat.append((time.perf_counter() - t0) * 1000)
        if lat:
            lat.sort()
            print("latency ms: min %.3f median %.3f p99 %.3f max %.3f (%d)" %
                  (lat[0], lat[len(lat) // 2], lat[int(len(lat) * 0.99)], lat[-1], len(lat)))
        # (2) mixed traffic, checked in order
        rnd = random.Random(1)
        sent, got, bad, nbytes = [], 0, 0, 0
        t_end = time.perf_counter() + a.secs
        def drain():
            nonlocal got, bad
            for r in pending(inp):
                if got >= len(sent) or r != sent[got]:
                    if bad < 5:
                        print("mismatch at", got, "got", r[:12], "want", sent[got][:12] if got < len(sent) else None)
                    bad += 1
                got += 1
        while time.perf_counter() < t_end:
            k = rnd.random()
            if k < 0.5:
                m = mido.Message("note_on", channel=rnd.randrange(16), note=rnd.randrange(128), velocity=rnd.randrange(128))
            elif k < 0.75:
                m = mido.Message("control_change", channel=rnd.randrange(16), control=rnd.randrange(120), value=rnd.randrange(128))
            elif k < 0.9:
                m = mido.Message("pitchwheel", channel=rnd.randrange(16), pitch=rnd.randrange(-8192, 8192))
            else:
                m = mido.Message("sysex", data=[0x7D] + [rnd.randrange(128) for _ in range(rnd.randrange(0, 300))])
            out.send(m)
            sent.append(bytes(m.bytes()))
            nbytes += len(sent[-1])
            drain()
            time.sleep(0.001)
        t1 = time.perf_counter()
        while got < len(sent) and time.perf_counter() - t1 < 2:
            drain()
            time.sleep(0.001)
        print("mixed: %d messages (%d B) sent, %d back, %d mismatched" % (len(sent), nbytes, got, bad))
        if got != len(sent) or bad:
            ok = False
        # (3) burst
        burst = [bytes(mido.Message("note_on" if i % 2 == 0 else "note_off", channel=i % 16, note=(i // 2) % 128,
                                    velocity=64).bytes()) for i in range(2000)]
        t0 = time.perf_counter()
        back = []
        for b in burst:
            out.send(mido.Message.from_bytes(b))
            back += pending(inp)
        while len(back) < len(burst) and time.perf_counter() - t0 < 5:
            back += pending(inp)
            time.sleep(0.001)
        dt = time.perf_counter() - t0
        same = sum(1 for x, y in zip(back, burst) if x == y)
        print("burst: %d sent, %d back, %d in order, %.0f messages/s round trip" % (len(burst), len(back), same, len(back) / dt))
        if same != len(burst):
            ok = False
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
