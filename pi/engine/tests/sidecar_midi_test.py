#!/usr/bin/env python3
"""The Sidecar's USB MIDI pass-through (pi/engine/sidecar_gadget.h, midi_filter.h) from the Mac, without the Rytm: with
the Pi's console :midifake=on, what the Mac sends toward the Rytm comes back as if the Rytm had sent it, through both
filters. Checks: notes, CCs, pitch bends, clock and SysEx (non-commercial 0x7D, Elektron 00 20 3C kit-dump shape) come
back unchanged and in order; the host link (F0 7D 52 54), MIDI-CI (F0 7E|7F xx 0D) and the Elektron OS update
(F0 00 20 3C dd 00 7E|7F) never come back; then SECS of mixed traffic and a burst.

    uv run --with python-rtmidi --with sounddevice python -I pi/engine/tests/sidecar_midi_test.py [SECS]

The port: the one named "Sidecar | Expander (Pi)" (teensy/uac2/coreaudio_pick.py; the Teensy is "... (Mini)"), or
SIDECAR_NAME.
Nothing here ever reaches a Rytm: with the Rytm's MIDI connected the Pi sends these to it instead, so run it only
with the Rytm unplugged from the Pi (the script refuses when the Pi's log is not at hand: check :midifake first).
"""
import os
import random
import sys
import time

import rtmidi

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "teensy", "uac2"))
from coreaudio_pick import midi_pick, midi_ports  # noqa: E402

NAME = os.environ.get("SIDECAR_NAME", "Sidecar | Expander (Pi)")
SECS = float(sys.argv[1]) if len(sys.argv) > 1 else 10.0


def is_ci(m):
    return len(m) > 4 and m[0] == 0xF0 and m[1] in (0x7E, 0x7F) and m[3] == 0x0D


def main():
    i, o = midi_pick(None, False, NAME), midi_pick(None, True, NAME)
    if i is None or o is None:
        print("FAIL no MIDI port named", NAME, midi_ports(False))
        return 1
    mi, mo = rtmidi.MidiIn(), rtmidi.MidiOut()
    mi.ignore_types(sysex=False, timing=False, active_sense=False)
    mi.open_port(i)
    mo.open_port(o)
    rx = []
    mi.set_callback(lambda m, d: rx.append(bytes(m[0])) if not is_ci(m[0]) else None)
    time.sleep(0.5)
    rx.clear()
    ok = True

    def roundtrip(msgs, wait=0.3):
        rx.clear()
        for m in msgs:
            mo.send_message(list(m))
            time.sleep(0.002)
        time.sleep(wait)
        return list(rx)

    passing = [bytes([0x90, 60, 100]), bytes([0xB2, 7, 99]), bytes([0xE5, 0x10, 0x40]), bytes([0xF8]),
               bytes([0xF0, 0x7D, 0x01, 0x02, 0x03, 0xF7]),
               bytes([0xF0, 0x00, 0x20, 0x3C, 0x07, 0x00, 0x52] + list(range(40)) + [0xF7]),
               bytes([0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7])]      # identity request: universal, not CI
    blocked = {"host link": bytes([0xF0, 0x7D, 0x52, 0x54, 0x04, 0x00, 48, 100, 0xF7]),
               "MIDI-CI": bytes([0xF0, 0x7E, 0x7F, 0x0D, 0x70, 0x02, 1, 2, 3, 4, 5, 6, 7, 8, 0xF7]),
               "OS update data": bytes([0xF0, 0x00, 0x20, 0x3C, 0x0C, 0x00, 0x7E, 0, 1, 2, 3, 0xF7]),
               "OS update start": bytes([0xF0, 0x00, 0x20, 0x3C, 0x0C, 0x00, 0x7F, 0x01, 0xF7])}
    got = roundtrip(passing)
    print("passing:", "all back, in order" if got == passing else f"FAIL got {got}")
    ok &= got == passing
    for what, m in blocked.items():
        got = roundtrip([passing[0], m, passing[1]])
        good = got == [passing[0], passing[1]]
        print(f"{what}: {'blocked' if good else 'FAIL got ' + repr(got)}")
        ok &= good

    rnd = random.Random(2)
    sent = []
    t_end = time.perf_counter() + SECS
    rx.clear()
    while time.perf_counter() < t_end:
        k = rnd.random()
        if k < 0.5:
            m = [0x90 | rnd.randrange(16), rnd.randrange(128), rnd.randrange(128)]
        elif k < 0.8:
            m = [0xB0 | rnd.randrange(16), rnd.randrange(120), rnd.randrange(128)]
        else:
            m = [0xF0, 0x7D] + [rnd.randrange(128) for _ in range(rnd.randrange(0, 300))] + [0xF7]
        mo.send_message(m)
        sent.append(bytes(m))
        time.sleep(0.001)
    time.sleep(1.0)
    same = sum(1 for a, b in zip(rx, sent) if a == b)
    print(f"mixed: {len(sent)} sent, {len(rx)} back, {same} in order")
    ok &= len(rx) == len(sent) == same

    burst = [bytes([0x90 | (j % 16), j % 128, 64]) for j in range(1500)]
    rx.clear()
    t0 = time.perf_counter()
    for m in burst:
        mo.send_message(list(m))
    while len(rx) < len(burst) and time.perf_counter() - t0 < 5:
        time.sleep(0.01)
    same = sum(1 for a, b in zip(rx, burst) if a == b)
    print(f"burst: {len(burst)} sent, {len(rx)} back, {same} in order")
    ok &= same == len(burst)
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
