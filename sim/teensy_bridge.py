#!/usr/bin/env python3
"""Run the real Teensy's voice loop (teensy/voiceloop) against rytm-emu.

    teensy_bridge.py [--ext TRACKS] [--link HOST:PORT] [--port /dev/cu.usbmodemX]

Does obhost.py's job with the synthesis on the Teensy: relays the EXT machine's
SysEx reports from "rytm-emu usb out" to the Teensy (and its card-name answers
back to "rytm-emu usb in"), and rytm-emu's audio
requests over the Overbridge link (rytm-emu --ob-link) to the Teensy, whose 8
voice channels come back and go into the Rytm's per-voice inputs. The Teensy's
log lines (trigs, CPU load every 2 s) are printed.

Start it in any order: it waits for the Teensy's serial port and for the
Teensy to finish booting (its card index: up to a few seconds after a card
change), then for rytm-emu's MIDI ports and Overbridge link, retrying with a
message until they are there.
"""
import argparse
import glob
import socket
import struct
import sys
import threading
import time

import mido
import numpy as np
import serial

from obhost import EXT_MACHINE, VOICE_CH


def wait_for(get_names, want, what):
    """The first name from get_names() containing want, waiting (retrying each
    second, a message once) until one appears."""
    said = False
    while True:
        for n in get_names():
            if want in n:
                if said:
                    print(f"{what}: found {n!r}")
                return n
        if not said:
            print(f"waiting for {what} ({want!r}; start rytm-emu with --ob-link) ...", flush=True)
            said = True
        time.sleep(1)


