#!/usr/bin/env python3
"""End to end, emulator only: user machines between the emulated Rytm (rytm-mods sidecar-1017's sfx1017 build:
0021-ext slots=1, the user slots U1..U5 and their slot menu) and the real engine core on the Mac with its card-store
user machines (sim/um_link.py: libcore + core/user/um.cpp, the same code the Pi runs; the starter set as RMPA
packages, AArch64 code run in place). Nothing is flashed; no hardware.

    .venv/bin/python sim/um_e2e.py [explore|all]

Needs rytm-emu beside this checkout (RYTM_EMU=...) with fw/AR173_us_sfx1017.syx and sections/us_sfx1017.snap/.img/.json
(rytm-mods tools/emu/us_base.sh sfx1017), and the starter packages (make -C plugins/sc_* a64). Screens, logs and
WAVs go to the git-ignored sim/build/um-e2e/ (firmware screens stay out of git).

  menu     the track menu's U1 item (FUNC + CHRO), YES steps U1 through the installed machines: a screen per step,
           the 1E each YES sent and the core's READY answer
  play     U1 = the chosen machine (the slot menu), BD's machine set to U1 (the machine list), BD pressed: voice 0
           sounds (the WAV), the machine list shows the machine's name
  missing  the package deleted from the core's card during the run (as the web UI's delete) and rescanned: the
           Rytm asks again, the machine list shows !NAME, BD is silent
  presets  the play scenario with /PRESETS/<id>/ on the core's card: U1's 1A carries the presets flag, BD sounds
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, HERE)
import protocol as P  # noqa: E402

EMU = os.environ.get("RYTM_EMU", os.path.abspath(os.path.join(REPO, "..", "rytm-emu")))
TAG = os.environ.get("UM_TAG", "sfx1020")
FW = os.path.join(EMU, "fw", f"AR173_us_{TAG}.syx")
SNAP = os.path.join(EMU, "sections", f"us_{TAG}.snap")
CARD = os.path.join(EMU, "sections", f"us_{TAG}.img")
OUT = os.path.join(HERE, "build", "um-e2e")   # git-ignored
STARTER = ("sc_acid", "sc_wavepad", "sc_metal", "sc_snare", "sc_pluck", "sc_grain")
SR = 48000
STEP, SETTLE, MENU_HOLD = 100, 200, 300   # rytm-mods tools/emu/us_ui.json (measured on the us1 base)
fails = []


def check(what, ok, detail=""):
    print(f"{'PASS' if ok else 'FAIL'}: {what}{(' - ' + detail) if detail and not ok else ''}", flush=True)
    if not ok:
        fails.append(what)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def frame_of(clock_m, start_m):
    """The link's frames at emulator clock clock_m (M instructions): 200M instructions an emulated second."""
    return int((clock_m - start_m) * 1e6 / 200e6 * SR)


