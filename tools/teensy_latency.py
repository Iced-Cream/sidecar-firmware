#!/usr/bin/env python3
"""A latency capture from the Teensy host build (voiceloop, ':cap=MS'), as spike3/latency.py's 4-channel WAV.

The Teensy (USB host of a stock Rytm, out=main, notes=1) sends MS ms of the Overbridge IN stream as "W" log
lines (usb_host.h, "Latency capture"): IN MAIN L/R, OUT slot 0 as sent at that IN frame, and per-frame marks
(MIDI note-on arrivals, voice triggers). This keeps one MIDI channel's marks (--channel N: arrivals on channel
N, voice N-1's triggers) and writes channels 0..3 = MAIN L, MAIN R, OUT sent, markers (0x10000000 arrival,
0x20000000 trigger), all int32 on the IN clock, then runs pi/circle/spike3/latency.py on it: the voice in MAIN
after the Rytm's own sound on the same step (a drum, or the metronome), the OUT -> IN loop, MIDI arrival ->
trigger.

    uv run python tools/teensy_latency.py --port /dev/cu.usbmodemXXXX [--ms 20000] [--channel 4] [-o cap.wav]
"""
import argparse
import base64
import os
import subprocess
import sys
import time
import wave

import numpy as np
import serial

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def lines(s, until, secs):
    """The Teensy's log lines, plain or 'L'-framed, until a line starting with `until` or `secs` pass."""
    buf, t0 = b"", time.time()
    while time.time() - t0 < secs:
        buf += s.read(1 << 16)
        while buf:
            if buf[:1] == b"L" and len(buf) >= 2:
                n = buf[1]
                if len(buf) < 2 + n:
                    break
                ln, buf = buf[2:2 + n], buf[2 + n:]
            elif buf[:1] == b"X" and len(buf) >= 2:   # a framed copy of what went to the Rytm: skipped
                n = buf[1]
                if len(buf) < 2 + n:
                    break
                buf = buf[2 + n:]
                continue
            else:
                i = buf.find(b"\n")
                if i < 0:
                    break
                ln, buf = buf[:i], buf[i + 1:]
            ln = ln.decode(errors="replace").rstrip("\r")
            yield ln
            if ln.startswith(until):
                return


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", required=True)
    ap.add_argument("--ms", type=int, default=20000)
    ap.add_argument("--channel", type=int, default=4, help="MIDI channel whose marks are kept (1..8; default 4)")
    ap.add_argument("-o", "--out", default="teensy_cap.wav")
    ap.add_argument("--jump", default="1.5", help="passed to latency.py")
    a = ap.parse_args()
    bit = 1 << (a.channel - 1)
    chunks, nxt, head, done = [], 0, None, None
    with serial.Serial(a.port, 115200, timeout=0.05) as s:
        s.reset_input_buffer()
        s.write(f":cap={a.ms}\n".encode())
        for ln in lines(s, "cap: done", a.ms / 1000 + 20):
            if ln.startswith("W "):
                _, k, b = ln.split(" ", 2)
                k = int(k, 16)
                if k != nxt:
                    sys.exit(f"frame {nxt} expected, line starts at {k}: lines lost on the serial side")
                d = np.frombuffer(base64.b64decode(b), dtype="<i2").reshape(-1, 4)
                chunks.append(d)
                nxt += len(d)
            elif ln.startswith("cap: "):
                print(ln)
                if ln.startswith("cap: done"):
                    done = ln
                elif head is None:
                    head = ln
                if "no " in ln or "running" in ln:
                    sys.exit(1)
    if not done:
        sys.exit("no 'cap: done' line (the capture did not finish)")
    if not done.endswith(" 0 lost"):
        sys.exit("frames were lost on the Teensy (its ring overflowed): the capture is void")
    d = np.concatenate(chunks).astype(np.int32)
    m = d[:, 3] & 0xFFFF
    out = np.zeros((len(d), 4), dtype="<i4")
    out[:, 0] = d[:, 0] << 16
    out[:, 1] = d[:, 1] << 16
    out[:, 2] = d[:, 2] << 16
    out[:, 3] = np.where(m & bit, 0x10000000, 0) | np.where((m >> 8) & bit, 0x20000000, 0)
    print(f"{len(d)} frames ({len(d) / 48000:.2f} s); channel {a.channel}: {int(np.count_nonzero(m & bit))} "
          f"note-on arrivals, {int(np.count_nonzero((m >> 8) & bit))} triggers; all channels: arrivals "
          f"{int(np.count_nonzero(m & 0xFF))} frames")
    with wave.open(a.out, "wb") as w:
        w.setnchannels(4)
        w.setsampwidth(4)
        w.setframerate(48000)
        w.writeframes(out.tobytes())
    print(f"wrote {a.out}")
    subprocess.run([sys.executable, os.path.join(ROOT, "pi/circle/spike3/latency.py"), a.out, "--jump", a.jump],
                   check=False)


if __name__ == "__main__":
    main()
