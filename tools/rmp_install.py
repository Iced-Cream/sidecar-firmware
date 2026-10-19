#!/usr/bin/env python3
"""Development installer for machine plugins over the voiceloop's serial link
(teensy/voiceloop, RYTM_PLUGINS 1). The bridge (sim/teensy_bridge.py) must not
be running: one program owns the port.

    rmp_install.py [--port PORT] install FILE.rmp       store it in the Teensy's flash
    rmp_install.py [--port PORT] assign SLOT ID VERSION  U<SLOT> (1..5) of the current
                                                         project plays ID (a.b.c)
    rmp_install.py [--port PORT] clear SLOT              U<SLOT> plays nothing
    rmp_install.py [--port PORT] info                    the device-info report ('D'), printed as JSON
    rmp_install.py [--port PORT] [--host] put FILE CARDPATH   a file onto the Teensy's card (/PRESETS/..., /MACHINES/...)
    rmp_install.py [--port PORT] [--host] remove ID   take a package out of the store
    rmp_install.py [--port PORT] [--host] export ID [DIR]   a package from the store to the card's
                                                         /MACHINES/<id>-<a.b.c>.rmp and to DIR (default .);
                                                         only with the transport stopped (the export holds the
                                                         Teensy's loop for the whole package)
    rmp_install.py [--port PORT] [--host] wipe --yes     format the store: EVERY package is lost (export them
                                                         first); the only way past a "foreign store layout" at boot

Frames (teensy/voiceloop/voiceloop.ino's header): 'D' (device info), 'I' <total u32 LE>, 'J' <n u16
LE> <n bytes> (n <= 256), 'K'; 'U' <slot> <len> <id> <a> <b> <c>; 'W' <mode> <len> <path> <n u16 LE>
<n bytes> (a file on the card, n <= 200); 'T' <0|1> (the transport stopped / running); 'Z' <len> <id> (remove a
package); 'E' <len> <id> (export a package: "export:" hex log lines, and a copy on the card); 'F' "WIPE" (format
the store). The Teensy
answers with log lines ('L' <len> <text>, as the bridge reads them); this tool
prints the "plugins:"/"plugin:" ones.

A Teensy in safe mode (after a watchdog reset) loads no plugins: assign then
prints "plugins: safe mode - not loaded" and exits 1.

Exit code 0 on success ("plugins: installed <id>"; for assign "plugins: U<n> <id>
ready"; for clear "plugins: U<n> cleared"), 1 on a failure line or a timeout.
Installing stalls the Teensy's flash (a 64 KB erase: ~0.25-0.28 s with interrupts off): not while it plays.
"""
import argparse
import glob
import re
import struct
import sys
import time

import serial

CHUNK = 256   # the voiceloop's input buffer holds 300 bytes


def safe_name(s):
    """A file-name part from what the Teensy reported (a version, an id): only [A-Za-z0-9._-], no leading dot."""
    s = re.sub(r"[^A-Za-z0-9._-]", "_", s)
    return "_" + s[1:] if s.startswith(".") else (s or "_")


def assign_frame(slot, mid, version):
    """The 'U' frame: U <slot> <len> <id> <a> <b> <c> (mid "" clears the slot)."""
    ver = [int(x) for x in version.split(".")] if mid else [0, 0, 0]
    if len(ver) != 3 or not all(0 <= x <= 255 for x in ver):
        sys.exit(f"bad version {version!r} (a.b.c, each 0..255)")
    b = mid.encode()
    return b"U" + bytes([slot, len(b)]) + b + bytes(ver)


