#!/usr/bin/env python3
"""End-to-end check of machine plugins on the real Teensy, without the emulator:
speaks the bridge's protocol (sim/teensy_bridge.py's framing, sim/protocol.py's
reports) straight to teensy/voiceloop.

    rmp_e2e.py [--port PORT] [--host]     (--host: the USB-host build, RYTM_USB_HOST=1)

Expects rytm.sine_test, rytm.fault_test and rytm.state_test installed (tools/rmp_install.py
install ...) and plugins/v2_test built (make -C plugins/v2_test: steps 7..13 install it); assigns U1 =
rytm.sine_test 1.0.0 and U2 = rytm.fault_test 1.0.0 in the current project itself. 153 checks, ALL PASS on
both builds (2026-10-11). Steps:
  1. voice 0 on U1 (voice flag 0x50), a trig at note 69: ~440 Hz
  2. voice 1 on U2 (0x58), fault_test mode 1 (null write): "U2 ... CRASHED -
     muted", the Teensy keeps answering, voice 1 silent, voice 0 still 440 Hz
  3. U2 assigned afresh, mode 2 (endless loop): "U2 ... timed out - muted" (or
     over budget), still answering, voice 0 still 440 Hz
  4. U2 afresh, mode 3 (a long render): "over budget - muted" after three
     renders past twice the declared CPU (or timed out, if a tick lands in one)
  5. the device-info report ('D')
  6. a 20000-byte state (rytm.state_test on U3): refused without PSRAM, 8 voices with it
  7. rytm.v2_test installed and assigned to U4: 1A (all parts) from its manifest, 'unchanged' for the current generation, P6's enum names (1B), a 1D after the assign, U2 (muted) says MUTED
  8. presets: three files on the card (/PRESETS/rytm.v2_test/, rmp_install.py put), listed as 1B 0x20, PRE loads
     with 1C write-back, a p-locked native preset played without, held trigs (a list being read, a save and a job
     queued), card-error back-offs; the files, their folder and U4 removed after
  9. a pool knob: a sample from the card added to project E2ETEST's pool (0F; the Teensy pushes the pool table,
     14, itself), played by U4 in POOL mode through the head and the ring (no PSRAM); ASSIGN (12) of the same
     file to a second slot pushes 14 again and keeps the voice's reader, ASSIGN to its own slot prepares it
     afresh; the second slot plays; the voice back to DSP SAMPLE (v2_test asks pool_info in voice_leave) plays
     as a fresh voice; back to the user's project (14 pushed for it). E2ETEST's files stay on the card
     (/.rytm/pools/E2ETEST.txt, /.rytm/slots/E2ETEST.txt: 'W' writes only under /PRESETS/ and /MACHINES/;
     each run reuses them)
  10. the slot menu's side: the installed list (1B 0x21), 1E assign U5 (with a preset folder: the READY answer's U5
     generation is U5's next 1A's, no push after it) and clear it (1D answers), a stale list refused;
     a project change (to E2ETEST, three slots to load) and a 1E straight after it answered within 2 s
  11. install from the card: a package only on the card's /MACHINES is MISSING with the 'on card' flag, STOP TO INSTALL
     while the transport runs ('T'), a card file whose header has another version BAD, installed and READY when
     stopped; two packages named V2 TEST listed as V2 TEST and V2 TEST~2 (each slot's 1A says its own); a 60 KB
     package's copy paused by a Play ('T' 1) midway: LOADING within 2 s, a 1E on the slot meanwhile BAD at once,
     finished after the Stop; a project change during a copy READY; a 10 to a project whose slot is MISSING
     and a 1E install in the same write installs that project's package. Every 1E answered within 2 s (the
     Rytm's wait).
  12. the sound tag (spec 2.6): U1's 1A carries rytm.sine_test's fingerprint (an EMPTY slot's 0); every report is in
     the 43 B (0x04) / 41 B (0x03) form; a matching tag and an untagged one play; a mismatched one is silent, its 0x03
     dropped, logged once; a mismatch releases a held note (a matching 0x03 and a note-off after it bring nothing
     back) and the next matching trig plays afresh; a slot reload and a project change clear the bits; trigs with the
     right tag while U1 is LOADING mismatch nothing; a built-in machine (PLAITS) with a nonzero tag plays its params
     as sent; a mismatched voice whose track moves to another U slot loses the bit (its mismatch there is logged
     afresh); two tracks sharing a voice, one with a foreign tag: each foreign trig after a matching one silences
     the voice (logged each time), each matching one plays.
  13. a Rytm that starts while the Teensy runs (no 1D seen): its 10 <project> gets a 1D (the installed list's
     generation) and a 1E with it works; 10s every 100 ms bring a 1D at most once a second; a 19 gets a 1D when
     none went out for 3 s, and none within 3 s of one (a push with new generations aside); a 19 what 3 (the slot
     menu's installed list) gets one once the last is 1 s old.
Exit 0 when every check passes.
"""
import argparse
import json
import os
import re
import struct
import sys
import tempfile
import time
import zlib

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import protocol as P                    # noqa: E402
from protocol import HDR, TRIG_PARAMS   # noqa: E402
from rmp_install import Link, assign_frame   # noqa: E402
import rmp_pack  # noqa: E402

API, FORMAT = 2, 3   # the Teensy's plugin API and newest package format
U1, U2 = 0x50, 0x58
RATE = 48000
V2_RMP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "plugins", "v2_test", "v2_test.rmp")
V2_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "plugins", "v2_test")
V2_ELF, V2_MAN = os.path.join(V2_DIR, "v2_test.elf"), os.path.join(V2_DIR, "manifest.json")
DECODE = {P.META: P.decode_meta, P.LIST: P.decode_list, P.KNOBS: P.decode_knobs, P.SLOTS: P.decode_slots}


def trig(voice_byte, note, vel, p, tag=0):
    """0x04 TRIG+PARAMS with P1..P16 (integer values, fraction 0) and the sound tag (spec 2.6; 0 untagged): the
    43-byte form the Rytm (rytm-mods 0021) sends."""
    ps = (list(p) + [0] * 16)[:16]
    return P.trig_params(voice_byte, note, vel, [(v & 0x7F) << 7 for v in ps], tag)


def render(link, n):
    """n frames, 8 voices, as an (n, 8) int16 array."""
    out = []
    while n > 0:
        k = min(1024, n)
        out.append(np.frombuffer(link.frames(k), "<i2").reshape(k, 8))
        n -= k
    return np.concatenate(out)


def pitch(x):
    """Hz from rising zero crossings (linear interpolation)."""
    x = x.astype(np.float64)
    i = np.where((x[:-1] < 0) & (x[1:] >= 0))[0]
    if len(i) < 3:
        return 0.0
    t = i + (-x[i]) / (x[i + 1] - x[i])
    return RATE * (len(t) - 1) / (t[-1] - t[0])


fails = []


def check(what, ok, detail=""):
    print(f"{'PASS' if ok else 'FAIL'}: {what}{(' - ' + detail) if detail else ''}", flush=True)
    if not ok:
        fails.append(what)


def sine_ok(link, tag):
    link.report(trig(U1 | 0, 69, 100, [64, 127, 127]))   # TUNE 0, LEVEL max, DECAY ~4 s
    a = render(link, 4800)
    f, peak = pitch(a[480:, 0]), int(np.abs(a[:, 0]).max())
    check(f"{tag}: voice 0 (U1 sine_test) at ~440 Hz", abs(f - 440) < 3 and peak > 3000, f"{f:.1f} Hz, peak {peak}")
    return a


def collect(link, cmd, n_min=1, timeout=3.0, since=0, pred=None):
    """The decoded messages `cmd` the Teensy sent for the Rytm ('X' frames from link.sysex[since:]),
    once n_min of them (passing pred) arrived, or what there is after timeout seconds."""
    t0 = time.time()
    while True:
        got = []
        for m in link.sysex[since:]:
            c = P.parse(m)
            if c and c[0] == cmd:
                d = DECODE[cmd](c[1])
                if d is not None and (pred is None or pred(d)):
                    got.append(d)
        if len(got) >= n_min or time.time() - t0 > timeout:
            return got
        link._read_frame()


from protocol import PARAMS   # noqa: E402
U4 = 0x68


def params(voice_byte, p, tag=0):
    """0x03 PARAMS with P1..P16 (integer values, fraction 0) and the sound tag: the 41-byte form."""
    ps = (list(p) + [0] * 16)[:16]
    return P.params_report(voice_byte, [(v & 0x7F) << 7 for v in ps], tag)


def pump(link, secs):
    """Let the Teensy run its loop passes (and read what it sends) for secs seconds."""
    t0 = time.time()
    while time.time() - t0 < secs:
        link._read_frame()


def meta_head(link, slot, pred=lambda h: True, timeout=5.0):
    """1A part 0 of a slot, asked for with generation 0 every 0.5 s until it passes pred (None after timeout)."""
    t0 = time.time()
    while time.time() - t0 < timeout:
        mark = len(link.sysex)
        link.report(P.ask(slot, 0, P.ASK_META))
        got = collect(link, P.META, since=mark, timeout=0.5, pred=lambda d: d["slot"] == slot and d["part"] == 0)
        if got and pred(got[-1]):
            return got[-1]
    return None


def u4_gen(link):
    """U4's current generation (1A part 0 for a 19 at generation 0), None without an answer."""
    mark = len(link.sysex)
    link.report(P.ask(4, 0, P.ASK_META))
    h = collect(link, P.META, since=mark, pred=lambda d: d["slot"] == 4 and d["part"] == 0)
    return (h[0]["gen"], h[0]["state"]) if h else (None, None)