class Teensy:
    def __init__(self, port):
        self.s = serial.Serial(port, 115200, timeout=2)
        self.wlock = threading.Lock()
        self.s.reset_input_buffer()
        self.to_rytm = None          # the Rytm's USB MIDI in, for 'X' messages
        self.held = []               # 'X' messages read before to_rytm opened (the boot 1D), relayed then

    def wait_ready(self):
        """Until the Teensy answers a request: it answers none while it boots
        (mounting the card, checking or rebuilding its index; a rebuild takes
        seconds). One request, waited for as long as it takes."""
        t0 = time.time()
        said = False
        with self.wlock:
            self.s.write(b"N" + struct.pack("<H", 32))
        while True:
            k = self.s.read(1)
            if not k:
                if not said:
                    print("waiting for the Teensy to finish booting ...", flush=True)
                    said = True
                continue
            if k == b"L":
                ln = self.s.read(1)[0]
                print("teensy:", self.s.read(ln).decode(errors="replace"))
            elif k == b"X":
                ln = self.s.read(1)[0]
                self.held.append(self.s.read(ln))
            elif k == b"A":
                m = struct.unpack("<H", self.s.read(2))[0]
                self.s.read(m * 16)
                print(f"teensy ready ({time.time() - t0:.1f} s)")
                return

    def open_rytm(self, port):
        """The Rytm's USB MIDI in: the 'X' messages held since boot first (the boot 1D, when the Teensy booted
        while the bridge waited; one sent before the bridge started is lost with the serial data - the Teensy
        answers the Rytm's next 10 <project> with a 1D anyway)."""
        self.to_rytm = port
        for m in self.held:
            if len(m) > 2:
                port.send(mido.Message("sysex", data=list(m[1:-1])))
        self.held = []

    def send_report(self, data):
        msg = bytes([0xF0]) + bytes(data) + bytes([0xF7])
        with self.wlock:
            self.s.write(b"S" + bytes([len(msg)]) + msg)

    def send_transport(self, running):
        """'T' <1|0>: the Rytm's MIDI Start/Continue (1) or Stop (0), so the Teensy installs nothing while it plays."""
        with self.wlock:
            self.s.write(b"T" + bytes([1 if running else 0]))

    def frames(self, n):
        """n frames x 8 voices, int16; prints log lines met on the way."""
        with self.wlock:
            self.s.write(b"N" + struct.pack("<H", n))
        while True:
            k = self.s.read(1)
            if not k:
                raise TimeoutError("Teensy did not answer")
            if k == b"L":
                ln = self.s.read(1)[0]
                print("teensy:", self.s.read(ln).decode(errors="replace"))
            elif k == b"X":           # SysEx for the Rytm (card names)
                ln = self.s.read(1)[0]
                m = self.s.read(ln)
                if self.to_rytm is not None and len(m) > 2:
                    self.to_rytm.send(mido.Message("sysex", data=list(m[1:-1])))
            elif k == b"A":
                m = struct.unpack("<H", self.s.read(2))[0]
                raw = self.s.read(m * 16)
                if len(raw) != m * 16:
                    raise TimeoutError("short audio block")
                return np.frombuffer(raw, "<i2").reshape(m, 8)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--link", default="127.0.0.1:7301")
    ap.add_argument("--ext", default="", help="tracks to set to EXT, e.g. 0,1,8")
    ap.add_argument("--port", default=None)
    ap.add_argument("--midi-from", default="rytm-emu usb out")
    ap.add_argument("--midi-to", default="rytm-emu usb in")
    a = ap.parse_args()
    port = a.port or wait_for(lambda: sorted(glob.glob("/dev/cu.usbmodem*")), "/dev/cu.usbmodem",
                              "the Teensy's serial port (micro-USB cable)")
    t = None
    while t is None:
        try:
            t = Teensy(port)
        except serial.SerialException as e:   # just enumerated, or busy
            print(f"{port}: {e}; retrying")
            time.sleep(1)
    print(f"teensy on {port}")
    t.wait_ready()
    if a.ext:
        with mido.open_output(wait_for(mido.get_output_names, "rytm-emu in", "rytm-emu's MIDI in")) as o:
            for tr in (int(x) for x in a.ext.split(",")):
                o.send(mido.Message("control_change", channel=tr, control=15, value=EXT_MACHINE))
                print(f"track {tr} -> EXT")

    def on_midi(m):
        if m.type == "sysex" and list(m.data[:3]) == [0x7D, 0x52, 0x54]:
            t.send_report(m.data)
        elif m.type in ("start", "continue"):
            t.send_transport(True)
        elif m.type == "stop":
            t.send_transport(False)

    inp = mido.open_input(wait_for(mido.get_input_names, a.midi_from, "rytm-emu's USB MIDI out"),
                          callback=on_midi)
    t.open_rytm(mido.open_output(wait_for(mido.get_output_names, a.midi_to, "rytm-emu's USB MIDI in")))
    h, p = a.link.rsplit(":", 1)
    said = False
    while True:
        try:
            s = socket.create_connection((h, int(p)))
            break
        except OSError:
            if not said:
                print(f"waiting for rytm-emu's Overbridge link on {a.link} (rytm-emu --ob-link) ...", flush=True)
                said = True
            time.sleep(1)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    print(f"overbridge link: connected to {a.link}")
    sizes = {b"N": 5, b"T": 4, b"P": 10}
    buf = b""
    try:
        while True:
            def need():
                if buf[0:1] == b"R":   # the Rytm's output (rytm-emu --usb-host); not used yet
                    return 5 if len(buf) < 5 else 5 + struct.unpack("<I", buf[1:5])[0] * 48
                return sizes.get(buf[0:1], 1)

            while not buf or len(buf) < need():
                d = s.recv(4096)
                if not d:
                    print("overbridge link closed")
                    return
                buf += d
            kind = buf[0:1]
            if kind == b"R":
                buf = buf[need():]
                continue
            if kind not in sizes:
                sys.exit(f"unexpected {buf[:5]!r}")
            msg, buf = buf[:sizes[kind]], buf[sizes[kind]:]
            if kind != b"N":
                continue          # the emulator's stand-in reports: unused here
            n = struct.unpack("<I", msg[1:5])[0]
            out = np.zeros((n, 12), dtype=np.int32)
            done = 0
            while done < n:
                k = min(1024, n - done)
                v = t.frames(k).astype(np.int32) << 16
                for i in range(8):
                    out[done:done + k, VOICE_CH[i]] = v[:, i]
                done += k
            s.sendall(b"A" + struct.pack("<I", n) + out.astype("<i4").tobytes())
    finally:
        inp.close()


if __name__ == "__main__":
    main()
