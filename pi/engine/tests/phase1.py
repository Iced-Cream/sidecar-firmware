#!/usr/bin/env python3
"""Phase 1 acceptance (spec 2026-10-14-pi-engine-design.md): the Pi engine as rytm-emu's Overbridge host over the
direct Ethernet cable, the mod build (fw/AR173_us_us1.syx) in the emulator, nothing flashed.

    phase1.py base        regenerate the emulator base: a cold boot of the mod build on a copy of sections/led.img,
                          the startup dialogs answered, USB CONFIG = OVERBRIDGE and TRANSPORT SEND on (as rytm-mods'
                          tools/emu/us_base.sh), saved as rytm-emu sections/us_us1.v2.snap + .img (git-ignored there).
                          The old sections/us_us1.snap no longer loads (snapshot format changed) and is never touched.
                          The other commands run it themselves when the base is missing or does not load.
    phase1.py tracks      four tracks on Pi machines (BD PLAITS 44, SD 6-OP FM 40, LT DSP SAMPLE 45, CH DAISYSP 51,
                          set by DIN CC 15 as sim/obhost.py --ext does), a pattern recorded by key events and played:
                          each track's voice channel in the emulator's USB stream (--usb-wav) sounds, the Pi logs
                          trigs on the matching voices with the matching machines; a knob turn on CH changes its
                          sound; a p-lock on SD's step 5 reaches the engine (:reportlog=1); the screenshots show
                          SD's PAT as a ROM voice name (17) and LT's SMP as sine440 (14)
    phase1.py gaps SECS   the same setup played for SECS: the emulator's "usb OUT frames short" and the Pi's
                          output underruns constant after a 20 s warm-up
    phase1.py --emu-only tracks|gaps SECS
                          the emulator side alone, no Pi, no serial port, no network: the link on 127.0.0.1 with
                          sim/emu_link.py as the stand-in host (silent voice frames). Checks the base resumes, the
                          hello (flags 0x7), the CC 15 machines and the recorded pattern (the Rytm's 0x04 trigs to
                          the host, per voice and machine), the Rytm's USB MIDI to the host, the link's frame counters

Every run works on copies (a scratch dir): the base snapshot and card are never written. Needs, without
--emu-only: the Pi on the Task 4+ kernel, cabled, the Mac at 192.168.77.1 (the plan's one user step); rytm-emu built.
"""
import argparse
import ast
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import wave

import mido
import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "sim"))
import protocol as P  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
EMU = os.environ.get("RYTM_EMU", os.path.abspath(os.path.join(REPO, "..", "rytm-emu")))   # beside this checkout
OUT = os.path.join(REPO, "pi", "circle", "out")   # git-ignored (pi/circle/.gitignore)
# PHASE1_TAG: another rytm-mods build copied by its tools/emu/us_base.sh (fw/AR173_us_<TAG>.syx; e.g. sfx1 = us1 +
# 0025-sendfx-report), with its own base sections/us_<TAG>.v2.snap + .img. Default us1.
TAG = os.environ.get("PHASE1_TAG", "us1")
FW = os.path.join(EMU, "fw", f"AR173_us_{TAG}.syx")
BASE_SNAP = os.path.join(EMU, "sections", f"us_{TAG}.v2.snap")
BASE_CARD = os.path.join(EMU, "sections", f"us_{TAG}.v2.img")
# track: (MIDI channel index, Rytm machine, physical voice, Rytm->host voice channel in --usb-wav, Pi log word)
TRACKS = {"BD": (0, 44, 0, 2, "engine"), "SD": (1, 40, 2, 3, "6-OP FM"), "LT": (5, 45, 3, 6, "DSP SAMPLE"),
          "CH": (8, 51, 4, 8, "DAISYSP")}
# CH's STR (knob B) turn, in detents: 40 takes it 64 -> 127 (measured 2026-10-15). +8 (64 -> 79) moved the spectral
# centroid of the Pi's DAISYSP under 2 % in three runs, within its own spread from second to second: band_dist now
CH_STR_STEPS = 40
LT_SMP = 1   # LT's SMP: the pool slot cmd_tracks assigns pi-test/sine440.wav to (pool=1:...)
fails = []