def step7_metadata(link):
    """1A from the manifest, generations, 'unchanged', enum names, 1D (U4 = rytm.v2_test)."""
    with open(V2_RMP, "rb") as f:
        check("7: rytm.v2_test installed", link.install(f.read()) is True)
    g0, _ = u4_gen(link)
    mark = len(link.sysex)
    check("7: U4 = rytm.v2_test ready", link.assign(4, "rytm.v2_test", "1.0.0") is True)
    slots = collect(link, P.SLOTS, since=mark, pred=lambda d: d["gens"][3] != g0)
    check("7: a 1D after the assign, U4's generation moved on", g0 is not None and bool(slots),
          f"before {g0}, 1D {slots[-1] if slots else None}")
    mark = len(link.sysex)
    link.report(P.ask(4, 0, P.ASK_META))
    parts = collect(link, P.META, n_min=4, since=mark, pred=lambda d: d["slot"] == 4)
    head = next((d for d in parts if d["part"] == 0), {})
    knobs = [k for d in parts if d["part"] not in (0, P.META_UNCHANGED) for k in d.get("knobs", [])]
    check("7: 1A part 0: READY, V2 TEST, page 2 params, 10 knobs in 4 parts",
          head.get("state") == P.ST_READY and head.get("name") == "V2 TEST" and head.get("short") == "V2T" and
          head.get("p2_title") == "V2 PAGE 2" and bool(head.get("flags", 0) & P.FLAG_PARAMS) and
          head.get("nknobs") == 10 and head.get("nparts") == 4, str(head))
    lev = next((k for k in knobs if k["p"] == 2), {})
    spr = next((k for k in knobs if k["p"] == 10), {})
    check("7: knobs: LEV a fader of role preset, default 100; SPR 10..20; P1 preset, P5 pool, P9 note",
          lev.get("look") == "fader" and lev.get("role") == "preset" and lev.get("default") == 100 and
          (spr.get("min"), spr.get("max"), spr.get("default")) == (10, 20, 15) and
          [k["kind"] for k in knobs if k["p"] in (1, 5, 9)] == ["preset", "pool", "note"], f"{len(knobs)} knobs")
    gen = head.get("gen", 0)
    check("7: 1A's generation is the 1D's", bool(slots) and gen == slots[-1]["gens"][3], f"1A {gen}")
    mark = len(link.sysex)
    link.report(P.ask(4, gen, P.ASK_META))
    un = collect(link, P.META, since=mark, pred=lambda d: d["slot"] == 4)
    check("7: the same generation: 'unchanged'", bool(un) and un[0].get("unchanged") is True, str(un[:1]))
    mark = len(link.sysex)
    link.report(P.ask(4, gen, P.ASK_ENUM, 5))   # P6 MODE
    ls = collect(link, P.LIST, since=mark, pred=lambda d: d["slot"] == 4 and d["list"] == 5)
    check("7: P6's enum names: SINE POOL", bool(ls) and ls[0]["total"] == 2 and
          [n for _, n in ls[0]["entries"]] == ["SINE", "POOL"], str(ls[:1]))
    mark = len(link.sysex)
    link.report(P.ask(2, 0, P.ASK_META))         # U2: fault_test, muted in step 4
    m2 = collect(link, P.META, since=mark, pred=lambda d: d["slot"] == 2 and d["part"] == 0)
    check("7: a muted slot says MUTED (the Rytm shows !FAULT)", bool(m2) and m2[0]["state"] == P.ST_MUTED and
          m2[0]["name"] == "FAULT", str(m2[:1]))
    # voice_enter / voice_leave (API 2): a voice moving U4 -> U1 -> U4 -> another machine -> U4 and U4
    # reassigned while it plays. v2_test counts the calls but does not log them, so this checks that they
    # run without a fault and the voice plays U4 again each time (not the counts).
    mark = len(link.logs)
    peaks = []
    for vb in (0x68, 0x50, 0x68, 0x00, 0x68):
        link.report(trig(vb | 3, 69, 100, [0, 127, 64, 64, 0, 0, 0, 127, 60, 15]))
        peaks.append(int(np.abs(render(link, 2400)[480:, 3]).max()))
    link.assign(4, "rytm.v2_test", "1.0.0")
    link.report(trig(0x68 | 3, 69, 100, [0, 127, 64, 64, 0, 0, 0, 127, 60, 15]))
    peaks.append(int(np.abs(render(link, 2400)[480:, 3]).max()))
    link.sync(5, flush=False)
    bad = [t for t in link.logs[mark:] if t.startswith("plugins: U4") and ("muted" in t or "CRASHED" in t)]
    check("7: voice 3 enters and leaves U4 (voice_enter/voice_leave), plays it each time, no fault",
          not bad and all(peaks[i] > 3000 for i in (0, 2, 4, 5)), f"peaks {peaks} {bad[:1]}")
    # U4 back to empty: a 1D with its generation moved on, 1A says EMPTY
    g1, _ = u4_gen(link)
    mark = len(link.sysex)
    check("7: U4 cleared", link.assign(4, "", "0.0.0") is True)
    slots = collect(link, P.SLOTS, since=mark, pred=lambda d: d["gens"][3] != g1)
    g2, st2 = u4_gen(link)
    check("7: after the clear a 1D moves U4's generation on, 1A says EMPTY", bool(slots) and g2 != g1 and
          st2 == P.ST_EMPTY, f"{g1} -> {g2}, state {st2}")
    link.sync(5, flush=False)
    sine_ok(link, "7")


PRESETS = {"Bright.txt": b"P2 120\nP3 30\n", "Dark.txt": b"P2 40\nP3 5\n", "Wide.v2p": b"V2P 90 60 200"}
PRE_DIR = "/PRESETS/rytm.v2_test"


EXTRA = ["Long.txt", "Zed.txt", "Aaa.txt", "Mid.txt", "Case.txt"] + [f"N{i:02d}.txt" for i in range(18)]
on_card = set()   # EXTRA files put (removed by step 8's cleanup)


def wframe(mode, path, data=b""):
    """A 'W' frame (tools/rmp_install.py put/remove_file), for writing it together with a report."""
    p = path.encode()
    return b"W" + bytes([mode, len(p)]) + p + struct.pack("<H", len(data)) + data


def sframe(sysex):
    """An 'S' frame (Link.report), for writing it together with another frame."""
    return b"S" + bytes([len(sysex)]) + bytes(sysex)


def put_extra(link, name, data):
    on_card.add(name)
    return link.put(f"{PRE_DIR}/{name}", data)


def remove_extra(link, name):
    on_card.discard(name)
    return link.remove_file(f"{PRE_DIR}/{name}")


def logs_since(link, mark, pat):
    return [t for t in link.logs[mark:] if re.search(pat, t)]


def preset_names(link, gen, page, n_min=1):
    mark = len(link.sysex)
    link.report(P.ask(4, gen, P.ASK_PRESETS, page))
    return collect(link, P.LIST, n_min=n_min, since=mark, pred=lambda d: d["slot"] == 4 and d["list"] == P.LIST_PRESETS)


def note_bounded(line):
    """A held trig's note that played at its bound (kPlugNoteHoldUs, 2 ms) before its preset load."""
    m = re.search(r"held, before its preset load: (\d+) us", line)
    return bool(m) and 2000 <= int(m.group(1)) < 8000