class Link:
    """The voiceloop's serial framing: reports in, log lines / audio / SysEx out."""

    def __init__(self, port=None, quiet=False, host=False):
        port = port or next(iter(sorted(glob.glob("/dev/cu.usbmodem*"))), None)
        if not port:
            sys.exit("no Teensy serial port (/dev/cu.usbmodem*)")
        self.s = serial.Serial(port, 115200, timeout=0.2)
        self.logs = []          # every log line read, in order
        self.sysex = []         # every 'X' frame (SysEx for the Rytm), in order
        self.quiet = quiet
        self.host = host        # a USB-host build logs plain text until a framed command arrives
        if host:
            self._await_pong()

    PONG = b"L\x04pong"         # the framed answer to 'P' (plain text never holds the 0x04)

    def _await_pong(self, timeout=30.0):
        """USB-host build: send 'P' (once a second: a booting Teensy answers nothing) and skip every
        byte - plain-text log lines, or frames of a link framed earlier - up to and including the
        framed "pong". From then on the Teensy frames everything (until it reboots)."""
        t0 = time.time()
        seen = b""
        nxt = 0.0
        while time.time() - t0 < timeout:
            if time.time() >= nxt:
                self.s.write(b"P")
                nxt = time.time() + 1.0
            d = self.s.read(256)
            if not d:
                continue
            seen = (seen + d)[-(len(d) + len(self.PONG)):]
            i = seen.find(self.PONG)
            if i >= 0:
                # whatever followed the pong in this read is the start of framed output: keep it
                self._pending = seen[i + len(self.PONG):]
                return
        raise TimeoutError(f"no framed 'pong' from the Teensy in {timeout:.0f} s "
                           "(is it running the USB-host voiceloop, RYTM_USB_HOST=1?)")

    _pending = b""              # bytes read past the pong, consumed before the port's

    def _read(self, n):
        if self._pending:
            d, self._pending = self._pending[:n], self._pending[n:]
            return d
        return self.s.read(n)

    def _read_n(self, n, what):
        """Exactly n bytes, or TimeoutError (a frame's rest arrives within ~2 s)."""
        raw = b""
        t0 = time.time()
        while len(raw) < n:
            d = self._read(n - len(raw))
            if d:
                raw += d
            elif time.time() - t0 > 2:
                raise TimeoutError(f"short {what} frame ({len(raw)} of {n} bytes)")
        return raw

    def _read_frame(self):
        """One frame: ('L', text) / ('A', raw audio bytes) / ('X', bytes) / None on timeout."""
        k = self._read(1)
        if not k:
            return None
        if k == b"L":
            n = self._read_n(1, "log")[0]
            t = self._read_n(n, "log").decode(errors="replace")
            self.logs.append(t)
            if not self.quiet:
                print("teensy:", t, flush=True)
            return ("L", t)
        if k == b"X":
            n = self._read_n(1, "SysEx")[0]
            m = self._read_n(n, "SysEx")
            self.sysex.append(m)
            return ("X", m)
        if k == b"A":
            m = struct.unpack("<H", self._read_n(2, "audio"))[0]
            return ("A", self._read_n(m * 16, "audio"))
        return ("?", k)

    def sync(self, timeout=30.0, flush=True):
        """Until the Teensy answers a 32-frame request (it answers none while booting);
        flush: drop what was waiting unread first (log lines before it are kept otherwise)."""
        if flush:
            self.s.reset_input_buffer()
            self._pending = b""
        if self.host:           # (again: the Teensy may have rebooted to plain text since)
            self._await_pong(timeout)
        self.s.write(b"N" + struct.pack("<H", 32))
        t0 = time.time()
        while time.time() - t0 < timeout:
            try:
                f = self._read_frame()
            except TimeoutError:   # a stray byte read as a frame header: skip it, keep looking
                continue
            if f and f[0] == "A" and len(f[1]) == 32 * 16:
                return
        raise TimeoutError("the Teensy did not answer")

    def frames(self, n):
        """n frames x 8 voices as raw little-endian int16 bytes (log lines met on the way kept)."""
        self.s.write(b"N" + struct.pack("<H", n))
        t0 = time.time()
        while time.time() - t0 < 10:
            f = self._read_frame()
            if f and f[0] == "A":
                return f[1]
        raise TimeoutError("no audio")

    def report(self, sysex):
        """A report: the whole SysEx F0 .. F7."""
        self.s.write(b"S" + bytes([len(sysex)]) + bytes(sysex))

    def wait_log(self, ok, bad, timeout=5.0, since=None):
        """The first log line from index `since` on (default: new ones) matching ok
        (-> True) or bad (-> False); None on timeout."""
        t0 = time.time()
        start = len(self.logs) if since is None else since
        while time.time() - t0 < timeout:
            self._read_frame()
            for t in self.logs[start:]:
                if bad and re.search(bad, t):
                    return False
                if re.search(ok, t):
                    return True
            start = len(self.logs)
        return None

    def info(self, timeout=15.0):
        """The device-info report: send 'D', collect the "info: i/n chunk" log lines, return the JSON as a dict
        (None on a timeout or a broken document)."""
        import json
        self.s.write(b"D")
        chunks, total = {}, None
        t0 = time.time()
        while time.time() - t0 < timeout and (total is None or len(chunks) < total):
            self._read_frame()
            for t in self.logs:
                m = re.match(r"^info: (\d+)/(\d+) (.*)$", t)
                if m:
                    chunks[int(m.group(1))] = m.group(3)
                    total = int(m.group(2))
            self.logs = [t for t in self.logs if not t.startswith("info: ")]
        if total is None or len(chunks) < total:
            return None
        try:
            return json.loads("".join(chunks[i] for i in range(1, total + 1)))
        except ValueError:
            return None

    # the development frames
    def install(self, data):
        self.s.write(b"I" + struct.pack("<I", len(data)))
        for i in range(0, len(data), CHUNK):
            c = data[i:i + CHUNK]
            self.s.write(b"J" + struct.pack("<H", len(c)) + c)
        self.s.write(b"K")
        return self.wait_log(r"^plugins: installed ", r"^plugins: install failed", 10)

    def assign(self, slot, mid, version):
        self.s.write(assign_frame(slot, mid, version))
        fail = r"^plugins: (assign U\d refused|cannot |no card|safe mode)"
        if not mid:
            return self.wait_log(rf"^plugins: U{slot} cleared", fail, 5)
        return self.wait_log(rf"^plugins: U{slot} {re.escape(mid)} ready",
                             rf"{fail}|^plugins: U{slot} {re.escape(mid)} (state|init|declares|needs)", 10)

    def put(self, path, data):
        """A file on the Teensy's card (/PRESETS/... or /MACHINES/...), 200 bytes a 'W' frame: create, then
        append. True when the Teensy logged the last write."""
        p = path.encode()
        mark = len(self.logs)
        for i in range(0, max(len(data), 1), 200):
            c = data[i:i + 200]
            self.s.write(b"W" + bytes([0 if i == 0 else 1, len(p)]) + p + struct.pack("<H", len(c)) + c)
            ok = self.wait_log(rf"^card: wrote {re.escape(path)} ", rf"^card: cannot write {re.escape(path)}", 5, since=mark)
            if ok is not True:
                return ok
            mark = len(self.logs)
        return True

    def transport(self, running):
        """'T': the Rytm's transport as the bridge relays it (MIDI Start/Continue 1, Stop 0)."""
        self.s.write(b"T" + bytes([1 if running else 0]))
        return self.wait_log(rf"^transport {'running' if running else 'stopped'}$", None, 3)

    def remove(self, mid):
        """'Z': remove a package from the store (True), False when it was not there or the id is refused
        (empty or over 31 bytes: the Teensy answers "cannot remove a <n>-byte id")."""
        b = mid.encode()[:255]
        self.s.write(b"Z" + bytes([len(b)]) + b)
        return self.wait_log(rf"^plugins: removed {re.escape(mid)}$",
                             rf"^plugins: cannot remove ({re.escape(mid)}|a {len(b)}-byte id)$", 5)

    def export(self, mid, timeout=30.0):
        """'E': a package's bytes from the store ("export: <id> <offset> <hex>" lines, then "export: <id> done
        <size> <crc32> <a.b.c> card <ok|failed|none|skipped|kept>"; none: no card; kept: an earlier
        export's .tmp is on the card, left alone), checked against the size and CRC-32 the Teensy reports.
        (data, version, card) on success, None on a timeout, False when the Teensy refused it."""
        import zlib
        b = mid.encode()[:255]
        mark = len(self.logs)
        self.s.write(b"E" + bytes([len(b)]) + b)
        quiet, self.quiet = self.quiet, True     # (not the hex lines on the terminal)
        try:
            ok = self.wait_log(rf"^export: {re.escape(mid)} done ", rf"^export: cannot export", timeout, since=mark)
        finally:
            self.quiet = quiet
        if not ok:
            return False if any(t.startswith("export: cannot export") for t in self.logs[mark:]) else None
        parts, done = {}, None
        for t in self.logs[mark:]:
            m = re.match(rf"^export: {re.escape(mid)} (\d+) ([0-9a-f]*)$", t)
            if m:
                parts[int(m.group(1))] = bytes.fromhex(m.group(2))
            m = re.match(rf"^export: {re.escape(mid)} done (\d+) ([0-9a-f]{{8}}) (\S+) card (\w+)$", t)
            if m:
                done = m
        data = b"".join(parts[k] for k in sorted(parts))
        if not done or len(data) != int(done.group(1)) or zlib.crc32(data) != int(done.group(2), 16) or \
                sorted(parts) != list(range(0, len(data), 64)):
            return False
        return data, done.group(3), done.group(4)

    def wipe(self):
        """'F' "WIPE": format the store (every package lost). True when the Teensy logged it wiped."""
        self.s.write(b"F" + b"WIPE")
        return self.wait_log(r"^plugins: store wiped", r"^plugins: (cannot wipe|store wipe FAILED)", 20)

    def remove_file(self, path):
        """Delete a file, or an empty folder, on the card (/PRESETS/... or /MACHINES/... only). False when it
        was not there."""
        p = path.encode()
        self.s.write(b"W" + bytes([2, len(p)]) + p + struct.pack("<H", 0))
        return self.wait_log(rf"^card: removed {re.escape(path)}$",
                             rf"^card: (removed {re.escape(path)} \(it was not there\)|cannot write {re.escape(path)})", 5)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--host", action="store_true", help="the Teensy runs the USB-host build")
    ap.add_argument("cmd", choices=("install", "assign", "clear", "info", "put", "remove", "export", "wipe"))
    ap.add_argument("args", nargs="*")
    ap.add_argument("--yes", action="store_true", help="wipe: yes, lose every package in the store")
    a = ap.parse_args()
    if a.cmd == "wipe" and not a.yes:
        ap.error("wipe formats the store and loses EVERY package (export them first): add --yes")
    link = Link(a.port, host=a.host)
    link.sync()
    if a.cmd == "install" and len(a.args) == 1:
        ok = link.install(open(a.args[0], "rb").read())
    elif a.cmd == "assign" and len(a.args) == 3:
        ok = link.assign(int(a.args[0]), a.args[1], a.args[2])
    elif a.cmd == "info" and not a.args:
        import json
        rep = link.info()
        if rep is None:
            print("rmp_install: no (complete) info report (timeout)", file=sys.stderr)
            sys.exit(1)
        print(json.dumps(rep, indent=2))
        sys.exit(0)
    elif a.cmd == "clear" and len(a.args) == 1:
        ok = link.assign(int(a.args[0]), "", "0.0.0")
    elif a.cmd == "put" and len(a.args) == 2:
        ok = link.put(a.args[1], open(a.args[0], "rb").read())
    elif a.cmd == "remove" and len(a.args) == 1:
        ok = link.remove(a.args[0])
        if ok is False:
            print(f"rmp_install: cannot remove {a.args[0]!r} (not installed, or not an id of 1..31 bytes)", file=sys.stderr)
    elif a.cmd == "export" and len(a.args) in (1, 2):
        import os
        got = link.export(a.args[0])
        ok = bool(got)
        if got:
            data, ver, card = got
            out = os.path.join(a.args[1] if len(a.args) == 2 else ".", f"{safe_name(a.args[0])}-{safe_name(ver)}.rmp")
            with open(out, "wb") as f:
                f.write(data)
            print(f"rmp_install: {a.args[0]} {ver}, {len(data)} B -> {out}; card /MACHINES copy: {card}")
            ok = card in ("ok", "none")   # no card in the Teensy: only the local copy was asked for
        elif got is False:
            print(f"rmp_install: cannot export {a.args[0]!r} (not installed, or the transfer did not check)", file=sys.stderr)
    elif a.cmd == "wipe" and not a.args:
        ok = link.wipe()
    else:
        ap.error("wrong arguments")
    if ok is None:
        print("rmp_install: no answer (timeout)", file=sys.stderr)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