def check(what, ok, detail=""):
    print(f"{'PASS' if ok else 'FAIL'}: {what}{(' - ' + detail) if detail else ''}", flush=True)
    if not ok:
        fails.append(what)


def key_codes():
    src = open(os.path.join(EMU, "src", "panel.rs")).read()
    table = src[src.index("pub const RYTM_KEYS"):]
    table = table[:table.index("];")]
    names = re.findall(r'\("([A-Z0-9_]+)",', table)
    base = int(re.search(r"pub const RYTM_CODE_BASE: u8 = (\d+);", src).group(1))
    return {n: base + i for i, n in enumerate(names)}


# ---- the emulator base (rytm-mods tools/emu/us_base.sh's walk, into new files)

def dtboot(*args):
    r = subprocess.run([os.path.join(EMU, "target", "release", "dtboot"), *args], cwd=EMU, capture_output=True, text=True)
    out = r.stdout + r.stderr
    if r.returncode != 0 or "FAULT" in out:
        sys.exit(f"dtboot failed ({r.returncode}):\n" + "\n".join(out.splitlines()[-20:]))
    return out


def peek(snap, addr):
    r = subprocess.run([os.path.join(EMU, "target", "release", "dtpeek"), snap, "mem", addr, "1"], cwd=EMU,
                       capture_output=True, text=True, env=dict(os.environ, DTEMU_FW=FW))
    m = re.match(r"[0-9a-f]+: ([0-9a-f]{2})", r.stdout)
    return m.group(1) if m else "?"


def make_base():
    """Cold boot on a copy of led.img (YES NO NO NO), then GLOBAL > MIDI CONFIG > SYNC: TRANSPORT SEND and USB CONFIG:
    OVERBRIDGE, each toggled only when off (0x407c2d32 / 0x405c8e4c in this us build, as us_base.sh reads them).
    The card is saved with the snapshot (--card-save), so the pair resumes without a card mismatch."""
    with tempfile.TemporaryDirectory() as tmp:
        card, cold, snap = os.path.join(tmp, "base.img"), os.path.join(tmp, "cold.snap"), os.path.join(tmp, "base.snap")
        shutil.copyfile(os.path.join(EMU, "sections", "led.img"), card)
        dtboot(FW, "--instr", "4400M", "--every", "4400M", "--card", card, "--press", "YES@2600M", "--press", "NO@3000M",
               "--press", "NO@3400M", "--press", "NO@3800M", "--save", cold)
        ts, usb = peek(cold, "407c2d32"), peek(cold, "405c8e4c")
        keys = "GLOBAL DOWN DOWN DOWN YES YES DOWN DOWN DOWN".split() + ([] if ts == "01" else ["YES"]) + ["NO", "NO"]
        keys += "DOWN DOWN DOWN DOWN YES DOWN YES".split() + ([] if usb == "01" else ["YES"]) + ["NO", "NO", "NO"]
        presses, t = [], 4500
        for k in keys:
            presses += ["--press", f"{k}@{t}M"]
            t += 100
        end = t + 600
        dtboot(FW, "--load", cold, "--card", card, "--card-save", "--instr", f"{end - 4400}M", "--every", "100000M",
               "--save", snap, *presses)
        ts, usb = peek(snap, "407c2d32"), peek(snap, "405c8e4c")
        if (ts, usb) != ("01", "01"):
            sys.exit(f"base: transport send {ts}, usb config {usb} after the walk (want 01 01)")
        shutil.move(card, BASE_CARD)
        shutil.move(snap, BASE_SNAP)
    print(f"base: {BASE_SNAP} + {os.path.basename(BASE_CARD)} at {end}M (transport send 01, usb config 01)", flush=True)


def base_loads():
    if not (os.path.exists(BASE_SNAP) and os.path.exists(BASE_CARD)):
        return False
    with tempfile.TemporaryDirectory() as tmp:
        snap = os.path.join(tmp, "b.snap")
        shutil.copyfile(BASE_SNAP, snap)
        r = subprocess.run([os.path.join(EMU, "target", "release", "dtboot"), FW, "--load", snap, "--instr", "1M"],
                           cwd=EMU, capture_output=True, text=True)
    return r.returncode == 0