def step8_more(link):
    """The review's cases: a PRE turn then a trig in one burst, a held note, a cut file, a list change in the
    generation, a 'W' while a load waits, paging, a card error's back-off."""
    # a PRE turn and a trig together (before the next loop pass): the write-back is not lost; the note waits for it
    mark, lmark = len(link.sysex), len(link.logs)
    link.s.write(sframe(params(U4 | 2, [0, 40, 5, 64, 0, 0, 0, 127])) +
                 sframe(trig(U4 | 2, 69, 100, [0, 40, 5, 64, 0, 0, 0, 127])))
    kn = collect(link, P.KNOBS, since=mark, timeout=1.0, n_min=2, pred=lambda d: d["voice"] == 2)
    pump(link, 0.2)
    lines = logs_since(link, lmark, r"^(preset voice 2|trig voice 2)")
    held = [int(m.group(1)) for t in lines for m in [re.search(r"after its preset load: (\d+) us", t)] if m]
    check("8: PRE 0 and a trig at once: 1C Bright still sent, the note after the load (< 2 ms)",
          [k["knobs"] for k in kn] == [[(1, 120), (2, 30)]] and len(lines) == 2 and lines[0].startswith("preset voice 2 U4 0 Bright (written back")
          and len(held) == 1 and held[0] < 2000, f"{[k['knobs'] for k in kn]} {lines}")
    mark, lmark = len(link.sysex), len(link.logs)
    link.s.write(sframe(params(U4 | 2, [1, 120, 30, 64, 0, 0, 0, 127])) +
                 sframe(trig(U4 | 2, 69, 100, [2, 120, 30, 64, 0, 0, 0, 127])))
    kn = collect(link, P.KNOBS, since=mark, timeout=1.0, n_min=2, pred=lambda d: d["voice"] == 2)
    pump(link, 0.2)
    lines = logs_since(link, lmark, r"^(preset voice 2|trig voice 2)")
    check("8: PRE 1 and a trig p-locked to 2 at once: 1C Dark, then Wide played, then the note",
          [k["knobs"] for k in kn] == [[(1, 40), (2, 5)]] and len(lines) == 3 and "1 Dark (written back" in lines[0] and
          "2 Wide (played" in lines[1] and "after its preset load" in lines[2], f"{[k['knobs'] for k in kn]} {lines}")
    # a PRE turn, a trig on the same PRE (it takes the pending write-back over) and a 'W' to the folder, in one
    # write: the list change drops the load, the sound's report is decided again (a resync, on its own values,
    # not the trig's p-locked LEV) - one 1C Bright, then the note; the file's write and the scan can take longer
    # than the note's hold: then the note plays at its bound (>= 2 ms), before the load. The trig's LEV 60 differs
    # from the sound's 40: a p-lock, kept over Bright's written-back LEV 120 (its hold) - the voice plays at 60.
    # Decided on the trig's values instead, LEV 60 would count as the sound's own and the hold would play 120.
    on_card.add("Zed.txt")
    mark, lmark = len(link.sysex), len(link.logs)
    link.s.write(sframe(params(U4 | 2, [0, 40, 5, 64, 0, 0, 0, 127])) +
                 sframe(trig(U4 | 2, 69, 100, [0, 60, 5, 64, 0, 0, 0, 127])) +
                 wframe(0, f"{PRE_DIR}/Zed.txt", b"P2 1\n"))   # Bright Dark Wide Zed: entry 0 still Bright
    link.wait_log(r"^trig voice 2 ", None, 3, since=lmark)
    kn = collect(link, P.KNOBS, since=mark, timeout=1.0, n_min=2, pred=lambda d: d["voice"] == 2)
    lines = logs_since(link, lmark, r"^(presets U4 |preset voice 2|trig voice 2)")
    pre = [i for i, t in enumerate(lines) if t.startswith("preset voice 2 U4 0 Bright (written back")]
    note = [i for i, t in enumerate(lines) if t.startswith("trig voice 2 ")]
    check("8: PRE 0, a trig on PRE 0 and a 'W' at once: the list read again, one 1C Bright, then the note (or at its bound)",
          [k["knobs"] for k in kn] == [[(1, 120), (2, 30)]] and "presets U4 rytm.v2_test: 4" in lines and
          len(pre) == 1 and len(note) == 1 and (pre[0] < note[0] or note_bounded(lines[note[0]])),
          f"{[k['knobs'] for k in kn]} {lines}")
    a = render(link, 4800)
    peak = int(np.abs(a[:, 2]).max())
    # measured: 8529 at LEV 60; 17058 with the takeover's report decided again (LEV 120)
    check("8: ... the trig's p-locked LEV 60 plays (not the held LEV 120)", 4000 < peak < 12800, f"peak {peak}")
    check("8: Zed.txt removed (resync case)", remove_extra(link, "Zed.txt") is True)
    pump(link, 0.2)
    # a trig p-locked to Wide, then a non-trig PRE turn to Dark before the load, in one write: the p-lock survives
    # the write-back - 1C Dark, Wide played, then the note (or at its bound: a slow first card read after a write)
    mark, lmark = len(link.sysex), len(link.logs)
    link.s.write(sframe(trig(U4 | 2, 69, 100, [2, 120, 30, 64, 0, 0, 0, 127])) +
                 sframe(params(U4 | 2, [1, 120, 30, 64, 0, 0, 0, 127])))
    kn = collect(link, P.KNOBS, since=mark, timeout=1.0, n_min=2, pred=lambda d: d["voice"] == 2)
    pump(link, 0.2)
    lines = logs_since(link, lmark, r"^(preset voice 2|trig voice 2)")
    loads = [t for t in lines if t.startswith("preset voice 2")]
    note = [t for t in lines if t.startswith("trig voice 2 ")]
    check("8: a trig p-locked to Wide, then PRE 1 before the load: 1C Dark, Wide played, then the note (or at its bound)",
          [k["knobs"] for k in kn] == [[(1, 40), (2, 5)]] and len(loads) == 2 and "1 Dark (written back" in loads[0] and
          "2 Wide (played" in loads[1] and len(note) == 1 and
          ("after its preset load" in note[0] and lines[-1] == note[0] or note_bounded(note[0])),
          f"{[k['knobs'] for k in kn]} {lines}")
    # a 'W' naming the folder in another letter case (FAT/exFAT: the same folder) rescans the slot's list
    up = "/PRESETS/" + PRE_DIR.split("/")[-1].upper()
    on_card.add("Case.txt")
    lmark = len(link.logs)
    put = link.put(f"{up}/Case.txt", b"P2 3\n")
    ok = link.wait_log(r"^presets U4 rytm\.v2_test: 4$", None, 3, since=lmark)
    check(f"8: a 'W' to {up}/Case.txt (other case): the list read again (4 presets)", put is True and ok is True)
    lmark = len(link.logs)
    on_card.discard("Case.txt")
    gone = link.remove_file(f"{up}/Case.txt")
    ok = link.wait_log(r"^presets U4 rytm\.v2_test: 3$", None, 3, since=lmark)
    check("8: ... removed the same way: read again (3 presets)", gone is True and ok is True)
    pump(link, 0.2)
    # a trig before the slot's list is read (a 'W' to its folder just before it): the voice waits for the scan
    # (as it does for its description after an assign) - the note held, then the scan, the preset, the note
    on_card.add("Mid.txt")
    lmark = len(link.logs)
    link.s.write(wframe(0, f"{PRE_DIR}/Mid.txt", b"P2 10\n") +
                 sframe(trig(U4 | 2, 69, 100, [2, 40, 5, 64, 0, 0, 0, 127])))   # Bright Dark Mid Wide: PRE 2 = Mid
    link.wait_log(r"^trig voice 2 ", None, 2, since=lmark)
    pump(link, 0.2)
    lines = logs_since(link, lmark, r"^(presets U4 |preset voice 2|trig voice 2)")
    held = [int(m.group(1)) for t in lines for m in [re.search(r"after its preset load: (\d+) us", t)] if m]
    check("8: a trig before the list is read: held, the scan, Mid played, then the note (< 2 ms)",
          len(lines) == 3 and lines[0] == "presets U4 rytm.v2_test: 4" and lines[1].startswith("preset voice 2 U4 2 Mid (played")
          and len(held) == 1 and held[0] < 2000 and lines[2].startswith("trig voice 2 "), str(lines))
    check("8: Mid.txt removed", remove_extra(link, "Mid.txt") is True)
    pump(link, 0.2)
    # a trig while a slot save and a package job are queued (U5 assigned in the same write): the held trig's
    # load runs ahead of them - its note after one pass (the load; < 6 ms), before the save applies
    lmark = len(link.logs)
    link.s.write(assign_frame(5, "rytm.sine_test", "1.0.0") + sframe(trig(U4 | 2, 69, 100, [0, 40, 5, 64, 0, 0, 0, 127])))
    ready = link.wait_log(r"^plugins: U5 rytm\.sine_test ready", r"^plugins: (assign U5 refused|cannot )", 10, since=lmark)
    pump(link, 0.2)
    lines = logs_since(link, lmark, r"^(preset voice 2|trig voice 2|plugins: U5)")
    held = [int(m.group(1)) for t in lines for m in [re.search(r"after its preset load: (\d+) us", t)] if m]
    check("8: a trig with a save and a job queued: Bright played, then the note (one pass, < 6 ms), before the save applies",
          ready is True and len(lines) >= 3 and lines[0].startswith("preset voice 2 U4 0 Bright (played") and
          len(held) == 1 and held[0] < 6000 and lines[1].startswith("trig voice 2 ") and lines[2].startswith("plugins: U5 = "),
          str(lines))
    # a trig whose list waits behind a slot save (U5 cleared, a 'W' to U4's folder, the trig, in one write): the
    # note plays by ~2 ms (one pass more at most) before its preset; the scan and the load follow the save
    on_card.add("Mid.txt")
    lmark = len(link.logs)
    link.s.write(assign_frame(5, "", "0.0.0") + wframe(0, f"{PRE_DIR}/Mid.txt", b"P2 10\n") +
                 sframe(trig(U4 | 2, 69, 100, [2, 40, 5, 64, 0, 0, 0, 127])))
    link.wait_log(r"^preset voice 2 ", None, 3, since=lmark)
    pump(link, 0.2)
    lines = logs_since(link, lmark, r"^(presets U4 |preset voice 2|trig voice 2|plugins: U5)")
    held = [int(m.group(1)) for t in lines for m in [re.search(r"held, before its preset load: (\d+) us", t)] if m]
    check("8: a trig while a save runs: the note held 2..8 ms, before the save applies; then the scan, Mid played",
          len(lines) >= 4 and len(held) == 1 and 2000 <= held[0] < 8000 and lines[0].startswith("trig voice 2 ") and
          lines[1] == "plugins: U5 cleared (/.rytm/slots/PRESETS.txt)" and "presets U4 rytm.v2_test: 4" in lines and
          lines[-1].startswith("preset voice 2 U4 2 Mid (played"), str(lines))
    check("8: Mid.txt removed again", remove_extra(link, "Mid.txt") is True)
    pump(link, 0.2)
    # a generic file over 4 KB: read to 4096 B, its cut last line dropped ("P2 9" of "P2 99"), the rest ignored
    body = b"P2 50\n"
    while len(body) + 60 <= 4092:
        body += b"#" * 59 + b"\n"
    body += b"#" * (4092 - len(body) - 1) + b"\n" + b"P2 99\nP3 77\n"
    body += b"#" * (5000 - len(body))
    assert body[4092:4096] == b"P2 9"
    check("8: Long.txt (5000 B) on the card", put_extra(link, "Long.txt", body) is True)
    mark = len(link.sysex)
    link.report(params(U4 | 2, [2, 40, 5, 64, 0, 0, 0, 127]))   # Bright Dark Long Wide: PRE 2
    kn = collect(link, P.KNOBS, since=mark, timeout=2.0, pred=lambda d: d["voice"] == 2)
    check("8: a 5000-byte generic preset: cut at 4096 B, its cut line dropped: 1C LEV 50 only",
          bool(kn) and kn[0]["knobs"] == [(1, 50)], str(kn[:1]))
    check("8: Long.txt removed", remove_extra(link, "Long.txt") is True)
    # a new preset moves the slot's generation (1D) and 1B lists it
    head = meta_head(link, 4)
    g0 = head["gen"] if head else None
    mark = len(link.sysex)
    check("8: Zed.txt on the card", put_extra(link, "Zed.txt", b"P2 1\n") is True)
    sl = collect(link, P.SLOTS, since=mark, pred=lambda d: d["gens"][3] != g0)
    g1 = sl[-1]["gens"][3] if sl else 0
    ls = preset_names(link, g1, 0)
    check("8: a new preset file: a 1D moves U4's generation, 1B lists it",
          g0 is not None and bool(sl) and bool(ls) and ls[0]["gen"] == g1 and [n for _, n in ls[0]["entries"]] == ["Bright", "Dark", "Wide", "Zed"],
          f"{g0} -> {g1}, {ls[:1]}")
    check("8: Zed.txt removed", remove_extra(link, "Zed.txt") is True)
    pump(link, 0.2)
    # a 'W' between a report's decision and its load: decided again on the new list (entry 1 is Bright now)
    on_card.add("Aaa.txt")
    mark, lmark = len(link.sysex), len(link.logs)
    link.s.write(sframe(params(U4 | 2, [1, 64, 64, 64, 0, 0, 0, 127])) +
                 wframe(0, f"{PRE_DIR}/Aaa.txt", b"P2 7\nP3 8\n"))
    kn = collect(link, P.KNOBS, since=mark, timeout=1.5, n_min=2, pred=lambda d: d["voice"] == 2)
    wrote = logs_since(link, lmark, r"^card: wrote .*/Aaa\.txt")
    check("8: a 'W' while a load waits: decided again on the new list, 1C Bright (not Dark)",
          bool(wrote) and [k["knobs"] for k in kn] == [[(1, 120), (2, 30)]], f"{[k['knobs'] for k in kn]} {wrote}")
    check("8: Aaa.txt removed", remove_extra(link, "Aaa.txt") is True)
    # paging: 21 names, page 0 (two messages of 8) and page 1 (5)
    for i in range(18):
        put_extra(link, f"N{i:02d}.txt", b"P2 1\n")
    head = meta_head(link, 4)
    gen = head["gen"] if head else 0
    p0 = preset_names(link, gen, 0, n_min=2)
    p1 = preset_names(link, gen, 1)
    want = ["Bright", "Dark"] + [f"N{i:02d}" for i in range(18)] + ["Wide"]
    got0 = [e for d in p0 for e in d["entries"]]
    got1 = [e for d in p1 for e in d["entries"]]
    check("8: 21 presets: page 0 = 16 names in two messages (the first replaces), page 1 = the last 5",
          len(p0) == 2 and p0[0]["replace"] and not p0[1]["replace"] and all(d["total"] == 21 for d in p0 + p1) and
          got0 == list(enumerate(want[:16])) and got1 == [(16 + i, n) for i, n in enumerate(want[16:])], f"{got0} | {got1}")
    # a card error mid-scan: no rescan loop, 1B answered with total 0, 1A/1D still answered, no load
    lmark = len(link.logs)
    link.s.write(wframe(3, PRE_DIR))
    err = link.wait_log(r"^presets U4 rytm\.v2_test: card error", None, 3, since=lmark)
    ls = preset_names(link, 0, 0)
    mark = len(link.sysex)
    link.report(params(U4 | 2, [0, 64, 64, 64, 0, 0, 0, 127]))
    for _ in range(4):
        link.report(P.ask(4, 0, P.ASK_PRESETS, 0))
        pump(link, 0.3)
    head = meta_head(link, 4)
    kn = collect(link, P.KNOBS, since=mark, timeout=0.2, pred=lambda d: d["voice"] == 2)
    scans = logs_since(link, lmark, r"^presets U4 ")
    check("8: a card error mid-scan: one scan, 1B total 0, 1A says no presets, no 1C",
          err is True and len(scans) == 1 and bool(ls) and ls[0]["total"] == 0 and head is not None and
          not (head["flags"] & P.FLAG_PRESETS) and not kn, f"{scans} {ls[:1]} {head} {len(kn)} 1C")
    lmark = len(link.logs)
    remove_extra(link, "N17.txt")   # a 'W' to the folder ends the back-off
    ok = link.wait_log(r"^presets U4 rytm\.v2_test: 20$", None, 3, since=lmark)
    check("8: after a 'W' the folder is scanned again (20 presets)", ok is True)
    # a card error's back-off ends by itself after 5 s (no 'W'): no rescan before, one on demand after
    lmark = len(link.logs)
    link.s.write(wframe(3, PRE_DIR))
    err = link.wait_log(r"^presets U4 rytm\.v2_test: card error", None, 3, since=lmark)
    t0 = time.time()
    while time.time() - t0 < 4.0:
        link.report(P.ask(4, 0, P.ASK_PRESETS, 0))
        pump(link, 0.5)
    early = logs_since(link, lmark, r"^presets U4 ")
    while time.time() - t0 < 5.5:
        pump(link, 0.1)
    rmark = len(link.logs)
    link.report(P.ask(4, 0, P.ASK_PRESETS, 0))
    ok = link.wait_log(r"^presets U4 rytm\.v2_test: 20$", None, 3, since=rmark)
    check("8: a card error: no rescan for 4 s, then (5 s on, no 'W') scanned again on demand (20 presets)",
          err is True and len(early) == 1 and ok is True, f"{early}")
    for i in range(17):
        remove_extra(link, f"N{i:02d}.txt")


