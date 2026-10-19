#!/usr/bin/env python3
"""The Teensy side, run on the Mac against rytm-emu's USB MIDI ports.

    teensy_sim.py listen                 print what the Rytm sends (USB MIDI out)
    teensy_sim.py stream [--voices N] [--secs S] [--hz F]
                                         stream sine audio blocks as RT AUDIO
                                         SysEx, paced in real time, and report
                                         the rate achieved

Start the emulator with its USB MIDI ports first, e.g.
    rytm FW --card C --snapshot S                (the window), or
    rytm FW ... --headless --midi --after 10     (no window)
Ports: "rytm-emu usb in" (into the Rytm) and "rytm-emu usb out" (from it).
"""
import argparse
import math
import sys
import time

import mido

import protocol

IN_NAME = "rytm-emu usb in"    # we send into the Rytm here
OUT_NAME = "rytm-emu usb out"  # the Rytm's USB MIDI output


def find(names, want):
    for n in names:
        if want in n:
            return n
    sys.exit(f"no MIDI port containing {want!r}; have: {names}")


def listen(args):
    with mido.open_input(find(mido.get_input_names(), OUT_NAME)) as port:
        print("listening; Ctrl-C to stop")
        for m in port:
            if m.type == "sysex":
                raw = bytes([0xF0] + list(m.data) + [0xF7])
                p = protocol.parse(raw)
                print("RT", p if p else raw.hex(" "))
            elif m.type != "clock":
                print(m)


def stream(args):
    port = mido.open_output(find(mido.get_output_names(), IN_NAME))
    block_s = protocol.BLOCK / 48000.0
    t0 = time.perf_counter()
    blocks = int(args.secs / block_s)
    sent = 0
    phase = [0.0] * args.voices
    late = 0
    for b in range(blocks):
        for v in range(args.voices):
            hz = args.hz * (1 + v / 4)
            s = []
            for _ in range(protocol.BLOCK):
                s.append(6000 * math.sin(phase[v]))
                phase[v] += 2 * math.pi * hz / 48000
            msg = protocol.audio_block(v, b, s)
            port.send(mido.Message("sysex", data=msg[1:-1]))
            sent += len(msg)
        due = t0 + (b + 1) * block_s
        now = time.perf_counter()
        if now < due:
            time.sleep(due - now)
        else:
            late += 1
    dt = time.perf_counter() - t0
    port.close()
    print(f"{blocks} blocks x {args.voices} voices, {sent} bytes in {dt:.2f} s = "
          f"{sent / dt / 1000:.0f} KB/s (needed {args.voices * 72 * 1500 / 1000:.0f} KB/s); "
          f"{late} blocks behind schedule")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("listen")
    s = sub.add_parser("stream")
    s.add_argument("--voices", type=int, default=1)
    s.add_argument("--secs", type=float, default=3.0)
    s.add_argument("--hz", type=float, default=220.0)
    args = ap.parse_args()
    {"listen": listen, "stream": stream}[args.cmd](args)


if __name__ == "__main__":
    main()