class Run:
    """One scenario: the core host (um_link.py) and dtboot on the base with key events at M instructions."""

    def __init__(self, name, length, events=(), removes=(), packages=STARTER, tag=None, load=None, save=None, prep=None,
                 at0=None):
        """tag: the build (default UM_TAG); load: a snapshot of that build to start from (default its base) and at0 its
        clock (M instructions); save: the snapshot at the end; prep(card): the core's card before it starts."""
        os.makedirs(OUT, exist_ok=True)
        self.name = name
        tag = tag or TAG
        tmp = tempfile.mkdtemp(prefix="ume2e-")
        card = os.path.join(tmp, "card")
        os.makedirs(os.path.join(card, "Samples"))
        os.makedirs(os.path.join(card, "Plugins"))
        for p in packages:
            shutil.copy(os.path.join(REPO, "plugins", p, f"{p}.a64.rmp"), os.path.join(card, "Plugins", f"{p}.rmp"))
        if prep:
            prep(card)
        port = free_port()
        self.clock = at0 if at0 is not None else json.load(open(os.path.join(EMU, "sections", f"us_{tag}.json")))["clock"]
        self.log_path = os.path.join(OUT, f"{name}.jsonl")
        self.wav = os.path.join(OUT, f"{name}.wav")
        hc = [sys.executable, os.path.join(HERE, "um_link.py"), "--card", card, "--link", f"127.0.0.1:{port}",
              "--log", self.log_path, "--wav", self.wav, "--secs", "900"]
        for at, path in removes:
            hc += ["--remove-at", f"{frame_of(self.clock + at, self.clock)}:{path}"]
        host = subprocess.Popen(hc, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        fw = os.path.join(EMU, "fw", f"AR173_us_{tag}.syx")
        cmd = [os.path.join(EMU, "target", "release", "dtboot"), fw, "--load", load or os.path.join(EMU, "sections", f"us_{tag}.snap"),
               "--card", os.path.join(EMU, "sections", f"us_{tag}.img"),
               "--instr", f"{length}M", "--every", "5M", "--usb-host", "--usb-midi-real", "--ob-link", f"127.0.0.1:{port}"]
        if save:
            cmd += ["--save", save]
        for at, kind, val in events:
            k, _, hold = val.partition(":")
            cmd += ["--press", f"{k}@{self.clock + at}M" + (f":{hold}" if hold else "")]
        env = dict(os.environ, DTBOOT_END_PNG=os.path.join(OUT, f"{name}.png"))
        r = subprocess.run(cmd, cwd=EMU, env=env, capture_output=True, text=True)
        self.emu = r.stdout + r.stderr
        open(os.path.join(OUT, f"{name}.emu.log"), "w").write(self.emu)
        try:
            host.wait(timeout=30)
        except subprocess.TimeoutExpired:
            host.kill()
        self.hout = host.stdout.read()
        open(os.path.join(OUT, f"{name}.host.log"), "w").write(self.hout)
        check(f"{name}: emulator and core host ran (no fault)", r.returncode == 0 and "FAULT" not in self.emu
              and "Traceback" not in self.hout, (self.emu[-300:] if r.returncode else "") + self.hout[-300:])
        with open(self.log_path) as f:
            self.log = [json.loads(x) for x in f]
        shutil.rmtree(tmp, True)

    def rx(self, cmd):
        return [(e["f"], bytes.fromhex(e["rx"][1])) for e in self.log if "rx" in e and e["rx"][0] == cmd]

    def tx(self, cmd):
        return [(e["f"], bytes.fromhex(e["tx"][1])) for e in self.log if "tx" in e and e["tx"][0] == cmd]

    def um(self):
        u = [e["um"] for e in self.log if "um" in e]
        return u[-1] if u else []

    def voices(self):
        with wave.open(self.wav) as w:
            return np.frombuffer(w.readframes(w.getnframes()), "<i2").reshape(-1, 8).astype(float) / 32767

    def u1_trigs(self):
        """The link frames of the Rytm's 0x04 trigs for voice 0 on U1 (flags 0x50)."""
        return [f for f, m in self.rx(P.TRIG_PARAMS) if m[0] == 0x50]

    def hostlines(self, s):
        return [x for x in self.hout.splitlines() if s in x]


def open_menu(at, downs=4):
    """The track menu (FUNC held + CHRO), DOWN to its U1 item."""
    ev = [(at, "press", f"FUNC:{MENU_HOLD}M"), (at + 20, "press", "CHRO")]
    t = at + MENU_HOLD + 100
    for _ in range(downs):
        ev.append((t, "press", "DOWN"))
        t += STEP
    return ev, t


def walk_list(at, downs, confirm=True):
    """The machine list of the selected track (SRC SRC), DOWN downs times, YES."""
    ev, t = [(at, "press", "SRC"), (at + STEP, "press", "SRC")], at + 2 * STEP
    for _ in range(downs):
        ev.append((t, "press", "DOWN"))
        t += STEP
    if confirm:
        ev.append((t, "press", "YES"))
        t += STEP
    return ev, t + SETTLE


def t_explore():
    """Screens for the key walks on this base (not in all)."""
    ev, t = open_menu(200, 0)
    Run("explore-menu0", t + 100, ev)
    ev, t = open_menu(200)
    Run("explore-menu4", t + 100, ev)
    ev, t = walk_list(200, 15, confirm=False)
    Run("explore-list15", t, ev)


def t_menu():
    """YES on the track menu's U1 steps U1 through the installed machines (sorted as the core's list)."""
    names = []
    ev, t = [], 300
    for k in range(3):
        # the menu, YES (the next installed machine), the answer's popup, the menu closed (0021 takes the slot's new
        # 1A in its UI tick, which runs with the menu closed), the menu again: U1 shows the machine (the PNG)
        ev2, t = open_menu(t, 4 if k == 0 else 0)   # (the menu reopens on its last item)
        ev += ev2 + [(t, "press", "YES"), (t + 300, "press", "NO"), (t + 400, "press", "NO")]
        ev2, t2 = open_menu(t + 700, 0)
        r = Run(f"menu-yes{k + 1}", t2 + 100, ev + ev2)
        t = t2 + 200
        ev += ev2 + [(t - 50, "press", "NO"), (t, "press", "NO")]
        t += 200
        a = [P.decode_assign(m) for _, m in r.rx(P.ASSIGN)]
        ans = [P.decode_slots(m)["answer"] for _, m in r.tx(P.SLOTS)]
        ans = [x for x in ans if x]
        u1 = [x for x in r.um() if x.get("t") == "slot" and x["slot"] == 1]
        names.append(u1[0]["name"] if u1 else None)
        check(f"menu: YES {k + 1}: the Rytm sent 1E (U1, entry {a[-1]['arg'] if a else '?'}) and the core answered READY, "
              f"U1 = {names[-1]} (PNG menu-yes{k + 1}.png: the menu's U1 item)",
              bool(a) and a[-1]["slot"] == 1 and bool(ans) and ans[-1] == (1, P.RES_READY) and bool(u1) and u1[0]["state"] == "READY",
              f"1E {a[-3:]}, answers {ans[-3:]}, U1 {u1}")
    check("menu: each YES chose another starter machine", len(set(names)) == 3, str(names))
    inst = [m for _, m in r.tx(P.LIST) if m[2] == P.LIST_INSTALLED]
    got = [n for m in inst for (_, n) in P.decode_list(m)["entries"]]
    check("menu: the Rytm asked for and got the installed list (the six starter machines)", len(set(got)) == 6, str(got))
    return ev, t


def t_play():
    """U1 = the first starter machine (one YES), BD on U1 (machine list), BD pressed: voice 0 sounds."""
    ev, t = open_menu(300)
    ev += [(t, "press", "YES"), (t + 300, "press", "NO"), (t + 400, "press", "NO")]
    ev2, t = walk_list(t + 600, 15)
    ev += ev2
    ev2, t2 = walk_list(t + 100, 0, confirm=False)   # the list again (it opens on BD's machine, U1): its name
    r0 = Run("play-list", t2 + 100, ev + ev2)
    ev += [(t + 200, "press", "BD"), (t + 600, "press", "BD")]
    r = Run("play", t + 1000, ev)
    v = r.voices()
    tf = r.u1_trigs()
    trig_f = tf[0] if tf else 0
    seg = v[trig_f:trig_f + SR // 4, 0]
    pre = v[:trig_f, 0]
    u1 = [x for x in r.um() if x.get("t") == "slot" and x["slot"] == 1]
    check(f"play: BD on U1 ({u1[0]['name'] if u1 else '?'}) sounds from the pad's trig ({len(tf)} trigs; voice 0 peak "
          f"{np.abs(seg).max():.3f} in the 0.25 s after the first, {np.abs(pre).max():.3f} before it; WAV play.wav, "
          f"PNG play-list.png)", bool(tf) and np.abs(seg).max() > 0.05 and np.abs(pre).max() == 0 and np.isfinite(v).all())
    reports = [m for _, m in r.rx(P.TRIG_PARAMS) if (m[0] & 0x78) == 0x50]   # payloads (after the command)
    check("play: the Rytm sent 0x04 trigs for voice 0 on U1 (flags 0x50) with the slot's fingerprint as tag",
          bool(reports) and (len(reports[-1]) != 37 or (reports[-1][35] << 7 | reports[-1][36]) in (0, u1[0]["fp"] if u1 else -1)),
          f"{len(reports)} trigs, last {reports[-1].hex() if reports else None}")
    return ev, t


def t_missing():
    """The play scenario, then U1's package deleted from the core's card and rescanned: MISSING, !NAME, silent."""
    ev, t = open_menu(300)
    ev += [(t, "press", "YES"), (t + 300, "press", "NO"), (t + 400, "press", "NO")]
    ev2, t = walk_list(t + 600, 15)
    ev += ev2
    first = sorted(STARTER)[0]
    rm_at = t + 300
    ev += [(t + 200, "press", "BD")]
    ev2, t3 = walk_list(rm_at + 600, 0, confirm=False)   # opens on U1
    ev += ev2 + [(t3 + 50, "press", "NO"), (t3 + 200, "press", "BD")]
    # which package U1 got: the core's list order is the card's; the first YES assigns entry 0 or the next one, so the
    # package is found from the log of a dry run (the same events: deterministic)
    dry = Run("missing-dry", t + 250, ev[:len(ev) - len(ev2) - 2])
    u1 = [x for x in dry.um() if x.get("t") == "slot" and x["slot"] == 1]
    pid = u1[0]["id"] if u1 else first
    pkg = {"sc.acid": "sc_acid", "sc.wavepad": "sc_wavepad", "sc.metal": "sc_metal", "sc.snare": "sc_snare",
           "sc.pluck": "sc_pluck", "sc.grain": "sc_grain"}.get(pid, first)
    r = Run("missing-list", t3, ev[:len(ev) - 2], removes=[(rm_at, f"/Plugins/{pkg}.rmp")])
    u1 = [x for x in r.um() if x.get("t") == "slot" and x["slot"] == 1]
    asks = [P.decode_ask(m) for f, m in r.rx(P.ASK) if f > frame_of(r.clock + rm_at, r.clock)]
    check(f"missing: {pkg}.rmp deleted and rescanned - U1 MISSING in the core, the Rytm asked U1's 1A again "
          f"(PNG missing-list.png: the machine list shows !NAME)",
          bool(u1) and u1[0]["state"] == "MISSING" and any(a and a["slot"] == 1 and a["what"] == 0 for a in asks),
          f"U1 {u1}, asks after {asks[-4:]}")
    r = Run("missing", t3 + 600, ev, removes=[(rm_at, f"/Plugins/{pkg}.rmp")])
    v = r.voices()
    rm_f = [e["f"] for e in r.log if "removed" in e]
    tf = r.u1_trigs()
    before = [f for f in tf if rm_f and f < rm_f[0]]
    after = [f for f in tf if rm_f and f > rm_f[0]]
    pk = lambda f: float(np.abs(v[f:f + SR // 4, 0]).max())
    check(f"missing: BD sounded on its trig before the delete ({[round(pk(f), 3) for f in before]}) and is silent on the "
          f"trig after it ({[round(pk(f), 3) for f in after]})",
          bool(before) and bool(after) and pk(before[0]) > 0.05 and max(pk(f) for f in after) == 0.0)


def shared(tag, new):
    """A project shared from another Sidecar (2026-10-19, slot-resolve; core/user/um.cpp, rmp_resolve.h).
    Setup: U1 = a starter machine (the slot menu), BD on U1 (the machine list: the sound tagged with its fingerprint),
    the Rytm saved as a snapshot. Then a Sidecar that has never seen this project: its card's last project is PREV,
    whose U1 is another machine (what a fresh project would inherit). The Rytm's 10 switches it to the project.
      new (0021 sends 0x25): U1 resolves from the project's sounds before the first trig (not inherited); BD's first
      trig sounds.
      old (no 0x25): U1 inherits PREV's machine at once; BD's first trig (its tag names the other machine) is silent
      and resolves U1 lazily; the next trig sounds."""
    ev, t = open_menu(300)
    ev += [(t, "press", "YES"), (t + 300, "press", "NO"), (t + 400, "press", "NO")]
    ev2, t = walk_list(t + 600, 15)
    ev += ev2
    snap = os.path.join(OUT, f"shared-{tag}.snap")   # git-ignored (firmware state)
    r0 = Run(f"shared-{tag}-setup", t + 200, ev, tag=tag, save=snap)
    u1 = [x for x in r0.um() if x.get("t") == "slot" and x["slot"] == 1]
    pid = u1[0]["id"] if u1 else "?"
    proj = [m.decode("ascii", "replace") for _, m in r0.rx(0x10)]
    check(f"shared {tag}: setup - U1 = {pid}, BD on U1, the Rytm's project {proj[-1] if proj else '?'!r}",
          bool(u1) and u1[0]["state"] == "READY" and bool(proj))
    other = "sc.grain" if pid != "sc.grain" else "sc.metal"

    def prep(card):   # another Sidecar: last project PREV (U1 = other), nothing about this project
        os.makedirs(os.path.join(card, ".rytm", "slots"))
        open(os.path.join(card, ".rytm", "project"), "w").write("PREV")
        open(os.path.join(card, ".rytm", "slots", "PREV.txt"), "w").write(f"U1 {other} 1.0.0\n")
    at0 = r0.clock + t + 200
    # BD at +1500M (7.5 s: the 10 + 25 resend comes every ~5 s) and at +2500M
    # (an older build sends its 10 only every ~11 s of ext voice blocks: three trigs, the last after it)
    presses = (3000, 4000, 5000) if not new else (1500, 2500)
    ev = [(p, "press", "BD") for p in presses]
    r = Run(f"shared-{tag}", presses[-1] + 600, ev, tag=tag, load=snap, prep=prep, at0=at0)
    sl = [(e["f"], [x for x in e["um"] if x.get("t") == "slot" and x["slot"] == 1]) for e in r.log if "um" in e]
    prints = r.rx(P.SLOT_PRINTS)
    tf = r.u1_trigs()
    v = r.voices()
    pk = lambda f: float(np.abs(v[f:f + SR // 4, 0]).max())
    last = sl[-1][1][0] if sl and sl[-1][1] else {}
    if new:
        ent = P.decode_slot_prints(prints[0][1]) if prints else None
        ready = [f for f, x in sl if x and x[0]["id"] == pid and x[0]["state"] == "READY"]
        inh = [f for f, x in sl if x and x[0]["id"] == other and x[0].get("src") == "inherited"]
        check(f"shared {tag}: the Rytm sent 10 and 0x25 ({ent}) and U1 resolved to {pid} from the project's sounds "
              f"before the first trig (READY at frame {ready[:1]}, first trig {tf[:1]}), never inheriting {other}",
              bool(ent) and any(e[0] == 1 and e[1] == P.plugin_fp(pid) for e in ent["entries"]) and bool(ready) and bool(tf)
              and ready[0] < tf[0] and not inh and last.get("src") == "sounds" and not last.get("updated"),
              f"U1 {last}, prints {prints[:1]}, inherited at {inh[:2]}")
        check(f"shared {tag}: BD's first trig sounds (peak {pk(tf[0]) if tf else 0:.3f})", bool(tf) and pk(tf[0]) > 0.05)
    else:
        inh = [f for f, x in sl if x and x[0]["id"] == other and x[0].get("src") == "inherited"]
        check(f"shared {tag}: no 0x25 from this build; U1 first inherited {other} from PREV", not prints and bool(inh),
              f"prints {len(prints)}, U1 {sl[:3]}")
        check(f"shared {tag}: BD's trig (tag of {pid}) resolved U1 lazily to {pid} (src {last.get('src')}, updated "
              f"{last.get('updated')}); the first trig is silent ({pk(tf[0]) if tf else -1:.3f}), the next sounds "
              f"({pk(tf[-1]) if tf else -1:.3f})",
              last.get("id") == pid and last.get("src") == "sounds" and len(tf) >= 3 and pk(tf[0]) == 0.0 and pk(tf[-1]) > 0.05,
              f"U1 {last}, trigs {tf}")


def t_shared():
    shared(os.environ.get("UM_TAG_NEW", "sfx1020"), True)


def t_lazy():
    shared(os.environ.get("UM_TAG_OLD", "sfx1018"), False)


def t_srcsteps():
    """The user's key sequence (hardware report 2026-10-17): U1 = ACID from the track menu, BD on U1 (SRC SRC, YES), then on
    BD's SRC page the machine list again (SRC SRC), DOWN over U2, U3 and U4 with a few seconds on each (the 25 SLOT
    PRINTS go out about once a second) and YES on U4. The arrows only move the cursor and YES chooses U4: the Rytm
    sent no 1E but the one for U1, no print named a slot but U1, and the core still has U2..U5 EMPTY (before the fix a
    sound tagged ACID on an empty U3 / U4 made the core fill them with ACID, one slot a step)."""
    ev, t = open_menu(300)
    ev += [(t, "press", "YES"), (t + 300, "press", "NO"), (t + 400, "press", "NO")]
    ev2, t = walk_list(t + 600, 15)   # BD on U1
    ev += ev2
    dwell = 1500   # M instructions: 7.5 emulated seconds
    ev += [(t + 100, "press", "SRC"), (t + 200, "press", "SRC")]
    t += 300 + STEP
    for k in range(3):   # U2, U3, U4: each previewed on BD for a while
        ev.append((t, "press", "DOWN"))
        t += dwell
    ev.append((t, "press", "YES"))
    t += dwell
    r = Run("srcsteps", t, ev)
    a = [P.decode_assign(m) for _, m in r.rx(P.ASSIGN)]
    check("srcsteps: the Rytm sent one 1E (U1, from the menu's YES), none for the arrows or the list's YES",
          len(a) == 1 and a[0]["slot"] == 1, str(a))
    pr = [P.decode_slot_prints(m) for _, m in r.rx(P.SLOT_PRINTS)]
    slots = sorted({e[0] for p in pr if p for e in p["entries"]})
    check("srcsteps: no 25 SLOT PRINTS named a slot but U1 (no preview, no stale tag on the chosen U4)",
          bool(pr) and slots in ([1], []), f"slots {slots} in {len(pr)} prints")
    st = [(x["slot"], x["state"]) for x in r.um() if x.get("t") == "slot"]
    check("srcsteps: the core holds U1 READY and U2..U5 EMPTY", st == [(1, "READY")] + [(k, "EMPTY") for k in (2, 3, 4, 5)],
          str(st))


def t_presets():
    """The play scenario with a preset folder for every starter machine on the core's card (/PRESETS/<id>/: a generic
    .txt and a macOS ._ file): the core scans U1's, its 1A to the Rytm carries the presets flag, the JSON counts one
    preset (the ._ file left out), and BD on U1 still sounds (the scan and the 1A's new generation in the way)."""
    ids = ("sc.acid", "sc.wavepad", "sc.metal", "sc.snare", "sc.pluck", "sc.grain")

    def prep(card):
        for i in ids:
            d = os.path.join(card, "PRESETS", i)
            os.makedirs(d)
            with open(os.path.join(d, "Init.txt"), "w") as f:
                f.write("P2 64\n")
            with open(os.path.join(d, "._Init.txt"), "w") as f:
                f.write("junk")
    ev, t = open_menu(300)
    ev += [(t, "press", "YES"), (t + 300, "press", "NO"), (t + 400, "press", "NO")]
    ev2, t = walk_list(t + 600, 15)
    ev += ev2 + [(t + 200, "press", "BD"), (t + 600, "press", "BD")]
    r = Run("presets", t + 1000, ev, prep=prep)
    u1 = [x for x in r.um() if x.get("t") == "slot" and x["slot"] == 1]
    heads = [P.decode_meta(m) for _, m in r.tx(P.META)]
    heads = [h for h in heads if h and h["slot"] == 1 and h["part"] == 0]
    check(f"presets: the core scanned U1's folder ({u1[0]['id'] if u1 else '?'}: {u1[0].get('presets') if u1 else '?'} preset, "
          f"the ._ file left out)", bool(u1) and u1[0]["state"] == "READY" and u1[0].get("presets") == 1 and u1[0].get("pre_ok"),
          str(u1))
    check("presets: U1's 1A to the Rytm carries the presets flag (4)", bool(heads) and heads[-1]["flags"] & P.FLAG_PRESETS,
          str(heads[-2:]))
    v = r.voices()
    tf = r.u1_trigs()
    seg = v[tf[0]:tf[0] + SR // 4, 0] if tf else np.zeros(1)
    check(f"presets: BD on U1 sounds ({len(tf)} trigs, peak {np.abs(seg).max():.3f}; WAV presets.wav)",
          bool(tf) and np.abs(seg).max() > 0.05)


TESTS = {"srcsteps": t_srcsteps, "presets": t_presets, "menu": t_menu, "play": t_play, "missing": t_missing, "shared": t_shared, "lazy": t_lazy}


def main():
    for p in STARTER:
        if not os.path.exists(os.path.join(REPO, "plugins", p, f"{p}.a64.rmp")):
            sys.exit(f"no plugins/{p}/{p}.a64.rmp: make -C plugins/{p} a64")
    for f in (FW, SNAP, CARD):
        if not os.path.exists(f):
            sys.exit(f"no {f}: make the base (rytm-mods tools/emu/us_base.sh {TAG})")
    which = sys.argv[1:] or ["all"]
    if which == ["explore"]:
        t_explore()
        return
    for name in (TESTS if which == ["all"] else which):
        TESTS[name]()
    print(f"{'ALL PASS' if not fails else f'{len(fails)} FAILED'} (screens and WAVs: {OUT})")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