def step8_presets(link):
    """U4's presets: listed (1B 0x20), PRE loads with write-back (1C), a p-locked native one without."""
    check("8: U4 = rytm.v2_test ready", link.assign(4, "rytm.v2_test", "1.0.0") is True)
    try:
        for name, data in PRESETS.items():
            check(f"8: {PRE_DIR}/{name} on the card", link.put(f"{PRE_DIR}/{name}", data) is True)
        head = meta_head(link, 4, lambda h: bool(h["flags"] & P.FLAG_PRESETS))
        check("8: U4's 1A says presets exist", head is not None, str(head))
        gen = head["gen"] if head else 0
        mark = len(link.sysex)
        link.report(P.ask(4, gen, P.ASK_PRESETS, 0))
        ls = collect(link, P.LIST, since=mark, pred=lambda d: d["slot"] == 4 and d["list"] == P.LIST_PRESETS)
        check("8: the preset list, name order: Bright Dark Wide", bool(ls) and ls[0]["total"] == 3 and ls[0]["replace"] and
              [n for _, n in ls[0]["entries"]] == ["Bright", "Dark", "Wide"], str(ls[:1]))
        mark = len(link.sysex)
        link.report(params(U4 | 2, [0, 100, 64, 64, 0, 0, 0, 127]))   # voice 2 becomes U4, PRE 0
        kn = collect(link, P.KNOBS, since=mark, pred=lambda d: d["voice"] == 2)
        check("8: the voice's first report loads Bright, written back: 1C LEV 120 TON 30", bool(kn) and kn[0]["slot"] == 4 and
              kn[0]["knobs"] == [(1, 120), (2, 30)], str(kn[:1]))
        mark = len(link.sysex)
        link.report(params(U4 | 2, [1, 120, 30, 64, 0, 0, 0, 127]))   # PRE turned to 1
        kn = collect(link, P.KNOBS, since=mark, pred=lambda d: d["voice"] == 2)
        check("8: PRE 1 loads Dark: 1C LEV 40 TON 5", bool(kn) and kn[0]["knobs"] == [(1, 40), (2, 5)], str(kn[:1]))
        mark = len(link.sysex)
        lmark = len(link.logs)
        link.report(trig(U4 | 2, 69, 100, [2, 40, 5, 64, 0, 0, 0, 127]))   # a trig with PRE p-locked to 2 (Wide, native)
        pump(link, 0.3)
        a = render(link, 4800)
        kn = collect(link, P.KNOBS, since=mark, timeout=0.3, pred=lambda d: d["voice"] == 2)
        f, peak = pitch(a[480:, 2]), int(np.abs(a[:, 2]).max())
        played = any(t.startswith("preset voice 2 U4 2 Wide (played") for t in link.logs[lmark:])
        check("8: a p-locked PRE 2 plays Wide (native load_entry: +100 cents) and writes nothing back",
              played and not kn and abs(f - 466.16) < 3 and peak > 3000, f"{f:.1f} Hz, peak {peak}, {len(kn)} 1C")
        step8_more(link)
    finally:   # the card and the project as they were: the test files, their folder, U4 empty
        for name in list(PRESETS) + [n for n in EXTRA if n not in PRESETS]:
            if name in PRESETS or name in on_card:
                link.remove_file(f"{PRE_DIR}/{name}")
        on_card.clear()
        gone = link.remove_file(PRE_DIR)
        cleared = link.assign(4, "", "0.0.0")
    check("8: test presets removed from the card, U4 cleared", gone is True and cleared is True)
    link.sync(5, flush=False)
    sine_ok(link, "8")

def decode_listing(payload):
    """0E's payload -> (folder, total, [(kind, id, size, name)])."""
    b = payload
    out, i = [], 8
    while i + 8 <= len(b):
        n = b[i + 7]
        out.append((b[i], b[i + 1] << 7 | b[i + 2], b[i + 3] << 21 | b[i + 4] << 14 | b[i + 5] << 7 | b[i + 6],
                    bytes(b[i + 8:i + 8 + n]).decode("ascii", "replace")))
        i += 8 + n
    return b[0] << 7 | b[1], b[6] << 7 | b[7], out


DECODE[0x0E] = decode_listing
DECODE[P.POOL_TABLE] = P.decode_pool_table


