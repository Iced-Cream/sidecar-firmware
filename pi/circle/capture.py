#!/usr/bin/env python3
"""Record 10 s of the Rytm's MAIN L/R from the Pi 4 spike2 kernel over the serial cable.

Sends 'w'; spike2 captures the next 10 s of Overbridge IN audio (Rytm -> host channels 0/1,
the raw int32 samples) and dumps it as
    WAV-BEGIN <bytes> <rate> <channels> <gaps>
    <base64, 76 characters a line>
    WAV-END <crc32>
with its log lines (printed here) possibly in between. Writes a 32-bit WAV and prints
duration, peak/RMS dBFS per channel and the gap count. spike3 sends 4 channels (IN MAIN L/R,
OUT MAIN L as sent, one IN voice channel: spike3/latency.py), which takes about 2 minutes.

Usage (from pi/circle; nothing else may hold the port, e.g. stop flash.sh's monitor):
    uv run --with pyserial python capture.py [-p /dev/cu.usbserial-XXXX] [-o out/x.wav]
"""
import argparse
import array
import base64
import binascii
import glob
import math
import os
import re
import sys
import time
import wave

B64_LINE = re.compile(rb"^[A-Za-z0-9+/]+={0,2}$")
LINE_BYTES = 57  # the kernel's base64 line: 76 characters


def find_port():
    ports = sorted(glob.glob("/dev/cu.usbserial-*"))
    if not ports:
        sys.exit("error: no serial adapter (/dev/cu.usbserial-*); pass -p")
    return ports[0]


def readline(ser, deadline, what):
    buf = b""
    while True:
        if time.monotonic() > deadline:
            sys.exit(f"error: timed out waiting for {what}")
        b = ser.readline()
        if not b:
            continue
        buf += b
        if buf.endswith(b"\n"):
            return buf.strip(b"\r\n")


def stats(pcm, ch):
    a = array.array("i")
    a.frombytes(pcm)
    if sys.byteorder != "little":
        a.byteswap()
    out = []
    for c in range(ch):
        s = a[c::ch]
        peak = max((abs(v) for v in s), default=0)
        rms = math.sqrt(sum(v * v for v in s) / len(s)) if len(s) else 0.0
        db = lambda x: 20 * math.log10(x / 2147483648) if x > 0 else float("-inf")
        out.append((db(peak), db(rms)))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-p", "--port", help="serial port (default: the first /dev/cu.usbserial-*)")
    ap.add_argument("-b", "--baud", type=int, default=921600)
    ap.add_argument("-o", "--out", help="WAV file (default out/capture-<time>.wav)")
    ap.add_argument("-t", "--timeout", type=float, default=240.0,
                    help="seconds for the whole run: 10 s capture + the dump, ~30 s for 2 channels, "
                         "~2 min for 4 (default 240)")
    args = ap.parse_args()

    try:
        import serial
    except ImportError:
        sys.exit("error: needs pyserial: uv run --with pyserial python capture.py")

    port = args.port or find_port()
    out = args.out or os.path.join(os.path.dirname(os.path.abspath(__file__)), "out",
                                   time.strftime("capture-%Y%m%d-%H%M%S.wav"))
    try:
        ser = serial.Serial(port, args.baud, timeout=0.5)
    except serial.SerialException as e:
        sys.exit(f"error: cannot open {port}: {e} (is flash.sh's monitor still running?)")

    deadline = time.monotonic() + args.timeout
    with ser:
        ser.reset_input_buffer()
        ser.write(b"w")
        ser.flush()
        print(f"sent 'w' on {port} at {args.baud}; waiting for the capture (10 s) and the dump")

        # log lines until WAV-BEGIN
        start_wait = time.monotonic() + 20
        while True:
            line = readline(ser, min(deadline, start_wait), "WAV-BEGIN (kernel not spike2 at this baud?)")
            if line.startswith(b"WAV-BEGIN"):
                break
            if b"WAV-ERROR" in line:
                sys.exit(f"error: kernel says: {line.decode(errors='replace')}")
            print(line.decode(errors="replace"))
        try:
            _, nbytes, rate, ch, gaps = line.split()
            nbytes, rate, ch, gaps = int(nbytes), int(rate), int(ch), int(gaps)
        except ValueError:
            sys.exit(f"error: bad header: {line!r}")
        print(f"dump: {nbytes} B, {rate} Hz, {ch} ch, {gaps} gaps during capture")

        chunks, got, t0, holes = [], 0, time.monotonic(), 0
        last_report = t0
        while True:
            line = readline(ser, deadline, f"WAV-END ({got} of {nbytes} B so far)")
            if line.startswith(b"WAV-END"):
                break
            if not line:
                continue
            if B64_LINE.match(line) and len(line) % 4 == 0:
                try:
                    data = base64.b64decode(line, validate=True)
                except binascii.Error:
                    data = None
                if data is None or (len(data) != LINE_BYTES and got + len(data) != nbytes):
                    # garbled (bytes lost on the line: two lines merged): a hole of the lines it
                    # spans, zero-filled so the frames after it stay aligned
                    k = max(1, round(len(line) / 76))
                    data = bytes(LINE_BYTES * k)
                    holes += k
                chunks.append(data)
                got += len(data)
                now = time.monotonic()
                if now - last_report >= 5:
                    last_report = now
                    print(f"  {got * 100 // nbytes}% ({got} B, {now - t0:.0f} s)")
            elif len(line) >= 60 and re.match(rb"^[A-Za-z0-9+/=]+", line) and b" " not in line[:60]:
                k = max(1, round(len(line) / 76))
                chunks.append(bytes(LINE_BYTES * k))
                got += LINE_BYTES * k
                holes += k
            else:
                print(line.decode(errors="replace"))  # a kernel log line

    pcm = b"".join(chunks)
    try:
        crc = int(line.split()[1], 16)
    except (IndexError, ValueError):
        sys.exit(f"error: bad trailer: {line!r}")
    if holes:
        # a long dump (spike3: 7.7 MB, ~2 min) can lose bytes on the serial line
        pcm = (pcm + bytes(nbytes))[:nbytes]
        print(f"warning: {holes} garbled serial line(s) zero-filled ({holes * LINE_BYTES} B, "
              f"{holes * LINE_BYTES / (4 * ch) / rate * 1000:.1f} ms of audio); crc not checked")
    elif len(pcm) != nbytes:
        sys.exit(f"error: got {len(pcm)} B, header said {nbytes} (lost or garbled serial lines)")
    mine = binascii.crc32(pcm) & 0xFFFFFFFF
    if mine != crc and not holes:
        sys.exit(f"error: crc32 {mine:08x} != kernel's {crc:08x} (serial corruption)")

    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with wave.open(out, "wb") as w:
        w.setnchannels(ch)
        w.setsampwidth(4)
        w.setframerate(rate)
        w.writeframes(pcm)

    frames = nbytes // (4 * ch)
    print(f"wrote {out}: {frames / rate:.3f} s, crc {crc:08x} {'not checked' if holes else 'ok'}, dump took {time.monotonic() - t0:.1f} s")
    for c, (pk, rms) in enumerate(stats(pcm, ch)):
        print(f"  {'LR'[c] if ch == 2 else ('L', 'R', 'out', 'voice')[c] if ch == 4 else c}: peak {pk:.1f} dBFS, RMS {rms:.1f} dBFS")
    print(f"gaps during capture: {gaps}" + ("" if gaps == 0 else "  (audio has holes)"))


if __name__ == "__main__":
    main()