def run_copies():
    """A fresh scratch copy of the base (snapshot and card): the emulator writes its card back at exit."""
    if not base_loads():
        print("base missing or does not load: regenerating it", flush=True)
        make_base()
    d = tempfile.mkdtemp(prefix="phase1-")
    snap, card = os.path.join(d, "p1.snap"), os.path.join(d, "p1.img")
    shutil.copyfile(BASE_SNAP, snap)
    shutil.copyfile(BASE_CARD, card)
    return d, snap, card


# ---- the run

class PiLog(threading.Thread):
    """The Pi's serial log, read in the background; commands written to it."""

    def __init__(self, keep=None):
        import serial
        super().__init__(daemon=True)
        ports = sorted(glob.glob("/dev/cu.usbserial-*"))
        if not ports:
            sys.exit("no serial adapter (/dev/cu.usbserial-*)")
        self.s = serial.Serial(ports[0], 921600, timeout=0.2)
        self.lines, self.stop = [], False
        self.keep = open(keep, "w") if keep else None   # every line also into this file (pi/circle/out)
        if keep:
            print(f"Pi log: {keep}")
        self.start()

    def run(self):
        while not self.stop:
            l = self.s.readline().decode(errors="replace").strip()
            if l:
                self.lines.append((time.time(), l))
                if self.keep:
                    self.keep.write(l + "\n")
                    self.keep.flush()

    def cmd(self, c):
        self.s.write(b":" + c.encode() + b"\n")