def find_card_file(link, max_folders=8):
    """The biggest sample in the first max_folders folders of the TEENSY folder (the card's /Samples, breadth
    first; not JUNO or DX7) - likely sound all through its first frames: (folder, file, size)."""
    todo, seen, best = [0], 0, None
    while todo and seen < max_folders:
        f = todo.pop(0)
        seen += 1
        mark = len(link.sysex)
        link.report(HDR + bytes([0x0D, (f >> 7) & 0x7F, f & 0x7F, 0xF7]))
        first = collect(link, 0x0E, since=mark, timeout=2.0, pred=lambda d: d[0] == f)
        if not first:
            continue
        got = collect(link, 0x0E, n_min=max(1, (first[0][1] + 5) // 6), since=mark, timeout=2.0, pred=lambda d: d[0] == f)
        ents = [e for d in got for e in d[2]]
        for kind, eid, size, name in ents:
            if kind == 0 and (best is None or size > best[2]):
                best = (f, eid, size)
        todo += [eid for kind, eid, size, name in ents if kind == 1 and not (f == 0 and name in ("JUNO", "DX7"))]
    return best


def project(link, name):
    """10 <name>: the Teensy's project (pool, slot map, lists); True when it logged the change (None: no change)."""
    mark = len(link.logs)
    link.report(HDR + bytes([0x10]) + name.encode() + b"\xf7")
    return link.wait_log(rf"^project: {re.escape(name)}$", None, 5, since=mark)


def pool_tables(link, mark, timeout=3.0):
    """The pool table (14) the Teensy sent since mark, unasked: its messages from the last replacing one on,
    as [(slot, folder, file, name)] (None: no 14 arrived)."""
    tabs = collect(link, P.POOL_TABLE, since=mark, timeout=timeout)
    if not tabs:
        return None
    last = max(i for i, (rep, _) in enumerate(tabs) if rep) if any(rep for rep, _ in tabs) else 0
    return [e for _, ents in tabs[last:] for e in ents]


def step9_pool(link, home):
    """A pool knob: a card sample in a test project's pool, played by U4 in POOL mode; past the head without PSRAM.
    The pool table (14) is pushed by the Teensy on every change (the Rytm asks with 13 only once)."""
    hit = find_card_file(link)
    if hit is None:
        print("SKIP: 9: no sample on the Teensy's card")
        return
    f, e, size = hit
    mark = len(link.sysex)
    changed = project(link, "E2ETEST")
    check("9: project E2ETEST", changed is not False)
    if changed:
        check("9: a project change pushes its pool table (14)", pool_tables(link, mark) is not None)
    check("9: U4 = rytm.v2_test in E2ETEST", link.assign(4, "rytm.v2_test", "1.0.0") is True)
    mark = len(link.sysex)
    link.report(HDR + bytes([0x0F, (f >> 7) & 0x7F, f & 0x7F, (e >> 7) & 0x7F, e & 0x7F, 0xF7]))   # LOAD TO PROJ
    tab = pool_tables(link, mark) or []
    slot = next((s for (s, fo, fi, _) in tab if fo == f and fi == e), None)
    check("9: LOAD TO PROJ pushes the pool table (14) with the sample", slot is not None,
          f"folder {f} file {e} ({size} B) -> slot {slot}")
    if slot is None:
        project(link, home)
        return
    lmark = len(link.logs)
    link.report(trig(U4 | 3, 60, 100, [0, 127, 64, 64, slot, 127, 0, 127]))   # voice 3: MODE POOL, SMP slot
    render(link, 1024)               # its prepare is queued (the voice waits, silent)
    pump(link, 0.3)                  # loop passes: the prepare (the head), refills
    b = render(link, 9600)
    peak = int(np.abs(b[:, 3]).max())
    ready = link.wait_log(rf"^voice 3: pool slot {slot} for its plugin ready", None, 1, since=lmark)
    line = next((t for t in link.logs[lmark:] if t.startswith(f"voice 3: pool slot {slot} for its plugin ready")), "")
    check("9: the pool sample plays on U4 (pool_info/pool_read, no PSRAM)", peak > 50 and ready is True,
          f"peak {peak}, {line or 'not prepared'}")
    m = re.search(r"\((\d+) frames, (\d+) in the head", line)
    frames, hf = (int(m.group(1)), int(m.group(2))) if m else (0, 0)
    if hf and frames > hf + 1000:    # longer than the head: what follows it comes through the ring
        a, z = hf + 64, min(frames, 9600)
        nz = int(np.count_nonzero(b[a:z, 3]))
        check("9: past the head, streamed through the ring", nz > (z - a) // 2, f"{nz} non-zero of {z - a} (frames {a}..{z})")
    else:
        print(f"SKIP: 9: past the head - the sample fits its head ({line})")
    # ASSIGN (12) the file to a second slot: the table is pushed again; the voice's reader (another slot) is kept
    s2 = slot + 1 if slot < 127 else slot - 1
    mark, lmark = len(link.sysex), len(link.logs)
    link.report(P.pool_assign(s2, f, e))
    tab = pool_tables(link, mark) or []
    slots = sorted(s for (s, fo, fi, _) in tab if fo == f and fi == e)
    check(f"9: ASSIGN slot {s2} pushes the pool table (14) with both slots", slot in slots and s2 in slots, f"{slots}")
    render(link, 1024)               # voice 3 still on its slot
    pump(link, 0.3)
    again = logs_since(link, lmark, rf"^voice 3: pool slot {slot} for its plugin ready")
    check(f"9: a change of slot {s2} keeps the reader of slot {slot}", not again, str(again))
    # ASSIGN to the voice's own slot (the same file): its reader is prepared afresh
    lmark = len(link.logs)
    link.report(P.pool_assign(slot, f, e))
    render(link, 1024)               # voice 3 asks again after the change
    again = link.wait_log(rf"^voice 3: pool slot {slot} for its plugin ready", None, 2, since=lmark)
    check(f"9: a change of slot {slot} re-prepares the voice's reader", again is True)
    lmark = len(link.logs)
    link.report(trig(U4 | 3, 60, 100, [0, 127, 64, 64, s2, 127, 0, 127]))   # SMP -> the second slot, from frame 0
    render(link, 1024)
    pump(link, 0.3)
    b = render(link, 4800)
    peak = int(np.abs(b[:, 3]).max())
    ready = link.wait_log(rf"^voice 3: pool slot {s2} for its plugin ready", None, 1, since=lmark)
    check(f"9: SMP {s2} (the same file) plays", peak > 50 and ready is True, f"peak {peak}, prepared {ready}")
    # voice 3 back to DSP SAMPLE on the same file from STA 32 (its head: other bytes than the plugin's); v2_test
    # calls pool_info in voice_leave - nothing may be prepared into the voice's buffers after it left. Voice 4
    # plays the same sound fresh: both the same, within 5 % of the peak (their effect states differ by a few LSB:
    # max diff 0 and 49 of 2600 measured; a head overwritten with the file's first bytes differs by the signal)
    lmark = len(link.logs)
    dsp = [64, 64, 64, 64, 64, 30, 64, 100, 64, 64, s2, 0, 32, 127, 0, 100]   # ... SMP s2, STA 32, END 127, LEV 100
    link.s.write(sframe(trig(0x20 | 3, 60, 100, dsp)) + sframe(trig(0x20 | 4, 60, 100, dsp)))
    render(link, 1024)               # the prepares (the voices wait for their heads)
    pump(link, 0.3)
    link.s.write(sframe(trig(0x20 | 3, 60, 100, dsp)) + sframe(trig(0x20 | 4, 60, 100, dsp)))   # from the start, ready
    pump(link, 0.1)
    b = render(link, 4800)
    late = logs_since(link, lmark, r"^voice 3: pool slot \d+ for its plugin")
    d = int(np.abs(b[:, 3].astype(np.int32) - b[:, 4]).max())
    pk = int(np.abs(b[:, 4]).max())
    check("9: back to DSP SAMPLE after the plugin's pool: its sample plays as on a fresh voice (no late prepare)",
          pk > 50 and d <= pk // 20 and not late,
          f"peak {int(np.abs(b[:, 3]).max())}/{int(np.abs(b[:, 4]).max())}, max diff {d}, {late}")
    check("9: U4 cleared in E2ETEST", link.assign(4, "", "0.0.0") is True)
    mark, lmark = len(link.sysex), len(link.logs)
    back = project(link, home)
    check("9: back to the user's project", back is not False)
    if back:
        check("9: ... and its pool table pushed (14)", pool_tables(link, mark) is not None)
        link.wait_log(r"^plugins: U1 rytm\.sine_test ready", None, 10, since=lmark)   # its slot map reloaded
    link.sync(5, flush=False)
    sine_ok(link, "9")


def installed_list(link, timeout=10.0):
    """The installed machines as the Rytm's slot menu gets them (19 what 3 -> 1B 0x21): (gen, [names])."""
    mark = len(link.sysex)
    link.report(P.ask(0, 0, P.ASK_INSTALLED))
    first = collect(link, P.LIST, since=mark, timeout=timeout, pred=lambda d: d["list"] == P.LIST_INSTALLED)
    if not first:
        return None, []
    got = collect(link, P.LIST, n_min=max(1, (first[0]["total"] + 7) // 8), since=mark, timeout=timeout,
                  pred=lambda d: d["list"] == P.LIST_INSTALLED)
    return got[0]["gen"], [nm for d in got for _, nm in d["entries"]]


ans_secs = []   # every 1E's answer time (the Rytm waits 2 s)
last_ans = {}   # the last assign_1e's answer: the 1D (decoded) and its index in link.sysex


def assign_1e(link, slot, op, arg, gen, timeout=15.0):
    """1E, then the 1D answer pair for that slot: its result, None without one."""
    mark = len(link.sysex)
    t0 = time.time()
    link.report(P.assign(slot, op, arg, gen))
    ans = collect(link, P.SLOTS, since=mark, timeout=timeout, pred=lambda d: d["answer"] is not None and d["answer"][0] == slot)
    ans_secs.append(time.time() - t0 if ans else timeout)
    last_ans.clear()
    for k in range(mark, len(link.sysex)):
        c = P.parse(link.sysex[k])
        d = P.decode_slots(c[1]) if c and c[0] == P.SLOTS else None
        if d and d["answer"] is not None and d["answer"][0] == slot:
            last_ans.update(d=d, at=k)
            break
    return ans[0]["answer"][1] if ans else None


def later_1ds(link, at):
    """The 1Ds the Teensy sent after link.sysex[at] (decoded)."""
    out = []
    for m in link.sysex[at + 1:]:
        c = P.parse(m)
        d = P.decode_slots(c[1]) if c and c[0] == P.SLOTS else None
        if d:
            out.append(d)
    return out


def card_package(mid, version="1.0.0", pad_to=0):
    """rytm.v2_test's image under another id (and version), its manifest padded with zeros to about pad_to bytes."""
    man = json.load(open(V2_MAN))
    man["id"], man["version"] = mid, version
    with tempfile.TemporaryDirectory() as d:
        mp = os.path.join(d, "m.json")
        with open(mp, "w") as f:
            json.dump(man, f)
        data = rmp_pack.pack(V2_ELF, mp)
    pad = max(0, pad_to - len(data)) // 4 * 4
    if pad:
        h = list(struct.unpack(rmp_pack.HEADER_FMT, data[:96]))
        body = data[96:] + b"\0" * pad
        h[rmp_pack.HEADER_KEYS.index("manifest_size")] += pad
        h[rmp_pack.HEADER_KEYS.index("crc32")] = zlib.crc32(body) & 0xFFFFFFFF
        data = struct.pack(rmp_pack.HEADER_FMT, *h) + body
    return data


def step10_slot_menu(link, home):
    """What the Rytm's slot menu does: the installed list, assign and clear by 1E, a stale list refused; a 1E
    straight after a project change (its reload and slot loads queued ahead of it) answered within 2 s."""
    gen, names = installed_list(link)
    check("10: the installed machines (1B 0x21) include V2 TEST and SINE TEST", "V2 TEST" in names and "SINE TEST" in names,
          f"gen {gen}: {names}")
    if "V2 TEST" not in names:
        return
    i = names.index("V2 TEST")
    # with a preset folder (scanned after the load and the description): the answer waits for the scan, and its
    # U5 generation is the one U5's next 1A carries (0021 us_tick3 takes the defaults at the answer's generation)
    try:
        put = [link.put(f"{PRE_DIR}/{name}", data) for name, data in PRESETS.items()]
        check(f"10: {PRE_DIR} with {len(PRESETS)} presets on the card", all(x is True for x in put), str(put))
        check("10: 1E assign U5 = V2 TEST -> READY", assign_1e(link, 5, P.OP_ASSIGN, i, gen) == P.RES_READY)
        ans_gen = last_ans["d"]["gens"][4] if last_ans else None
        pump(link, 1.0)   # settled: a push with a newer generation would have come by now
        moved = [d["gens"][4] for d in later_1ds(link, last_ans["at"]) if d["gens"][4] != ans_gen] if last_ans else []
        h = meta_head(link, 5)
        check("10: U5 says READY, V2 TEST", h is not None and h["state"] == P.ST_READY and h["name"] == "V2 TEST", str(h))
        check("10: the READY answer's U5 generation is U5's next 1A's (presets flag set), no 1D moved it after",
              h is not None and ans_gen is not None and h["gen"] == ans_gen and bool(h["flags"] & P.FLAG_PRESETS) and not moved,
              f"answer gen {ans_gen}, 1A gen {h and h['gen']}, flags {h and h['flags']}, later U5 gens {moved}")
    finally:
        for name in PRESETS:
            link.remove_file(f"{PRE_DIR}/{name}")
        link.remove_file(PRE_DIR)
    check("10: a stale installed generation is refused (NOT INSTALLED)",
          assign_1e(link, 5, P.OP_ASSIGN, i, gen % 127 + 1) == P.RES_NOT_INSTALLED)
    check("10: 1E clear U5 -> READY (done)", assign_1e(link, 5, P.OP_CLEAR, 0, gen) == P.RES_READY)
    h = meta_head(link, 5)
    check("10: U5 says EMPTY", h is not None and h["state"] == P.ST_EMPTY and h["name"] == "", str(h))
    if home and project(link, "E2ETEST") is True:
        # E2ETEST with three machines to load, then back home; 10 E2ETEST and a 1E for its U5 in one write
        loads = [link.assign(u, mid, "1.0.0") for u, mid in ((1, "rytm.sine_test"), (2, "rytm.v2_test"), (3, "rytm.fault_test"))]
        check("10: E2ETEST's U1..U3 assigned", all(x is True for x in loads), str(loads))
        check("10: back to the user's project", project(link, home) is True)
        link.sync(5, flush=False)
        lmark, smark = len(link.logs), len(link.sysex)
        t0 = time.time()
        link.s.write(sframe(HDR + bytes([0x10]) + b"E2ETEST\xf7") + sframe(P.assign(5, P.OP_ASSIGN, i, gen)))
        ans = collect(link, P.SLOTS, since=smark, timeout=5, pred=lambda d: d["answer"] is not None and d["answer"][0] == 5)
        secs = time.time() - t0
        ans_secs.append(secs if ans else 5.0)
        res = ans[0]["answer"][1] if ans else None
        link.wait_log(r"^plugins: U5 answered", None, 3, since=lmark)   # (logged after the 1D)
        said = [t for t in link.logs[lmark:] if t.startswith("plugins: U5 answered")]
        check("10: a 1E straight after a project change (three slots to load) -> READY or LOADING within 2 s",
              res in (P.RES_READY, P.RES_LOADING) and secs < 2.0, f"result {res}, {secs:.2f} s, {said[:1]}")
        link.wait_log(r"^plugins: U5 rytm\.v2_test ready", None, 10, since=lmark)
        for u in (1, 2, 3, 5):
            link.assign(u, "", "0.0.0")
        lmark = len(link.logs)
        check("10: back to the user's project after E2ETEST", project(link, home) is True)
        link.wait_log(r"^plugins: U1 rytm\.sine_test ready", None, 10, since=lmark)
        link.sync(5, flush=False)
    check("10: each 1E answered within 2 s", max(ans_secs) < 2.0, " ".join(f"{t:.2f}" for t in ans_secs))


def step11_install_from_card(link, home):
    """On the card, not in flash: MISSING with the 'on card' flag; STOP TO INSTALL while the transport runs;
    a header of another version refused; installed and READY when it is stopped; duplicate names; a copy
    paused by a Play."""
    path = "/MACHINES/rytm.v2_card-1.0.0.rmp"
    link.remove("rytm.v2_card")                          # not in flash (an earlier run's)
    check("11: rytm.v2_card on the card's /MACHINES (its header says 1.0.1)",
          link.put(path, card_package("rytm.v2_card", "1.0.1")) is True)
    link.assign(3, "rytm.v2_card", "1.0.0")             # the map names it; not installed: MISSING
    h = meta_head(link, 3, lambda h: h["state"] == P.ST_MISSING and bool(h["flags"] & P.FLAG_ON_CARD))
    check("11: U3 MISSING, its package on the card (1A flag bit 1)", h is not None, str(h))
    link.transport(True)
    check("11: install while the transport runs -> STOP TO INSTALL", assign_1e(link, 3, P.OP_INSTALL, 0, 0) == P.RES_STOP_TO_INSTALL)
    link.transport(False)
    time.sleep(1.2)                                      # and no trig for a second (the fallback)
    check("11: a card file whose header is another version -> BAD", assign_1e(link, 3, P.OP_INSTALL, 0, 0) == P.RES_BAD)
    check("11: rytm.v2_card 1.0.0 on the card", link.put(path, card_package("rytm.v2_card")) is True)
    check("11: install with the transport stopped -> READY", assign_1e(link, 3, P.OP_INSTALL, 0, 0, timeout=20) == P.RES_READY)
    h3 = meta_head(link, 3)
    check("11: U3 says READY, V2 TEST or V2 TEST~2 (rytm.v2_card)",
          h3 is not None and h3["state"] == P.ST_READY and h3["name"] in ("V2 TEST", "V2 TEST~2"), str(h3))
    # two installed packages named V2 TEST: unique in the list, and each slot's 1A says its own entry's name
    gen, names = installed_list(link)
    check("11: duplicate names listed once each: V2 TEST and V2 TEST~2", len(set(names)) == len(names) and
          "V2 TEST" in names and "V2 TEST~2" in names, f"gen {gen}: {names}")
    link.assign(5, "rytm.v2_test", "1.0.0")
    h3 = meta_head(link, 3, lambda h: h["name"] in names)
    h5 = meta_head(link, 5, lambda h: h["state"] == P.ST_READY)
    check("11: U3 (rytm.v2_card) and U5 (rytm.v2_test) say different list names", h3 is not None and h5 is not None and
          h3["name"] != h5["name"] and {h3["name"], h5["name"]} <= set(names), f"U3 {h3 and h3['name']}, U5 {h5 and h5['name']}")
    link.assign(5, "", "0.0.0")
    # a 60 KB package: its copy (30 flash writes) paused by a Play midway, finished after the Stop
    big = card_package("rytm.v2_big", pad_to=60000)
    bpath = "/MACHINES/rytm.v2_big-1.0.0.rmp"
    link.remove("rytm.v2_big")
    check(f"11: rytm.v2_big ({len(big)} B) on the card", link.put(bpath, big) is True)
    link.assign(3, "rytm.v2_big", "1.0.0")
    h = meta_head(link, 3, lambda h: h["state"] == P.ST_MISSING and bool(h["flags"] & P.FLAG_ON_CARD))
    check("11: U3 = rytm.v2_big MISSING, on the card", h is not None, str(h))
    lmark, smark = len(link.logs), len(link.sysex)
    t0 = time.time()
    link.report(P.assign(3, P.OP_INSTALL, 0, 0))
    started = link.wait_log(r"^plugins: U3 installing /MACHINES/rytm\.v2_big", None, 3, since=lmark)
    link.s.write(b"T\x01")                                # Play, as the bridge relays MIDI Start
    ans = collect(link, P.SLOTS, since=smark, timeout=3, pred=lambda d: d["answer"] is not None and d["answer"][0] == 3)
    secs = time.time() - t0
    check("11: a copy paused by Play -> LOADING within 2 s", started is True and bool(ans) and
          ans[0]["answer"][1] == P.RES_LOADING and secs < 2.0, f"{ans[:1]}, {secs:.2f} s")
    check("11: a 1E on the slot while its copy runs -> BAD at once", assign_1e(link, 3, P.OP_ASSIGN, 0, 0) == P.RES_BAD)
    pump(link, 1.0)
    done = [t for t in link.logs[lmark:] if "rytm.v2_big installed from the card" in t]
    check("11: no flash write while the transport runs (the copy waits)", not done, done[0] if done else "")
    link.transport(False)
    fin = link.wait_log(r"^plugins: U3 rytm\.v2_big installed from the card", r"^plugins: U3 card copy failed", 10, since=lmark)
    check("11: ... and finishes after the Stop", fin is True)
    h = meta_head(link, 3, lambda h: h["state"] == P.ST_READY)
    check("11: U3 says READY (rytm.v2_big)", h is not None, str(h))
    # a project change while the copy runs (the slot now names another machine): READY, not BAD
    if home:
        link.assign(3, "", "0.0.0")
        link.remove("rytm.v2_big")                       # out of flash again; the card copy stays
        link.assign(3, "rytm.v2_big", "1.0.0")
        meta_head(link, 3, lambda h: h["state"] == P.ST_MISSING and bool(h["flags"] & P.FLAG_ON_CARD))
        lmark, smark = len(link.logs), len(link.sysex)
        t0 = time.time()
        link.report(P.assign(3, P.OP_INSTALL, 0, 0))
        link.wait_log(r"^plugins: U3 installing /MACHINES/rytm\.v2_big", None, 3, since=lmark)
        link.report(HDR + bytes([0x10]) + b"E2ETEST\xf7")   # once the copy runs
        ans = collect(link, P.SLOTS, since=smark, timeout=3, pred=lambda d: d["answer"] is not None and d["answer"][0] == 3)
        secs = time.time() - t0
        # READY: the copy (30 x 2 KB, ~0.5 s) ends inside the 1 s before LOADING would go out (E2ETEST's map
        # has nothing to load ahead of it)
        check("11: a project change during the copy -> READY within 2 s",
              bool(ans) and ans[0]["answer"][1] == P.RES_READY and secs < 2.0, f"{ans[:1]}, {secs:.2f} s")
        fin = link.wait_log(r"^plugins: U3 rytm\.v2_big installed from the card", r"^plugins: U3 card copy failed", 10, since=lmark)
        check("11: ... and the package is installed", fin is True)
        check("11: back to the user's project", project(link, home) is True)
        link.wait_log(r"^plugins: U3 rytm\.v2_big ready", None, 10, since=lmark)
    link.assign(3, "", "0.0.0")
    if home:
        # 10 to a project whose U3 is MISSING with its package on the card, and a 1E install of U3 in the same
        # write: the 1E is about that project's map (its reload first), not the one before (U3 empty there)
        link.remove("rytm.v2_card")                      # out of flash; its card copy stays
        if project(link, "E2ETEST") is True:
            link.assign(3, "rytm.v2_card", "1.0.0")
            meta_head(link, 3, lambda h: h["state"] == P.ST_MISSING and bool(h["flags"] & P.FLAG_ON_CARD))
            check("11: back to the user's project (U3 empty there)", project(link, home) is True)
            link.sync(5, flush=False)
            lmark, smark = len(link.logs), len(link.sysex)
            t0 = time.time()
            link.s.write(sframe(HDR + bytes([0x10]) + b"E2ETEST\xf7") + sframe(P.assign(3, P.OP_INSTALL, 0, 0)))
            ans = collect(link, P.SLOTS, since=smark, timeout=5, pred=lambda d: d["answer"] is not None and d["answer"][0] == 3)
            secs = time.time() - t0
            ans_secs.append(secs if ans else 5.0)
            res = ans[0]["answer"][1] if ans else None
            check("11: 10 E2ETEST + 1E install U3 in one write -> READY (or LOADING) within 2 s, the new project's map",
                  res in (P.RES_READY, P.RES_LOADING) and secs < 2.0, f"result {res}, {secs:.2f} s")
            fin = link.wait_log(r"^plugins: U3 rytm\.v2_card installed from the card",
                                r"^plugins: U3 card copy failed|^plugins: U3 installing /MACHINES/(?!rytm\.v2_card-)", 10, since=lmark)
            check("11: ... and it installs rytm.v2_card (E2ETEST's U3)", fin is True,
                  next((t for t in link.logs[lmark:] if "installing" in t or "card copy" in t), ""))
            h = meta_head(link, 3, lambda h: h["state"] == P.ST_READY)
            check("11: U3 says READY in E2ETEST", h is not None, str(h))
            link.assign(3, "", "0.0.0")
            check("11: back to the user's project", project(link, home) is True)
            link.sync(5, flush=False)
    check("11: every 1E answered within 2 s", max(ans_secs) < 2.0, f"longest {max(ans_secs):.2f} s of {len(ans_secs)}")
    for mid, fp in (("rytm.v2_card", path), ("rytm.v2_big", bpath)):
        link.remove(mid)
        link.remove_file(fp)


def step12_sound_tag(link):
    """The sound tag (spec 2.6): a 0x04's tag must be 0 or the slot's plugin fingerprint (1A part 0 carries it while
    READY); otherwise the voice is released, silent, its 0x03s dropped, logged once, until a matching trig."""
    fp = P.plugin_fp("rytm.sine_test")
    other = P.plugin_fp("rytm.v2_test")
    h = meta_head(link, 1, lambda h: h["state"] == P.ST_READY)
    check("12: U1's 1A part 0 (43 B) carries rytm.sine_test's fingerprint", h is not None and h["fp"] == fp,
          f"fp {h and h['fp']}, want {fp}")
    h5 = meta_head(link, 5)
    check("12: an EMPTY slot's 1A carries fp 0", h5 is not None and h5["fp"] == 0, str(h5 and h5["fp"]))
    sine = [64, 127, 127]
    # a matching tag plays; an untagged sound (0) plays
    for vi, tag, what in ((0, fp, "a matching tag"), (1, 0, "tag 0 (untagged)")):
        link.report(trig(U1 | vi, 69, 100, sine, tag))
        a = render(link, 4800)
        f, pk = pitch(a[480:, vi]), int(np.abs(a[:, vi]).max())
        check(f"12: {what} plays (voice {vi} at ~440 Hz)", abs(f - 440) < 3 and pk > 3000, f"{f:.1f} Hz, peak {pk}")
    # a mismatched tag: silent, its 0x03 dropped, logged once over two trigs
    lmark = len(link.logs)
    link.report(trig(U1 | 5, 69, 100, sine, other))
    link.report(params(U1 | 5, sine, other))
    link.report(trig(U1 | 5, 69, 100, sine, other))
    a = render(link, 4800)
    link.sync(5, flush=False)
    said = logs_since(link, lmark, r"^trig voice 5 U1 .*sound tag")
    check("12: a mismatched tag is silent (voice 5)", int(np.abs(a[:, 5]).max()) == 0, f"peak {int(np.abs(a[:, 5]).max())}")
    check("12: ... and logged once (two trigs)", len(said) == 1 and f"sound tag {other}, the slot's {fp}" in said[0], str(said))
    # a held note, then a mismatch: released; a matching 0x03 does not bring it back (a 0x03 never clears); a
    # note-off is harmless; the next matching trig plays afresh
    lmark = len(link.logs)
    link.report(trig(U1 | 6, 69, 100, sine, fp))
    a = render(link, 2400)
    pk0 = int(np.abs(a[:, 6]).max())
    link.report(trig(U1 | 6, 69, 100, sine, other))
    a = render(link, 4800)
    pk1 = int(np.abs(a[256:, 6]).max())
    link.report(params(U1 | 6, sine, fp))
    link.report(HDR + bytes([0x11, 6, 0xF7]))           # NOTE_OFF: forwarded, harmless on the released voice
    a = render(link, 4800)
    pk2 = int(np.abs(a[256:, 6]).max())
    check("12: a mismatch while a note is held releases it (no stuck note, a matching 0x03 after it)",
          pk0 > 3000 and pk1 == 0 and pk2 == 0, f"peak before {pk0}, after the mismatch {pk1}, after the 0x03 {pk2}")
    link.report(trig(U1 | 6, 69, 100, sine, fp))
    a = render(link, 4800)
    f, pk = pitch(a[480:, 6]), int(np.abs(a[:, 6]).max())
    check("12: ... and the matching trig after it plays afresh", abs(f - 440) < 3 and pk > 3000, f"{f:.1f} Hz, peak {pk}")
    link.sync(5, flush=False)
    # a slot reload (U1 cleared and assigned again) and a project change clear the bits: voice 5 mismatches anew
    for what in ("a slot reload", "a project change"):
        if what == "a slot reload":
            ok = link.assign(1, "", "0.0.0") is True and link.assign(1, "rytm.sine_test", "1.0.0") is True
        else:
            home = (link.info() or {}).get("project", "")
            lmark = len(link.logs)
            ok = bool(home) and project(link, "E2ETEST") is True and project(link, home) is True and \
                link.wait_log(r"^plugins: U1 rytm\.sine_test ready", None, 10, since=lmark) is True
        link.sync(5, flush=False)
        lmark = len(link.logs)
        link.report(trig(U1 | 5, 69, 100, sine, other))
        render(link, 1024)
        link.sync(5, flush=False)
        said = logs_since(link, lmark, r"^trig voice 5 U1 .*sound tag")
        check(f"12: {what} clears the mismatch (voice 5 logged again)", ok and len(said) == 1, f"{ok}, {said}")
    # a LOADING slot causes no mismatch (it has no fingerprint yet): trigs with the right tag stream in while U1
    # loads; the ones that land in LOADING log "not playing", none logs a tag mismatch, and the voice plays after
    hit = False
    for _ in range(5):
        link.assign(1, "", "0.0.0")
        link.sync(5, flush=False)
        lmark = len(link.logs)
        link.s.write(assign_frame(1, "rytm.sine_test", "1.0.0"))
        t0 = time.time()
        while time.time() - t0 < 1.0:
            link.s.write(sframe(trig(U1 | 4, 69, 100, sine, fp)))
            link.frames(32)
        link.wait_log(r"^plugins: U1 rytm\.sine_test ready", None, 5, since=lmark)
        link.sync(5, flush=False)
        hit = bool(logs_since(link, lmark, r"^trig voice 4 U1: not playing"))
        bad = logs_since(link, lmark, r"^trig voice 4 U1 .*sound tag")
        if hit or bad:
            break
    check("12: trigs with the slot's tag during LOADING: no mismatch", hit and not bad, f"hit LOADING {hit}, {bad}")
    link.report(trig(U1 | 4, 69, 100, sine, fp))
    a = render(link, 4800)
    f = pitch(a[480:, 4])
    check("12: ... and the voice plays once U1 is READY", abs(f - 440) < 3, f"{f:.1f} Hz")
    link.sync(5, flush=False)
    # a built-in machine's 0x04 with a nonzero tag: the tag concerns the user slots only - the voice plays its
    # P1..P16 as sent (PLAITS 2-OP FM, VOL 127; the spectrum's peak: tagged as untagged at TUN 64, doubled at TUN 76),
    # and nothing is logged
    def peak_hz(x):
        sp = np.abs(np.fft.rfft(x.astype(np.float64) * np.hanning(len(x)), 1 << 17))
        return float(np.argmax(sp)) * RATE / (1 << 17)
    lmark = len(link.logs)
    got = []
    for tag, tun in ((0, 64), (other, 64), (other, 76)):
        link.report(trig(0x00 | 7, 69, 100, [56, 64, 0, 0, 127, 127, tun, 127] + [0] * 7 + [127], tag))
        a = render(link, 4800)
        got.append((round(peak_hz(a[960:, 7]), 2), int(np.abs(a[960:, 7]).max())))
    link.sync(5, flush=False)
    said = logs_since(link, lmark, r"^trig voice 7 .*sound tag")
    (f0, p0), (f1, p1), (f2, p2) = got
    check("12: a built-in machine (PLAITS) with a nonzero tag plays its params as sent (as untagged; TUN +12: x2), no "
          "tag log", min(p0, p1, p2) > 1000 and f0 > 0 and abs(f1 / f0 - 1) < 0.01 and abs(f2 / f1 - 2) < 0.02 and not said,
          f"(Hz, peak) untagged {got[0]}, tagged {got[1]}, tagged TUN 76 {got[2]}; {said}")
    # a mismatched voice whose track moves to another U slot: the move clears its bit (plug_leave), so a mismatch on
    # the new slot is logged afresh (set -> set would log nothing)
    ok = link.assign(2, "rytm.v2_test", "1.0.0") is True
    link.sync(5, flush=False)
    lmark = len(link.logs)
    link.report(trig(U1 | 2, 69, 100, sine, other))
    render(link, 1024)
    link.report(trig(U2 | 2, 69, 100, sine, fp))   # sine_test's fp on U2 (rytm.v2_test): a mismatch there
    a = render(link, 2400)
    link.sync(5, flush=False)
    s1 = logs_since(link, lmark, r"^trig voice 2 U1 .*sound tag")
    s2 = logs_since(link, lmark, r"^trig voice 2 U2 .*sound tag")
    check("12: a mismatched voice moved to another U slot loses the bit (its mismatch there logged afresh, silent)",
          ok and len(s1) == 1 and len(s2) == 1 and int(np.abs(a[:, 2]).max()) == 0,
          f"U2 v2_test {ok}, U1 {s1}, U2 {s2}, peak {int(np.abs(a[:, 2]).max())}")
    # two tracks sharing voice 3, one with a foreign tag: each foreign trig after a matching one releases the voice
    # and is logged (clear -> set), each matching trig plays afresh
    lmark = len(link.logs)
    pk = []
    for tag in (fp, other, fp, other):
        link.report(trig(U1 | 3, 69, 100, sine, tag))
        a = render(link, 2400)
        pk.append(int(np.abs(a[256:, 3]).max()))
    link.sync(5, flush=False)
    said = logs_since(link, lmark, r"^trig voice 3 U1 .*sound tag")
    check("12: two tracks on one voice, one with a foreign tag: the matching one plays, the foreign one silences it "
          "(each time, logged each time)", pk[0] > 3000 and pk[1] == 0 and pk[2] > 3000 and pk[3] == 0 and len(said) == 2,
          f"peaks {pk}, {len(said)} log lines")
    link.assign(2, "rytm.fault_test", "1.0.0")   # U2 as steps 2-4 left it in the project's map
    link.sync(5, flush=False)


def plain_1ds(link, since):
    """The 1Ds without an answer pair the Teensy sent since `since` (index into link.sysex)."""
    return [d for d in collect(link, P.SLOTS, n_min=10 ** 6, timeout=0, since=since) if d["answer"] is None]


def step13_fresh_rytm(link, home):
    """A Rytm that starts while the Teensy runs has seen no 1D (us.seen 0: no 1E from its slot menu): the Teensy
    answers its 10 <project> with a 1D (at most once a second), and a 19 when no 1D went out for 3 s."""
    pump(link, 3.5)   # no 1D owed, the last one older than 3 s
    mark = len(link.sysex)
    link.report(HDR + bytes([0x10]) + home.encode() + b"\xf7")
    got = collect(link, P.SLOTS, since=mark, timeout=2.0, pred=lambda d: d["answer"] is None)
    check("13: a 10 <project> from a fresh Rytm gets a 1D", bool(got), str(got[:1]))
    if got:
        gen, names = installed_list(link)
        check("13: its installed generation is the installed list's", got[0]["installed"] == gen,
              f"1D {got[0]['installed']}, list {gen}")
        if "V2 TEST" in names:
            i = names.index("V2 TEST")
            check("13: then a 1E with the 1D's generation works (U5 = V2 TEST -> READY)",
                  assign_1e(link, 5, P.OP_ASSIGN, i, got[0]["installed"]) == P.RES_READY)
            check("13: and U5 cleared again", assign_1e(link, 5, P.OP_CLEAR, 0, got[0]["installed"]) == P.RES_READY)
            link.sync(5, flush=False)
    # the rate limit: 10s every 100 ms for 3.5 s -> at most one plain 1D a second
    pump(link, 1.2)
    mark = len(link.sysex)
    t0 = time.time()
    seen = []   # each plain 1D's arrival (to the 0.1 s pump)
    while time.time() - t0 < 5.0:
        link.report(HDR + bytes([0x10]) + home.encode() + b"\xf7")
        pump(link, 0.1)
        seen += [time.time() - t0] * (len(plain_1ds(link, mark)) - len(seen))
    gaps = [b - a for a, b in zip(seen, seen[1:])]
    # (arrivals seen to the pump's 0.1 s and the link's delays: a gap of 1 s may read as 0.8)
    check("13: 10s every 100 ms for 5 s -> a 1D at most once a second (4..6, gaps >= 0.7 s)",
          4 <= len(seen) <= 6 and min(gaps, default=0) >= 0.7, f"at {' '.join(f'{t:.2f}' for t in seen)} s")
    # a 19 alone: a 1D when the last is 3 s old, none for 19s within 3 s of it (the last 10's 1D up to 1 s on)
    pump(link, 4.5)
    mark = len(link.sysex)
    link.report(P.ask(1, 0, P.ASK_META))
    got = collect(link, P.SLOTS, since=mark, timeout=2.0, pred=lambda d: d["answer"] is None)
    check("13: a 19 after 3 s without a 1D gets one", bool(got))
    mark = len(link.sysex)
    t0 = time.time()
    while time.time() - t0 < 1.8:
        link.report(P.ask(1, 0, P.ASK_META))
        pump(link, 0.2)
    pump(link, 0.3)
    # a 1D whose generations differ from the one before is a genuine push (a slot or the list changed), not owed
    prev = (got[-1]["gens"], got[-1]["installed"]) if got else None
    n = 0
    for d in plain_1ds(link, mark):
        if (d["gens"], d["installed"]) == prev:
            n += 1
        prev = (d["gens"], d["installed"])
    check("13: 19s within 3 s of a 1D bring no other (pushes with new generations aside)", n == 0, f"{n} 1Ds")
    # the slot menu's list ask (19 what 3): a 1D owed with only the 1 s limit - inside the menu's 2 s wait
    mark = len(link.sysex)
    link.report(P.ask(0, 0, P.ASK_INSTALLED))
    got = collect(link, P.SLOTS, since=mark, timeout=1.0, pred=lambda d: d["answer"] is None)
    check("13: a 19 what 3 over 1 s after a 1D gets one at once", bool(got), str(got[:1]))


def console(link, line, ok, timeout=3.0):
    """A ':' console line; True when a log line matching ok follows."""
    mark = len(link.logs)
    link.s.write(b":" + line.encode() + b"\n")
    return link.wait_log(ok, None, timeout, since=mark) is True


def step14_out_main(link):
    """The host build's output choice (voiceloop ':out=main|tracks'; core/main_out.h): tracks at boot (unless
    -DRYTM_OUT_MAIN=1); notes on with tracks hints at main; with main the 'N' frames (no Overbridge stream) carry
    what the Rytm would get: the voices' sum in slot 0 (MAIN L/R), faded in from 0, at most -12 dBFS, the other
    slots silent. The serial build only says it is the host build's."""
    if not link.host:
        check("14: serial build: ':out=main' says the output is the host build's",
              console(link, "out=main", r"^output: the host build's"))
        return
    check("14: ':out' -> tracks at boot", console(link, "out", r"^output: tracks$"))
    check("14: ':notes=1' with tracks hints at ':out=main'", console(link, "notes=1", r"^output: tracks \(a stock Rytm"))
    console(link, "notes=0", r"^MIDI notes off")
    check("14: U1 = rytm.sine_test ready", link.assign(1, "rytm.sine_test", "1.0.0") is True)
    # The MAIN sum is all 8 voices: the earlier steps leave some ringing (a 4 s decay; voices 4 and 7 at 11000 and
    # 13000 peak, the latter ~399 Hz), which mix into the "sine" and moved its zero crossings (594 Hz) whenever the
    # decay stood high enough - a flake that does not depend on the FIFO. Retrigger voices 1..7 on U1 at LEVEL 0.
    for v in range(1, 8):
        link.report(trig(U1 | v, 69, 100, [64, 0, 127]))
    b = render(link, 2400)[1100:]   # the first 1024 frames may be ones the idle loop rendered before the trigs
    check("14: the other voices silenced first (the MAIN sum is all 8)", int(np.abs(b[:, 1:]).max()) == 0,
          f"peaks {[int(np.abs(b[:, v]).max()) for v in range(1, 8)]}")
    link.report(trig(U1 | 0, 69, 100, [64, 127, 127]))   # voice 0 at full level (DECAY ~4 s), playing
    render(link, 2400)                                   # before the switch (tracks)
    check("14: ':out=main' -> main", console(link, "out=main", r"^output: main$"))
    a = render(link, 14400)                              # 300 ms: the 200 ms fade and past it
    m = a[:, 0].astype(np.int32)
    head, mid, tail = (int(np.abs(m[i:j]).max()) for i, j in ((0, 480), (4320, 4800), (-4800, None)))
    check("14: main: the other slots silent", int(np.abs(a[:, 1:]).max()) == 0)
    check("14: main: at most -12 dBFS (8192), audible", 1000 < tail <= 8192, f"peak {tail}")
    check("14: main: faded in (the first 10 ms under a quarter of 90..100 ms's level)", 0 < head < mid // 4,
          f"first 10 ms {head}, at 90..100 ms {mid}, after 200 ms {tail}")
    f = pitch(a[-4800:, 0])
    check("14: main: the sine at ~440 Hz", abs(f - 440) < 3, f"{f:.1f} Hz")
    check("14: ':out=tracks' -> tracks", console(link, "out=tracks", r"^output: tracks$"))
    sine_ok(link, "14: tracks again")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--host", action="store_true", help="the Teensy runs the USB-host build (RYTM_USB_HOST=1)")
    a = ap.parse_args()
    try:
        run(Link(a.port, host=a.host))
    except Exception as e:   # a dead link, a timeout: a FAIL, not a traceback
        check(f"no exception ({type(e).__name__}: {e})", False)
    print(f"\n{'ALL PASS' if not fails else 'FAILED: ' + ', '.join(fails)}")
    sys.exit(1 if fails else 0)


def run(link):
    link.sync()
    print("teensy answering")

    # 1. U1: sine_test
    check("U1 = rytm.sine_test ready", link.assign(1, "rytm.sine_test", "1.0.0") is True)
    sine_ok(link, "1")

    # 2. U2: fault_test mode 1 (null write)
    ok = link.assign(2, "rytm.fault_test", "1.0.0")
    check("U2 = rytm.fault_test ready", ok is True)
    mark = len(link.logs)
    link.report(trig(U2 | 1, 60, 100, [40]))              # P1 40: mode 1
    b = render(link, 1024)
    # the mute is logged from loop(), after the audio frame
    crashed = link.wait_log(r"^plugins: U2 rytm\.fault_test CRASHED - muted", None, 3, since=mark)
    check("2: fault_test mode 1 -> 'U2 rytm.fault_test CRASHED - muted'", crashed is True)
    check("2: voice 1 silent", int(np.abs(b[:, 1]).max()) == 0)
    link.sync(5, flush=False)
    check("2: the Teensy still answers", True)
    sine_ok(link, "2")

    # 3. a fresh U2, mode 2 (endless loop)
    ok = link.assign(2, "rytm.fault_test", "1.0.0")
    check("3: U2 assigned afresh, ready", ok is True)
    mark = len(link.logs)
    link.report(trig(U2 | 1, 60, 100, [80]))              # P1 80: mode 2
    b = render(link, 1024)
    link.wait_log(r"^plugins: U2 rytm\.fault_test .* - muted", None, 3, since=mark)
    muted = [t for t in link.logs[mark:] if t.startswith("plugins: U2 rytm.fault_test") and "muted" in t]
    check("3: fault_test mode 2 -> timed out / over budget, muted", bool(muted), muted[0] if muted else "")
    check("3: voice 1 silent", int(np.abs(b[:, 1]).max()) == 0)
    link.sync(5, flush=False)
    check("3: the Teensy still answers", True)
    sine_ok(link, "3")

    # 4. a fresh U2, mode 3 (a render ~5x its declared CPU, no fault, no loop)
    ok = link.assign(2, "rytm.fault_test", "1.0.0")
    check("4: U2 assigned afresh, ready", ok is True)
    mark = len(link.logs)
    link.report(trig(U2 | 1, 60, 100, [100]))             # P1 100: mode 3
    render(link, 1024)
    link.wait_log(r"^plugins: U2 rytm\.fault_test .* - muted", None, 3, since=mark)
    muted = [t for t in link.logs[mark:] if t.startswith("plugins: U2 rytm.fault_test") and "muted" in t]
    check("4: fault_test mode 3 -> over budget (or timed out), muted", bool(muted), muted[0] if muted else "")
    link.sync(5, flush=False)
    sine_ok(link, "4")

    # 5. the device-info report ('D')
    info = link.info()
    check("5: info report received and parsed", isinstance(info, dict))
    if isinstance(info, dict):
        check("5: core firmware, plugin API, formats accepted",
              isinstance(info.get("fw"), str) and info.get("api") == API and info.get("formats") == [1, FORMAT],
              f"fw {info.get('fw')}, api {info.get('api')}, formats {info.get('formats')}")
        check("5: PSRAM MB and per-voice state limit", isinstance(info.get("psram_mb"), int) and
              info.get("state_limit", 0) >= 16384 and (info["psram_mb"] > 0 or info["state_limit"] == 16384),
              f"psram {info.get('psram_mb')} MB, limit {info.get('state_limit')}")
        ar = info.get("arena", {})
        check("5: arena size and use", ar.get("size") == 32768 and 0 < ar.get("used", 0) <= 32768, str(ar))
        sl = info.get("slots", [])
        check("5: five slots with the project", len(sl) == 5 and isinstance(info.get("project"), str) and info["project"], f"project {info.get('project')}")
        u1 = sl[0] if sl else {}
        check("5: U1 rytm.sine_test READY with declared and measured CPU",
              u1.get("id") == "rytm.sine_test" and u1.get("state") == "READY" and u1.get("permille") == 25 and
              u1.get("calls", 0) > 0 and u1.get("avg_cyc", 0) > 0 and u1.get("peak_cyc", 0) >= u1.get("avg_cyc", 0),
              str(u1))
        u2 = sl[1] if len(sl) > 1 else {}
        check("5: U2 rytm.fault_test is MUTED after its timeout", u2.get("id") == "rytm.fault_test" and u2.get("state") == "MUTED", str(u2))
        st = info.get("store", {})
        pk = {p.get("id"): p for p in st.get("pkgs", [])}
        check("5: store size, free, installed packages with versions",
              st.get("size", 0) > 0 and 0 < st.get("free", 0) <= st["size"] and
              pk.get("rytm.sine_test", {}).get("ver") == "1.0.0" and pk.get("rytm.fault_test", {}).get("size", 0) > 96, str(st)[:200])
        check("5: SD card flag and the /MACHINES list", isinstance(info.get("sd"), bool) and isinstance(info.get("machines"), list),
              f"sd {info.get('sd')}, machines {info.get('machines')}")
        check("5: built in passes of at most 6 ms (no audio stall)", info.get("pass_max_us", 1e9) <= 6000,
              f"{info.get('passes')} passes, longest {info.get('pass_max_us')} us, {info.get('build_us')} us in all")
        print("info:", {k: info[k] for k in ("passes", "pass_max_us", "build_us")})
    link.sync(5, flush=False)
    sine_ok(link, "5")

    # 6. a 20000-byte state (rytm.state_test on U3): the voice has 16384 B of its own, more only with PSRAM
    psram = (info or {}).get("psram_mb", 0)
    mark = len(link.logs)
    ok = link.assign(3, "rytm.state_test", "1.0.0")
    if not psram:
        bad = [t for t in link.logs[mark:] if "rytm.state_test needs 20000 B state, the voice has 16384" in t]
        check("6: no PSRAM: U3 rytm.state_test is refused", ok is False and bool(bad), bad[0] if bad else "no 'needs' line")
        i2 = link.info()
        u3 = (i2 or {}).get("slots", [{}] * 3)[2]
        check("6: info shows U3 BAD", u3.get("state") == "BAD", str(u3))
    else:
        check("6: PSRAM: U3 rytm.state_test ready", ok is True)
        for v in range(8):
            link.report(trig(0x60 | v, 69, 100, [64, 127, 127]))
        a = render(link, 4800)
        fs = [pitch(a[480:, v]) for v in range(8)]
        check("6: PSRAM: 8 voices play 440 Hz", all(abs(f - 440) < 3 for f in fs), " ".join(f"{f:.1f}" for f in fs))
    link.assign(3, "", "0.0.0")   # leave the project's map as it was
    link.sync(5, flush=False)
    step7_metadata(link)
    step8_presets(link)
    step9_pool(link, (info or {}).get("project", ""))
    step10_slot_menu(link, (info or {}).get("project", ""))
    step11_install_from_card(link, (info or {}).get("project", ""))
    step12_sound_tag(link)
    step13_fresh_rytm(link, (info or {}).get("project", ""))
    step14_out_main(link)


if __name__ == "__main__":
    main()