class StandIn(threading.Thread):
    """--emu-only: sim/emu_link.py as the link's host (MIDI pair, no R, silent 'a' frames), its output kept."""

    def __init__(self, link, secs):
        super().__init__(daemon=True)
        self.p = subprocess.Popen([sys.executable, os.path.join(REPO, "sim", "emu_link.py"), "--link", link,
                                   "--secs", str(secs)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.lines = []
        self.start()

    def run(self):
        for l in self.p.stdout:
            self.lines.append(l.rstrip("\n"))

    def sysex(self):
        """The Rytm's USB MIDI SysEx seen by the host -> [(cmd, payload bytes)] (RT messages only)."""
        out = []
        for l in self.lines:
            if l.startswith("rytm sysex: ("):
                try:
                    out.append(ast.literal_eval(l[len("rytm sysex: "):]))
                except (ValueError, SyntaxError):
                    pass
        return out


def emulator(secs, events, png, wav, snap, card, link):
    """rytm-emu headless with the mod build, scratch copies of the base, the link, the USB stream recorded."""
    codes = key_codes()
    cmd = [os.path.join(EMU, "target", "release", "rytm"), FW, "--card", card, "--snapshot", snap, "--headless",
           "--midi", "--usb-host", "--audio-null", "--ob-link", link, "--usb-wav", wav, "--after", f"{secs:.1f}",
           "--screenshot", png]
    for ev in events:
        what, at = ev.split("@", 1)
        if what.startswith("TURN:"):
            _, enc, delta = what.split(":")
            cmd += ["--turn", f"{enc}:{delta}@{at}"]
        else:
            name = what.split(":")[0]
            cmd += ["--press", f"{codes[name]}@{at}" + (":" + what.split(":")[1] if ":" in what else "")]
    return subprocess.Popen(cmd, cwd=EMU, stderr=subprocess.PIPE, text=True)


def set_machines():
    """DIN CC 15 (the machine) on each track's channel, as obhost.py --ext; the ports come with --midi. Also LT's
    SMP (CC 27, the sample page's slot) = 1, the pool slot the Pi gets sine440 in: the base's LT has SMP 107 (measured
    2026-10-15 in its 0x03 reports, P11), beyond the pool, so DSP SAMPLE played silence. CC, not knob turns: the
    emulator's turns of the SMP knob are lossy (40 one-detent turns 0.15 s apart moved it 13)."""
    t0 = time.time()
    while time.time() - t0 < 15:
        names = [n for n in mido.get_output_names() if "rytm-emu in" in n and "usb" not in n]
        if names:
            with mido.open_output(names[0]) as o:
                for ch, mach, _, _, _ in TRACKS.values():
                    o.send(mido.Message("control_change", channel=ch, control=15, value=mach))
                o.send(mido.Message("control_change", channel=TRACKS["LT"][0], control=27, value=LT_SMP))
            return True
        time.sleep(0.5)
    return False


def knob_turns(enc, steps, t):
    """A knob change the firmware takes: a first turn (ignored: it only shows the parameter - measured with a
    debugger 2026-10-15, the stock OS does the same, rytm-emu src/machine.rs counts_per_detent), then `steps` turns of
    one detent 50 ms apart (as rytm-emu's own nudge_steps: small turns in quick succession). Measured 2026-10-15 on
    the p-lock: a wake turn followed by one large turn (+30 or +4 detents, 0.1 or 0.3 s later) left the parameter
    unchanged; seven one-detent turns 50 ms apart moved TIM 8192 -> 9344."""
    return [f"TURN:{enc}:1@{t + 0.05 * k:.2f}" for k in range(steps + 1)]


def pattern_events(t):
    """The base project's pattern A01 has trigs: FUNC + PLAY (CLEAR) empties it first (measured 2026-10-15: no step
    lit for SD in grid recording after it). Grid recording: each track's pad selects it, trig keys place steps; then
    SD again and its step 5 held while knob B (encoder 1, measured on the SRC page) turns +6 (a p-lock on TIM);
    RECORD off, PLAY. Returns the events and the time playback starts.
    Every knob change is knob_turns (the firmware ignores a knob's first turn).
    LT's track in A01 is 64 steps long and grid recording places its steps on page 1 only (measured 2026-10-15), so
    LT trigs a quarter as often as a 16-step track: 2 trigs per 64 steps, ~7 in the tracks run."""
    ev = [f"FUNC:0.6@{t - 1.2:.1f}", f"PLAY@{t - 0.9:.1f}", f"RECORD@{t:.1f}"]
    steps = {"BD": [1, 9], "SD": [5, 13], "LT": [3, 11], "CH": [1, 5, 9, 13]}
    for pad, st in steps.items():
        t += 0.6
        ev.append(f"{pad}@{t:.1f}")
        for s in st:
            t += 0.3
            ev.append(f"T{s}@{t:.1f}")
    t += 0.6
    ev.append(f"SD@{t:.1f}")
    t += 0.4
    ev += [f"T5:1.2@{t:.1f}", *knob_turns(1, 6, t + 0.4)]
    t += 1.6
    ev += [f"RECORD@{t:.1f}", f"PLAY@{t + 0.5:.1f}"]
    return ev, t + 1.0


def channel_rms(wav, ch, t0, t1):
    with wave.open(wav, "rb") as w:
        n, rate, chans = w.getnframes(), w.getframerate(), w.getnchannels()
        a = np.frombuffer(w.readframes(n), "<i2").reshape(-1, chans)
    x = a[int(t0 * rate):int(t1 * rate), ch].astype(np.float64) / 32768.0
    return float(np.sqrt(np.mean(x ** 2))) if len(x) else 0.0, x, rate


def band_db(x, rate):
    """Third-octave band energies 100 Hz..25.6 kHz, as a share of the whole, in dB."""
    s = np.abs(np.fft.rfft(x * np.hanning(len(x)))) ** 2
    f = np.fft.rfftfreq(len(x), 1 / rate)
    edges = 100 * 2 ** (np.arange(0, 25) / 3)
    e = np.array([s[(f >= a) & (f < b)].sum() for a, b in zip(edges[:-1], edges[1:])])
    return 10 * np.log10(e / max(e.sum(), 1e-30) + 1e-12)


def band_dist(a, b):
    """The mean |difference| of two band_db spectra, in dB. Measured 2026-10-15 on the Pi's DAISYSP CH, STR 64 ->
    127: 11.8 dB, each window's own halves 1.3 and 1.8 dB. The centroid moved 3.4 % only (STR shapes the partials'
    spacing more than the brightness)."""
    return float(np.mean(np.abs(a - b)))


REPORT = re.compile(r"overbridge link: host (\w+), flags (0x[0-9a-f]+), frames short (\d+)(?:, usb OUT frames short (\d+))?"
                    r", midi to host (\d+) B, from host (\d+) B")


def link_reports(err):
    """The emulator's 10 s link reports (src/machine.rs ob_link_report) -> [dict]."""
    out = []
    for l in err.splitlines():
        m = REPORT.search(l)
        if m:
            out.append(dict(host=m.group(1), flags=m.group(2), short=int(m.group(3)),
                            usb_short=int(m.group(4)) if m.group(4) else None, to=int(m.group(5)), frm=int(m.group(6))))
    return out


def emu_checks(err, png):
    """What the emulator's stderr shows in every mode."""
    check("the emulator resumed the base copy", "resumed " in err and "booting from reset" not in err,
          next((l for l in err.splitlines() if "resumed" in l or "booting from reset" in l), "no resume line"))
    check("a host connected to the link", "overbridge link: host connected from" in err,
          next((l for l in err.splitlines() if "host connected from" in l), ""))
    hello = re.search(r"overbridge link: host hello, flags (0x[0-9a-f]+)", err)
    check("the host's hello granted MIDI, no R, voice frames (0x7)", bool(hello) and hello.group(1) == "0x7",
          hello.group(0) if hello else "no hello line")
    check("the screenshot was written", os.path.exists(png) and os.path.getsize(png) > 0, png)


def emu_link_summary(err):
    print("\n".join(l for l in err.splitlines() if "overbridge link" in l)[-900:])


def tracks_events():
    ev, t_play = pattern_events(6.0)
    knob_at = t_play + 8.0
    ev += ["CH@%.1f" % (knob_at - 0.5), *knob_turns(1, CH_STR_STEPS, knob_at)]   # CH selected, DAISYSP's STR (knob B)
    after = knob_at + 0.05 * CH_STR_STEPS + 0.5   # the turns done
    ev += ["SD@%.1f" % (after + 4), "SRC@%.1f" % (after + 4.5)]   # end on SD's SRC page: PAT's value text
    return ev, t_play, knob_at, after, after + 7


def trig_checks_emu(si, t_play):
    """--emu-only: the Rytm's 0x04 reports to the host (rytm-mods 0021 ext_render): per track, its voice reported
    with a trig (CC 15 changed the machine, the pattern recorded and plays)."""
    msgs = si.sysex()
    kinds = {}
    for c, _ in msgs:
        kinds[c] = kinds.get(c, 0) + 1
    print("the Rytm's RT SysEx to the host by command: " + (", ".join(f"{c:#04x} x{n}" for c, n in sorted(kinds.items())) or "none"))
    trigs = [p for c, p in msgs if c == 0x04 and len(p) >= 1]
    for pad, (_, mach, voice, _, _) in TRACKS.items():
        mine = [p for p in trigs if p[0] & 7 == voice]
        flags = sorted({p[0] & 0x78 for p in mine})
        # >= 2 for every track: LT, 64 steps long in A01 (pattern_events), trigs a quarter as often as the others
        check(f"{pad}: the Rytm reports trigs on voice {voice} (machine {mach})", len(mine) >= 2,
              f"{len(mine)} trigs, machine flags {[hex(f) for f in flags]}")
    sd = [P.decode_trig_params(p) for p in trigs if p[0] & 7 == TRACKS["SD"][2]]
    p2 = sorted({d["params"][1] for d in sd if d})   # sim/protocol.py's decoder: P1..P16, P2 = TIM
    differ = {f"P{i + 1}": sorted({d["params"][i] for d in sd if d}) for i in range(16)
              if len({d["params"][i] for d in sd if d}) > 1}
    check("a p-lock on SD step 5 (P2 differs between its trigs)", len(p2) >= 2,
          f"P2 values {p2}; parameters that differ: {differ or 'none'}")
    ch = [P.decode_trig_params(p) for p in trigs if p[0] & 7 == TRACKS["CH"][2]]
    ch2 = [d["params"][1] for d in ch if d]
    check("a knob turn on CH reaches its trigs (P2, DAISYSP STR, changes once)", len(set(ch2)) >= 2 and ch2[-1] != ch2[0],
          f"P2 first {ch2[0] if ch2 else '?'} last {ch2[-1] if ch2 else '?'}, values {sorted(set(ch2))}")


def cmd_tracks(a):
    d, snap, card = run_copies()
    ev, t_play, knob_at, after, secs = tracks_events()
    wav, png = os.path.join(OUT, "phase1.wav"), os.path.join(OUT, "phase1-sd-src.png")
    if a.emu_only:
        p = emulator(secs, ev, png, wav, snap, card, "127.0.0.1:7301")
        si = StandIn("127.0.0.1:7301", secs + 2)
        check("track machines set by CC 15 (the DIN port found)", set_machines())
        err = p.communicate()[1]
        si.p.wait(timeout=30)
        si.join(timeout=5)
        with open(os.path.join(OUT, "phase1-standin.txt"), "w") as f:   # the host's view, for a FAIL's diagnosis
            f.write("\n".join(si.lines) + "\n")
        emu_checks(err, png)
        hello = [l for l in si.lines if l.startswith("hello answered")]
        check("the stand-in host got the hello answer", bool(hello) and "0x7" in hello[0], hello[0] if hello else "none")
        served = [l for l in si.lines if l.startswith("served ")]
        m = re.match(r"served (\d+) frames in ([0-9.]+) s", served[-1]) if served else None
        rate = int(m.group(1)) / float(m.group(2)) if m and float(m.group(2)) > 0 else 0
        check("the stand-in host served about 48000 frames/s", 40000 < rate < 52000, served[-1] if served else "no summary")
        reps = link_reports(err)
        check("the emulator's link reports (every 10 s)", len(reps) >= 2, f"{len(reps)} reports")
        check("the Rytm's USB MIDI reached the host (midi to host)", bool(reps) and reps[-1]["to"] > 0,
              f"{reps[-1]['to'] if reps else '?'} B")
        trig_checks_emu(si, t_play)
        print(f"screenshot: {png} - Read it: SD's SRC page with the 6-OP FM knob labels (PAT shows a number here: "
              "no Pi sends 17)")
        emu_link_summary(err)
        shutil.rmtree(d, ignore_errors=True)
        return
    pi = PiLog(os.path.join(OUT, "phase1-tracks-pi.log"))
    for c in ["notes=0", "out=tracks", "reportlog=1", "link=tcp"]:
        pi.cmd(c)
    p = emulator(secs, ev, png, wav, snap, card, "0.0.0.0:7301")
    check("track machines set by CC 15", set_machines())
    time.sleep(4)
    pi.cmd(f"pool={LT_SMP}:pi-test/sine440.wav")   # after the emulator's 10 <project>: the pool is that project's
    err = p.communicate()[1]
    pi.stop = True
    logs = [l for _, l in pi.lines]
    emu_checks(err, png)
    for pad, (_, _, voice, ch, word) in TRACKS.items():
        trig = [l for l in logs if re.search(rf"trig voice {voice} .*{re.escape(word)}", l)]
        check(f"{pad}: the Pi plays voice {voice} as {word}", len(trig) >= 2, f"{len(trig)} trigs")
        rms, _, _ = channel_rms(wav, ch, t_play, t_play + 6)
        check(f"{pad}: its channel sounds in the Rytm's output", rms > 1e-3, f"rms {rms:.4f}")
    # The CH channel's spectrum (third-octave bands) before and after the turn, against each window's own halves
    ch = TRACKS["CH"][3]
    b0 = [band_db(*channel_rms(wav, ch, t, t + 1.75)[1:]) for t in (knob_at - 4, knob_at - 2.25)]
    b1 = [band_db(*channel_rms(wav, ch, t, t + 1.75)[1:]) for t in (after, after + 1.75)]
    own = max(band_dist(*b0), band_dist(*b1))
    turn = band_dist(band_db(*channel_rms(wav, ch, knob_at - 4, knob_at - 0.5)[1:]),
                     band_db(*channel_rms(wav, ch, after, after + 3.5)[1:]))
    check("a knob turn changes the sound (CH, DAISYSP STR)", turn > 3.0 and turn > 3 * own,
          f"spectrum {turn:.1f} dB apart (each window's own halves {own:.1f} dB)")
    # cores.cpp ToCore with :reportlog=1: "report 04 voice V flags FF P1..P4 a b c d" (flags = voice byte & 0x78)
    sd = [l for l in logs if re.search(r"report 04 voice 2 flags [0-9a-f]{2} P1\.\.P4 \d+ \d+", l)]
    p2 = {re.search(r"P1\.\.P4 \d+ (\d+)", l).group(1) for l in sd}
    check("a p-lock reaches the engine (SD step 5, P2 differs)", len(p2) >= 2, f"P2 values {sorted(p2)}")
    # tcp_link.cpp LogStats: "... midi in N B (lost L) out M B dropped D ..."
    check("the engine's names and pool table reached the Rytm (midi out)",
          any(re.search(r"midi in \d+ B \(lost \d+\) out [1-9]\d* B", l) for l in logs))
    check("the pool slot was assigned", any("pool slot 1 = pi-test/sine440.wav: ok" in l for l in logs))
    fr = [int(m.group(1)) for l in logs for m in [re.search(r"frames (\d+)/s in", l)] if m]
    if fr:
        print(f"Pi tcp frames/s: last {fr[-1]}, min {min(fr[3:] or fr)} (want about 48000; record in docs/pi-engine.md)")
    print(f"screenshot: {png} - Read it: SD's SRC page, PAT shows a ROM voice name (e.g. 'BRASS   1'), not a number")
    emu_link_summary(err)
    shutil.rmtree(d, ignore_errors=True)


def cmd_gaps(a):
    d, snap, card = run_copies()
    ev, t_play = pattern_events(6.0)
    wav, png = os.path.join(OUT, "phase1-gaps.wav"), os.path.join(OUT, "phase1-gaps.png")
    secs = t_play + a.secs
    if a.emu_only:
        p = emulator(secs, ev, png, wav, snap, card, "127.0.0.1:7301")
        si = StandIn("127.0.0.1:7301", secs + 2)
        set_machines()
        err = p.communicate()[1]
        si.p.wait(timeout=30)
        pi = None
    else:
        pi = PiLog(os.path.join(OUT, "phase1-gaps-pi.log"))
        for c in ["notes=0", "out=tracks", "link=tcp"] + ([f"fifo={a.fifo}"] if a.fifo else []):
            pi.cmd(c)
        p = emulator(secs, ev, png, wav, snap, card, "0.0.0.0:7301")
        set_machines()
        err = p.communicate()[1]
        pi.stop = True
    emu_checks(err, png)
    shorts = [r["usb_short"] for r in link_reports(err) if r["usb_short"] is not None]
    warm = int((t_play + 20) // 10)   # the reports come every 10 s
    check("emulator: no OUT frames short after the warm-up", len(shorts) > warm and shorts[-1] == shorts[warm],
          f"{shorts[warm] if len(shorts) > warm else '?'} -> {shorts[-1] if shorts else '?'} over {len(shorts)} reports")
    if pi is not None:
        under = [int(m.group(1)) for _, l in pi.lines for m in [re.search(r"engine: fifo .* under (\d+)", l)] if m]
        check("Pi: no output underruns after the warm-up", len(under) > 40 and under[-1] == under[30],
              f"{under[30] if len(under) > 30 else '?'} -> {under[-1] if under else '?'}")
        check("Pi: no muted voice", not any("engine: muted" in l for _, l in pi.lines))
    emu_link_summary(err)
    shutil.rmtree(d, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--emu-only", action="store_true", help="no Pi: sim/emu_link.py as the host on 127.0.0.1")
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("base")
    b.add_argument("--if-needed", action="store_true", help="only when the base is missing or does not load")
    sub.add_parser("tracks")
    g = sub.add_parser("gaps")
    g.add_argument("secs", type=int)
    g.add_argument("--fifo", type=int, help="the Pi's FIFO target for this run (:fifo=N after :link=tcp)")
    a = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    if a.cmd == "base":
        if a.if_needed and base_loads():
            print(f"base: {BASE_SNAP} + {os.path.basename(BASE_CARD)} (loads)")
        else:
            make_base()
        return
    {"tracks": cmd_tracks, "gaps": cmd_gaps}[a.cmd](a)
    print(f"\n{'ALL PASS' if not fails else 'FAILED: ' + ', '.join(fails)}")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
