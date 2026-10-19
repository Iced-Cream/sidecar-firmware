#!/usr/bin/env python3
"""The remote UI end to end in rytm-emu: the emulated Rytm (rytm-mods 0026-remote-ui, build rui1022) against the real
engine core (sim/rui_link.py: core/build/libcore.dylib's remote UI host, core/ui), over the Overbridge link.

    python3 sim/rui_e2e.py [TEST ...]       TESTS: connect sidecar plaits status leave gone keys splash scope wave rec play gfx extmach machines sdvoice srcgfx srcwave noexit lfo srcview latency (default all but latency and srcview)

Every run: rytm-emu's dtboot with the rui1023 firmware from its base snapshot (rytm-mods tools/emu/us_base.sh rui1023),
DTEMU_STRICT=1, --usb-host --usb-midi-real --ob-link, keys and turns by --press / --turn; times are ms after the
snapshot (200 M instructions = 1 emulated second). The checks:
  connect  2026-10-19 (a freeze on hardware, PLAY while the Sidecar connected): BD on JUNO, the sequencer playing and
           pads hammered while the USB host re-attaches RUI_CONNECTS (8) times (rytm-emu --usb-host-gone), PLAY / STOP
           at points through each enumeration: the UI's 20 renders and the audio interrupt's 1500 blocks a second
           throughout, no fault, no USB MIDI wait from interrupt level, a hello each attach, the core silenced at it.
  sidecar  TRK + YES opens the page (30 on); the core sends the whole SIDECAR frame; the panel's own pixels (the OLED
           the panel MCU holds) equal it byte for byte.
  plaits   RIGHT twice (SIDECAR, SYSTEM) goes to the first PLAITS voice page; encoder B (HRM) turned +1 x 6 changes that voice's P2 in the
           engine by +6; the knob's frames carry only changed tiles (a few), the panel sends exactly those; the OLED
           equals the core's last frame.
  leave    TRK + YES again leaves (30 off, reason escape): the stock screen returns, equal to a run that never opened.
  gone     the core stops mid-page: the Rytm leaves by its 1 s timeout, the UI keeps rendering 20 times a second and the
           stock screen returns (no hang).
  status   the two status pages: SIDECAR (the tracks' machines, FX1 / FX2 with PRE / POST) first, RIGHT to SYSTEM (U1..U5,
           the recorder, Overbridge, each core, the network: Ethernet up and its address), drawn as text by the Rytm
           (0026 1021: 3D), never the Wi-Fi's name; again with long names, recording and Wi-Fi (the worst case).
  keys     1022, the catch mask (37): on the PLAITS page PLAY / STOP start and stop the sequencer (the Rytm's MIDI FA / FC)
           while the page stays and shows the core's frame, a pad plays (the Rytm's note-on) and never reaches the core,
           HRM (caught) still edits the voice, LEVEL (passed) never reaches the core; LEFT pressed on PLAITS changes the
           page and so the mask, its release still reaches the core; UP held across leaving: its release eaten; 0026's
           key books empty and the stock screen as a Rytm that never opened the page.
  splash   1023, the connect splash: the base Rytm has been up ~30 s, the run's USB host enumerates it, 0026 says hello
           (35) and the core answers with its splash (the frame, 36 64, the icon sliding in, the line running down, 36 0
           at 3 s): the panel shows the core's frames while PLAY runs the sequencer and a pad plays (nothing reaches the
           core, the page stays closed); after it the stock screen equals the same presses without a splash; NO ends it
           early (eaten); TRK + YES during it opens the SIDECAR page; the core gone mid-splash: stock ~1 s later.
           RUI_SHOTS: the splash's frames on the panel (splash_<ms>.png).
  scope    LEFT on SIDECAR opens SCOPE (the last page) with sim/rui_live.py's synthetic IN stream in the tap (RUI_TAP):
           its mask (encoders A..C), the frame rate (<= 15 / s), tiles and bytes a second within the budget, the panel
           equals the frame, a TIME turn reaches the core and changes the bottom bar within a frame, the Rytm's UI
           keeps its 20 renders a second while the scope streams.
  wave     voice 1 on DSP SAMPLE (RUI_VOICE1; the core's card with a drum break as pool slot 1, the sample RAM): RIGHT
           twice opens its page, the overview loads (LOADING, then the mode), STA turned moves the region's marker in
           frames of few tiles, MOD turned into GRAIN's zone turns the page into GRAIN (POS SIZE DENS), the panel equals
           the frame.
  rec      the REC page with a model of the Pi's recorder (RUI_REC): LEFT twice from SIDECAR (SCOPE, then REC), its mask
           (encoders A..D), YES records (the take's time in large digits, RECORDING), YES again stops, A turned right arms;
           the page stays (YES is the page's), the panel equals the frame.
  play     playback on the REC page against the model's synthetic takes (RUI_REC): B turned picks the channel, B
           pushed plays (PLAYING, the take, BD 2/3, the progress track, the level, the length), D the level; C the
           older take (stops), YES records: playback stops and B / B turned do nothing while the take records.
  gfx      voice 1 on 6-OP FM (RUI_VOICE1): its page with the algorithm diagram; PAT turned picks other ROM voices, the
           diagram's tiles change; the panel equals the frame.
  extmach  the machine chosen on the Rytm (SRC SRC, the machine list to JUNO, then to 6-OP FM, YES: 0021's EXT
           machines on track BD), no voice reported at the start (RUI_FRESH): no report until the track sounds, no
           voice page and no PLAITS guessed; the BD pad (passed while the page shows) trigs it: SIDECAR names it, RIGHT
           twice opens its page; the panel equals the frame.
  machines 1025 (33, rui1025's base, RUI_TAG_1025): JUNO picked in BD's SRC list without a trig: the Rytm's 33s (the
           snapshots, BD (41, 08)), no voice report, TRK + YES and RIGHT twice open voice 1's JUNO page; the list again
           up to an analog machine: SIDECAR names none for track 1, no voice page left.
  lfo      the LFO page (core/lfo.h; RUI_LFO sets L1 on BD's FREQ, L2 a 3 Hz sine on SD's RES, L3 on voice 1's HRM):
           LEFT twice from SIDECAR, its mask (all eight encoders), D turned sets the depth, YES the design view (A..D
           and T1..T16 caught), T5 and B edit step 5, NO back; the CCs to the Rytm (CC 74 on channel 1 only on a change,
           at most 100 a second in all); the panel equals the frame; then the stock FLTR page with and without the
           LFOs: the emulated Rytm took the CC (the panels differ).
  latency  a turn on the PLAITS page, the 1020 host (RUI_LEGACY) against this one: the Rytm's own clocks (DTCN0) for
           turn -> report, turn -> the local echo on the panel, turn -> the host's frame on the panel.
1021: the text is the Rytm's (3D, drawn by its own text routine in its own font); the core's frame carries the pixels
and the text items. "The panel equals the frame": every pixel outside the text items' boxes equal, and every item's
box inked by the Rytm (rows y .. y + 6, the nominal width + a margin: the Rytm's glyphs are its own).
Output (git-ignored): sim/build/rui_e2e/<test>.{png,dt.txt,jsonl,link.txt} and the frames the core sent (frames_<test>/).
RYTM_EMU: the rytm-emu checkout (default ../rytm-emu (a sibling checkout)); RUI_TAG (default rui1022);
RYTM_MODS: the rytm-mods checkout whose build/emu/TAG/manifest.json names 0026's symbols (latency); RUI_SHOTS: copy
the screens there as PNG.
"""
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from rui_render import frame_png  # noqa: E402

EMU = os.environ.get("RYTM_EMU", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "rytm-emu"))
TAG = os.environ.get("RUI_TAG", "rui1023")
MODS = os.environ.get("RYTM_MODS", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "sidecar-mods"))   # its build/emu/TAG/manifest.json
SHOTS = os.environ.get("RUI_SHOTS")   # a directory for the screens as PNG (outside git: firmware-derived pixels)
OUT = os.path.join(HERE, "build", "rui_e2e")
TILE_FN, RENDER = 0x40095924, 0x400a2aaa   # 1.73: the panel tile writer, the UI render (20 Hz) [rytm-mods 0026 design.md]
FAILS = []
ENTER = [(300, "press", "TRK", 300), (400, "press", "YES")]


def check(name, ok, detail=""):
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   [{detail}]" if detail else ""), flush=True)
    if not ok:
        FAILS.append(name)


def oled(out):
    """dtboot's last panel screen (DTBOOT_OLED: the panel MCU's own pixels), half blocks -> the 1024 B framebuffer."""
    i = out.rfind("panel MCU OLED")
    if i < 0:
        return None
    fb = bytearray(1024)
    for ty, line in enumerate(out[i:].splitlines()[1:33]):
        for x, ch in enumerate(line[:128]):
            for dy, on in enumerate({"█": (1, 1), "▀": (1, 0), "▄": (0, 1)}.get(ch, (0, 0))):
                if on:
                    y = 2 * ty + dy
                    fb[(7 - y // 8) + 8 * x] |= 1 << (y % 8)
    return bytes(fb)


def status(out):
    """The status lines: [(clock M, {counter: n})]."""
    r = []
    for m in re.finditer(r"^\s*(\d+)M pc=\S+ .*?$", out, re.M):
        cs = dict((int(a, 16), int(n)) for a, n in re.findall(r"([0-9a-f]{8}):(\d+)", m.group(0)))
        r.append((int(m.group(1)), cs))
    return r


class Emu:
    def __init__(self, tag=None):
        tag = tag or TAG
        self.fw = f"fw/AR173_us_{tag}.syx"
        self.card = f"sections/us_{tag}.img"
        self.snap = f"sections/us_{tag}.snap"
        self.clock = json.load(open(os.path.join(EMU, f"sections/us_{tag}.json")))["clock"]
        os.makedirs(OUT, exist_ok=True)
        self.core_card = tempfile.mkdtemp(prefix="rui_e2e_card_")

    def at(self, ms):
        return f"{self.clock + ms / 5:g}M"

    def run(self, name, ms, events=(), link=True, stop_at=None, save=False, extra=()):
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        s.close()
        cmd = [os.path.join(EMU, "target/release/dtboot"), self.fw, "--load", self.snap, "--card", self.card,
               "--instr", f"{ms // 5}M", "--every", "20M", "--usb-host", "--usb-midi-real",
               "--ob-link", f"127.0.0.1:{port}", "--count", f"{TILE_FN:x}", "--count", f"{RENDER:x}"]
        cmd += list(extra)
        snap = os.path.join(OUT, f"{name}.snap")
        if save:
            cmd += ["--save", snap]
        for e in events:
            if e[1] == "press":
                cmd += ["--press", f"{e[2]}@{self.at(e[0])}" + (f":{e[3] // 5}M" if len(e) > 3 else "")]
            elif e[1] == "turn":
                cmd += ["--turn", f"{e[2]}@{self.at(e[0])}x{e[3]}/{e[4] // 5}M"]
        log = os.path.join(OUT, f"{name}.jsonl")
        frames = os.path.join(OUT, f"frames_{name}")
        sp = None
        if link:
            for p in (log,):
                if os.path.exists(p):
                    os.remove(p)
            args = [sys.executable, os.path.join(HERE, "rui_link.py"), "--link", f"127.0.0.1:{port}", "--card",
                    self.core_card, "--log", log, "--frames", frames]
            if stop_at is not None:
                args += ["--stop-at", str(stop_at)]
            lt = os.path.join(OUT, f"{name}.link.txt")
            sp = subprocess.Popen(args, stdout=open(lt, "w"), stderr=subprocess.STDOUT)
            t0 = time.time()
            while "ready" not in open(lt).read():   # the core up before the emulator runs (it runs faster than real time)
                if time.time() - t0 > 30 or sp.poll() is not None:
                    sys.exit(f"rui_link did not start: {open(lt).read()}")
                time.sleep(0.01)
        r = subprocess.run(cmd, cwd=EMU, env=dict(os.environ, DTEMU_STRICT="1", DTBOOT_OLED="1"), capture_output=True,
                           text=True, timeout=1800)
        if sp:
            try:
                sp.wait(timeout=30)
            except subprocess.TimeoutExpired:
                sp.kill()
        out = r.stdout + r.stderr
        open(os.path.join(OUT, f"{name}.dt.txt"), "w").write(out)
        res = dict(out=out, oled=oled(out), status=status(out), events=[], rc=r.returncode, snap=snap)
        if link and os.path.exists(log):
            res["events"] = [json.loads(x) for x in open(log)]
        if res["oled"]:
            frame_png(os.path.join(OUT, f"{name}.png"), res["oled"])
        fault = "FAULT" in out or "strict:" in out or r.returncode != 0
        check(f"{name}: dtboot ran clean (strict, no fault)", not fault,
              "" if not fault else (re.findall(r".*(?:FAULT|strict:).*", out)[:3] or [f"rc {r.returncode}"]))
        return res


def ev(res, **kw):
    return [e for e in res["events"] if all(e.get(k) == v for k, v in kw.items())]


def last_frame(name, res):
    c = ev(res, tx="commit")
    if not c:
        return None
    return open(os.path.join(OUT, f"frames_{name}", f"frame_{c[-1]['frame']}.bin"), "rb").read()


def diff_bytes(a, b):
    return sum(x != y for x, y in zip(a or b"", b or b"")) if a and b else 1024


def last_texts(name, res):
    c = ev(res, tx="commit")
    if not c:
        return []
    p = os.path.join(OUT, f"frames_{name}", f"texts_{c[-1]['frame']}.json")
    return json.load(open(p)) if os.path.exists(p) else []


def px(fb, x, y):
    return fb[(7 - y // 8) + 8 * x] >> (y % 8) & 1


def text_boxes(items):
    """Each item's box with a margin: the widest advance (5 px, core/ui/ui.h kRSmall) for the width, rows y - 1 .. y + 6."""
    out = []
    for x, y, font, flags, t in items:
        w = (7 if font == 7 else 5) * len(t)   # 1023: the bold font (7) 7 px a character, 10 rows
        a = flags & 3
        x0 = x - w // 2 if a == 1 else x - w if a == 2 else x
        out.append((x0 - 2, y - 1, x0 + w + 2, y + (12 if font == 7 else 7), t))
    return out


def panel_matches(oled, frame, items):
    """(pixels outside the text boxes that differ, the items whose box the Rytm left as the frame has it)."""
    if not oled or not frame:
        return 8192, items
    boxes = text_boxes(items)
    inside = lambda x, y: any(b[0] <= x < b[2] and b[1] <= y < b[3] for b in boxes)   # noqa: E731
    out = sum(px(oled, x, y) != px(frame, x, y) for y in range(64) for x in range(128) if not inside(x, y))
    blank = [b[4] for b in boxes if b[4].strip() and not any(
        px(oled, x, y) != px(frame, x, y) for y in range(max(0, b[1]), min(64, b[3])) for x in range(max(0, b[0]), min(128, b[2])))]
    return out, blank


def shot(name, src):
    if SHOTS and os.path.exists(src):
        os.makedirs(SHOTS, exist_ok=True)
        import shutil
        shutil.copy(src, os.path.join(SHOTS, name + ".png"))


def t_sidecar(e):
    r = e.run("sidecar", 1200, ENTER)
    m = ev(r, rx="mode")
    check("sidecar: TRK + YES opens the page (30 on, reason user)", bool(m) and m[0]["on"] == 1 and m[0]["reason"] == "user", m[:1])
    c = ev(r, tx="commit")
    check("sidecar: the core sent the whole SIDECAR frame first (128 tiles)", bool(c) and c[0]["tiles"] == 128,
          c[0]["tiles"] if c else None)
    fr, tx = last_frame("sidecar", r), last_texts("sidecar", r)
    check("sidecar: the frame carries the page's text as Rytm text items (3D): SIDECAR, track 1's badge, FX1, FX2",
          all(any(t[4] == w for t in tx) for w in ("SIDECAR", "1", "FX1", "FX2")),
          [t[4] for t in tx][:6])
    out, blank = panel_matches(r["oled"], fr, tx)
    check("sidecar: the panel equals the core's frame outside the text, and the Rytm drew every text item",
          out == 0 and not blank, f"{out} pixels differ outside, not drawn: {blank}")
    print(f"    screenshot {os.path.join(OUT, 'sidecar.png')}")
    shot("sidecar-status-default", os.path.join(OUT, "sidecar.png"))


def t_plaits(e):
    turn_at = 1400
    r = e.run("plaits", 2600, ENTER + [(800, "press", "RIGHT"), (1000, "press", "RIGHT"), (turn_at, "turn", "1:+1", 6, 20)])
    keys = [x for x in ev(r, rx="key") if x["key"] == 0x41]
    check("plaits: RIGHT x 2 reached the core (31 0x41 down + up, twice)", len(keys) == 4, keys)
    st = [x for x in r["events"] if "stats" in x]
    page = st[-1]["stats"][14:16] if st else None
    check("plaits: the core is on a voice page (PLAITS)", page is not None and page[0] == 1, page)
    voice = page[1] if page else 0
    turns = ev(r, rx="enc")
    total = sum(x["delta"] for x in turns if x["enc"] == 1)
    check("plaits: encoder B's turns arrived summed (+6 on encoder 1)", total == 6, [(x["enc"], x["delta"]) for x in turns])
    ps = [x["param"] for x in r["events"] if "param" in x]
    hrm = [p for p in ps if p[0] == voice and p[1] == 1]
    check(f"plaits: voice {voice + 1}'s HRM (P2) in the engine moved +6 (the core's 0x03 path), nothing else",
          bool(hrm) and hrm[-1][3] - hrm[0][2] == 6 and all(p[1] == 1 for p in ps), ps)
    c = ev(r, tx="commit")
    t_turn = turn_at / 1000.0
    after = [x for x in c if x["t"] >= t_turn - 0.02]
    tiles = [x["tiles"] for x in after]
    check("plaits: each knob frame carried only changed tiles (the HRM cell's and the bottom bar's: 1..12 a frame)",
          bool(tiles) and all(0 <= k <= 12 for k in tiles) and any(tiles), tiles)
    # the panel's own tile writes over the turn window = the tiles the core sent then
    stl = r["status"]
    lo, hi = e.clock + (turn_at - 100) / 5, e.clock + 2400 / 5
    w = [x for x in stl if lo <= x[0] <= hi]
    panel = (w[-1][1].get(TILE_FN, 0) - w[0][1].get(TILE_FN, 0)) if len(w) >= 2 else None
    sent = sum(x["tiles"] for x in c if (w[0][0] - e.clock) * 5 / 1000 < x["t"] <= (w[-1][0] - e.clock) * 5 / 1000) if w else None
    # 1021: the Rytm draws the text, so the tiles under a text item that changed are written too (the core never sends
    # those pixels): sent tiles <= panel tiles <= sent + the changed text items' tiles
    txt_tiles = 0
    if w:
        prev = None
        for x in c:
            p_ = os.path.join(OUT, "frames_plaits", f"texts_{x['frame']}.json")
            cur = {(t[0], t[1], t[3]): t for t in json.load(open(p_))} if os.path.exists(p_) else {}
            if prev is not None and (w[0][0] - e.clock) * 5 / 1000 < x["t"] <= (w[-1][0] - e.clock) * 5 / 1000:
                for k in set(cur) | set(prev):
                    if cur.get(k) != prev.get(k):
                        b = text_boxes([cur.get(k) or prev.get(k)])[0]
                        txt_tiles += len({(xx // 8, yy // 8) for xx in range(max(0, b[0] + 2), min(128, b[2] - 2))
                                          for yy in range(max(0, b[1] + 1), min(64, b[3] - 1))})
            prev = cur
    check("plaits: the panel wrote the tiles the core sent and the changed text's, nothing more (no stock redraw)",
          panel is not None and sent <= panel <= sent + txt_tiles, f"panel {panel}, sent {sent}, text tiles {txt_tiles}")
    fr, tx = last_frame("plaits", r), last_texts("plaits", r)
    hv = hrm[-1][3] if hrm else None
    lab = VOICE_TRACKS[voice]   # the page names the voice by its track (core/ui/remote.cpp voice_label)
    check(f"plaits: the bottom bar names the turned knob and its value ({'/'.join(lab)} HRM:{hv}) as Rytm text",
          any(t[4] in [f"{n} HRM:{hv}" for n in lab] for t in tx), [t[4] for t in tx if "HRM" in t[4]])
    out, blank = panel_matches(r["oled"], fr, tx)
    check("plaits: the panel equals the core's last frame outside the text (the knob moved), every text item drawn",
          out == 0 and not blank, f"{out} pixels differ outside, not drawn: {blank}")
    print(f"    tiles per frame after the turn {tiles}; screenshot {os.path.join(OUT, 'plaits.png')}")
    shot("plaits-page", os.path.join(OUT, "plaits.png"))


def t_leave(e):
    ref = e.run("stock", 2600, [], link=False)
    r = e.run("leave", 2600, ENTER + [(1300, "press", "TRK", 300), (1400, "press", "YES")])
    m = ev(r, rx="mode")
    check("leave: TRK + YES again leaves (30 off, reason escape)", len(m) == 2 and m[1]["on"] == 0 and m[1]["reason"] == "escape",
          m)
    late = [x for x in ev(r, tx="commit") if m and len(m) == 2 and x["t"] > m[1]["t"]]
    check("leave: nothing drawn after leaving", not late, len(late))
    check("leave: the stock screen is back, equal to a Rytm that never opened the page",
          r["oled"] is not None and r["oled"] == ref["oled"], f"{diff_bytes(r['oled'], ref['oled'])} bytes differ")
    print(f"    screenshots {os.path.join(OUT, 'leave.png')} (after leaving), {os.path.join(OUT, 'stock.png')} (never opened)")


def t_gone(e):
    r = e.run("gone", 3600, ENTER, stop_at=1.4)
    check("gone: the core sent frames before it stopped", bool(ev(r, tx="commit")))
    stl = r["status"]
    tail = [x for x in stl if x[0] >= e.clock + 1600 / 5]
    renders = (tail[-1][1].get(RENDER, 0) - tail[0][1].get(RENDER, 0)) if len(tail) >= 2 else 0
    secs = (tail[-1][0] - tail[0][0]) / 200 if len(tail) >= 2 else 1
    check("gone: the UI kept rendering 20 times a second after the core went (no hang)", abs(renders / secs - 20) <= 1,
          f"{renders} renders in {secs:.1f} s")
    ref = e.run("stock-gone", 3600, [], link=False)
    check("gone: the stock screen is back (the Rytm's 1 s timeout), as a Rytm that never opened the page",
          r["oled"] is not None and r["oled"] == ref["oled"], f"{diff_bytes(r['oled'], ref['oled'])} bytes differ")
    print(f"    screenshot {os.path.join(OUT, 'gone.png')}")


def texts_of(name, res, before=None):
    """The text of the last commit (before emulated second `before`)."""
    c = [x for x in ev(res, tx="commit") if before is None or x["t"] < before]
    if not c:
        return []
    p = os.path.join(OUT, f"frames_{name}", f"texts_{c[-1]['frame']}.json")
    return [t[4] for t in json.load(open(p))] if os.path.exists(p) else []


def status_run(e, name, st, fx=None, events=()):
    os.environ["RUI_STATUS"] = st
    if fx:
        os.environ["RUI_SENDFX"] = fx
    try:
        return e.run(name, 1400, ENTER + list(events))
    finally:
        os.environ.pop("RUI_STATUS", None)
        os.environ.pop("RUI_SENDFX", None)


def panel_check(name, r, live=False):
    """live: a page that keeps changing (the meters): the panel may still show one of the last frames (a commit waits for
    the Rytm's next 20 Hz render), so any of the last three counts."""
    out, blank = panel_matches(r["oled"], last_frame(name, r), last_texts(name, r))
    c = ev(r, tx="commit")
    for x in (c[-3:-1] if live else []):
        if out == 0 and not blank:
            break
        d = os.path.join(OUT, f"frames_{name}")
        fr = open(os.path.join(d, f"frame_{x['frame']}.bin"), "rb").read()
        tp = os.path.join(d, f"texts_{x['frame']}.json")
        out, blank = panel_matches(r["oled"], fr, json.load(open(tp)) if os.path.exists(tp) else [])
    check(f"{name}: the panel equals the frame outside the text, every text item drawn by the Rytm", out == 0 and not blank,
          f"{out} pixels differ outside, not drawn: {blank}")


def t_status(e):
    normal = "3,75,2,1,192.168.1.20,CLUST1|---|DRONE|---|---"
    worst = "3,7425,1,2,192.168.100.200,SUPERSAW|GRANULAR|WAVETBL8|DRONE 02|BASSLINE"
    long_fx = "sendknob=1:0:8;sendknob=1:1:127;sendknob=2:0:5;sendknob=2:1:0"   # CLOUDSEED POST, TAPE DLY PRE
    right = [(800, "press", "RIGHT")]
    for tag, st, fx in (("", normal, None), ("-worst", worst, long_fx)):
        n1, n2 = f"status-p1{tag}", f"status-p2{tag}"
        r = status_run(e, n1, st, fx)
        names = texts_of(n1, r)
        check(f"{n1}: TRK + YES opens SIDECAR: the tracks (badges 1..8), FX1 / FX2, nothing of SYSTEM",
              all(w in names for w in ["SIDECAR", "FX1", "FX2"] + [str(i) for i in range(1, 9)]) and
              not any(n.startswith(("OB ", "U1", "ETH", "WIFI", "NET")) for n in names), names)
        if fx:
            check(f"{n1}: the long FX names whole, with PRE / POST", all(w in names for w in ("CLOUDSEED", "TAPE DLY", "POST", "PRE")),
                  names)
        panel_check(n1, r)
        shot(n1, os.path.join(OUT, f"{n1}.png"))
        r = status_run(e, n2, st, fx, right)
        st_ = [x for x in r["events"] if "stats" in x]
        page = st_[-1]["stats"][14] if st_ else None
        c0 = ev(r, tx="commit")   # the first frame (SIDECAR): the link's clock is not the emulator's to a few 100 ms
        before, names = texts_of(n2, r, c0[0]["t"] + 1e-6) if c0 else [], texts_of(n2, r)
        check(f"{n2}: RIGHT goes from SIDECAR to SYSTEM (the core on page 2)",
              "SIDECAR" in before and any(t.startswith("SYSTEM") for t in names) and "SIDECAR" not in names and page == 2, (before[:1], names[:1], page))
        want = (["ETH UP 192.168.1.20", "REC 1:15", "U1 CLUST1", "U3 DRONE", "12%", "34%", "5%", "OB OK", "UND 0"] if not fx else
                ["WIFI UP 192.168.100.200", "REC 123:45", "U1 SUPERSAW", "U2 GRANULAR", "U3 WAVETBL8", "U5 BASSLINE"])
        check(f"{n2}: SYSTEM shows U1..U5, the recorder, Overbridge, each core, the network", all(w in names for w in want),
              [w for w in want if w not in names])
        check(f"{n2}: nothing of the Wi-Fi but up / down (no SSID, no key)", not any("SSID" in n or "PSK" in n for n in names))
        panel_check(n2, r)
        print(f"    screenshots {os.path.join(OUT, n1 + '.png')}, {os.path.join(OUT, n2 + '.png')}")
        shot(n2, os.path.join(OUT, f"{n2}.png"))


def peek_st(e, snap, offs, n=512, raw=False):
    """0026's struct rui words at the given offsets (its symbol from rytm-mods' manifest), from a snapshot."""
    man = os.path.join(MODS, "build", "emu", TAG, "manifest.json")
    if not os.path.exists(man):
        return None
    a = json.load(open(man))["elek"]["symbols"]["rytm-0026-remote-ui:st"]
    r = subprocess.run([os.path.join(EMU, "target/release/dtpeek"), snap, "mem", f"{a:x}", str(n)], cwd=EMU,
                       env=dict(os.environ, DTEMU_FW=e.fw), capture_output=True, text=True)
    b = bytearray()
    for line in r.stdout.splitlines():
        m = re.match(r"^[0-9a-f]+: ((?:[0-9a-f]{2} ?)+)", line)
        if m:
            b += bytes.fromhex(m.group(1).replace(" ", ""))
    if raw:
        return bytes(b)
    return {k: int.from_bytes(b[o:o + 4], "big") for k, o in offs.items()}


# struct rui (0026 rui.c): cb_* 304.., er_* 316.., 1021: comps 348, echoes 364, ee_* 372.., enc_sent_now 384, e2e 396..
ST_OFFS = dict(cb_sum=304, cb_n=308, cb_max=312, er_sum=316, er_n=320, er_max=324, comps=348, texts_drawn=352,
               comp_sum=356, comp_max=360, echoes=364, ee_sum=372, ee_n=376, ee_max=380, enc_now=384, e2e_sum=396,
               e2e_n=400, e2e_max=404)
TIMER_HZ = 132e6   # DTCN0 [measured, 0026 design.md]


def t_latency(e):
    res = {}
    for mode in ("legacy", "now"):
        name = f"latency-{mode}"
        if mode == "legacy":
            os.environ["RUI_LEGACY"] = "1"
        try:   # turns of +1 every 120 ms on HRM: each a round trip on its own
            r = e.run(name, 3400, ENTER + [(800, "press", "RIGHT"), (1000, "press", "RIGHT"), (1400, "turn", "1:+1", 12, 120)],
                      save=True)
        finally:
            os.environ.pop("RUI_LEGACY", None)
        st = peek_st(e, r["snap"], ST_OFFS)
        if not st:
            check("latency: rytm-mods' manifest for the symbols", False, MODS)
            return
        ms = lambda s_, n_: 1000 * st[s_] / max(1, st[n_]) / TIMER_HZ   # noqa: E731
        mx = lambda k: 1000 * st[k] / TIMER_HZ   # noqa: E731
        res[mode] = dict(report=(ms("er_sum", "er_n"), mx("er_max")), echo=(ms("ee_sum", "ee_n"), mx("ee_max"), st["ee_n"]),
                         e2e=(ms("e2e_sum", "e2e_n"), mx("e2e_max"), st["e2e_n"]), blit=(ms("cb_sum", "cb_n"), mx("cb_max")),
                         compose=(1e6 * st["comp_sum"] / max(1, st["comps"]) / TIMER_HZ, 1e6 * st["comp_max"] / TIMER_HZ,
                                  st["comps"], st["texts_drawn"]))
        c = ev(r, tx="commit")
        rx = ev(r, rx="enc")
        lags = []
        for x in rx:   # the host: the 32's arrival -> its next commit (emulated ms)
            n = [y["t"] for y in c if y["t"] >= x["t"]]
            if n:
                lags.append(1000 * (n[0] - x["t"]))
        res[mode]["host"] = (sum(lags) / len(lags) if lags else -1, max(lags) if lags else -1)
        print(f"    {mode}: turn -> report {res[mode]['report'][0]:.1f} ms (max {res[mode]['report'][1]:.1f}); host 32 -> "
              f"frame {res[mode]['host'][0]:.1f} ms (max {res[mode]['host'][1]:.1f}); commit -> blit "
              f"{res[mode]['blit'][0]:.1f} ms (max {res[mode]['blit'][1]:.1f}); turn -> host frame on the panel "
              f"{res[mode]['e2e'][0]:.1f} ms (max {res[mode]['e2e'][1]:.1f}, n {res[mode]['e2e'][2]}); turn -> local echo "
              f"{res[mode]['echo'][0]:.1f} ms (max {res[mode]['echo'][1]:.1f}, n {res[mode]['echo'][2]}); compose "
              f"{res[mode]['compose'][0]:.0f} us mean, {res[mode]['compose'][1]:.0f} us max, {res[mode]['compose'][2]} composes, "
              f"{res[mode]['compose'][3]} texts", flush=True)
    check("latency: the local echo shows a turn on the panel (n > 0) sooner than the host's frame does",
          res["now"]["echo"][2] > 0 and res["now"]["echo"][0] < res["now"]["e2e"][0], res["now"]["echo"])
    check("latency: turn -> the host's frame on the panel is shorter than with the 1020 host",
          res["now"]["e2e"][0] < res["legacy"]["e2e"][0], (res["now"]["e2e"], res["legacy"]["e2e"]))
    json.dump(res, open(os.path.join(OUT, "latency.json"), "w"), indent=1)


K_LEFT, K_UP, K_PLAY, K_STOP, K_BD = 0x40, 0x42, 0x51, 0x52, 0x90
BOOKS = (200, 232, 264, 904)   # struct rui: stock_down, our_down, eat, 1022 both_down (32 B each)


def t_keys(e):
    # the page open: transport, a pad, a caught and a passed knob
    r = e.run("keys", 2300, ENTER + [(800, "press", "RIGHT"), (1000, "press", "RIGHT"), (1300, "press", "PLAY"),
                                     (1400, "press", "BD"), (1500, "turn", "1:+1", 3, 20), (1600, "turn", "8:+1", 3, 20),
                                     (1900, "press", "STOP")])
    rt = [x["b"] for x in ev(r, rx="rt")]
    check("keys: PLAY started and STOP stopped the sequencer with the page open (the Rytm's MIDI FA, then FC)",
          0xFA in rt and 0xFC in rt and rt.index(0xFA) < max(i for i, b in enumerate(rt) if b == 0xFC), [hex(b) for b in rt])
    keys = [(x["key"], x["down"]) for x in ev(r, rx="key")]
    check("keys: PLAY, STOP and the pad never reached the core", all(k not in (K_PLAY, K_STOP, K_BD) for k, _ in keys), keys)
    fa = [x["t"] for x in ev(r, rx="rt") if x["b"] == 0xFA][:1]
    notes = ev(r, rx="note")
    check("keys: the pad played (the Rytm's note-on 0.1 s after PLAY's FA)", bool(fa) and any(0.05 < n["t"] - fa[0] < 0.3
                                                                                              for n in notes),
          [(n["t"], n["note"]) for n in notes])
    masks = ev(r, tx="mask")
    check("keys: the core sent the catch mask on opening (SIDECAR: all nine encoders) and on the PLAITS page (its encoders)",
          len(masks) >= 2 and masks[0]["encs"] == list(range(9)) and 0x78 in masks[0]["keys"] and 1 in masks[-1]["encs"] and 8 not in masks[-1]["encs"] and
          K_PLAY not in masks[-1]["keys"] and K_BD not in masks[-1]["keys"], [(m["encs"], len(m["keys"])) for m in masks])
    enc = {}
    for x in ev(r, rx="enc"):
        enc[x["enc"]] = enc.get(x["enc"], 0) + x["delta"]
    check("keys: HRM (encoder B, caught) reached the core (+3), LEVEL (passed) did not", enc == {1: 3}, enc)
    st = [x for x in r["events"] if "stats" in x]
    voice = st[-1]["stats"][15] if st else 0
    hrm = [p for p in (x["param"] for x in r["events"] if "param" in x) if p[0] == voice and p[1] == 1]
    check("keys: the caught knob still edits the voice (HRM +3 in the engine)", bool(hrm) and hrm[-1][3] - hrm[0][2] == 3, hrm)
    m = ev(r, rx="mode")
    check("keys: the page stayed open (one 30 on, no 30 off)", [x["on"] for x in m] == [1], m)
    out, blank = panel_matches(r["oled"], last_frame("keys", r), last_texts("keys", r))
    check("keys: the panel shows the core's last frame (no stock draw over it)", out == 0 and not blank,
          f"{out} pixels differ outside, not drawn: {blank}")
    shot("keys-plaits-open", os.path.join(OUT, "keys.png"))
    # ownership: LEFT pressed on PLAITS (caught) changes the page and the mask, released after; B turned on SYSTEM
    # (every knob is caught there) edits nothing; UP held across leaving
    r = e.run("owner", 3600, ENTER + [(800, "press", "RIGHT"), (1000, "press", "RIGHT"), (1400, "press", "LEFT", 400),
                                      (2000, "turn", "1:+1", 2, 20), (2400, "press", "UP", 600),
                                      (2600, "press", "TRK", 300), (2700, "press", "YES")], save=True)
    keys = [(x["key"], x["down"], x["t"]) for x in ev(r, rx="key")]
    lk = [(d, t) for k, d, t in keys if k == K_LEFT]
    mk = [x["t"] for x in ev(r, tx="mask")]
    check("owner: LEFT went to the core down and up; the up after the mask the new page sent",
          [d for d, _ in lk] == [1, 0] and any(lk[0][1] <= t <= lk[1][1] for t in mk), (lk, mk))
    enc = [x for x in ev(r, rx="enc")]
    check("owner: encoder B on SYSTEM (caught there) reached the core or was ignored there, nothing else",
          all(x["enc"] == 1 for x in enc), enc)
    uk = [d for k, d, t in keys if k == K_UP]
    m = ev(r, rx="mode")
    check("owner: UP held across leaving: down reached the core, its release (after leaving) did not",
          uk == [1] and len(m) == 2 and m[1]["reason"] == "escape", (uk, m))
    books = peek_st(e, r["snap"], {}, 1024, raw=True)
    check("owner: 0026's key books empty (stock / ours / eaten / both)", books is not None and
          not any(any(books[o:o + 32]) for o in BOOKS), MODS)
    ref = e.run("owner-stock", 3600, [], link=False)
    check("owner: after leaving the stock screen is the one of a Rytm that never opened the page and had no turn "
          "(the turn on SYSTEM was caught)",
          r["oled"] is not None and r["oled"] == ref["oled"], f"{diff_bytes(r['oled'], ref['oled'])} bytes differ")
    # status pages catch every knob: A and LEVEL turned on SIDECAR change nothing in the stock state
    r = e.run("status-knobs", 3600, ENTER + [(1400, "turn", "0:+1", 3, 20), (1800, "turn", "8:+1", 3, 20),
                                             (2600, "press", "TRK", 300), (2700, "press", "YES")], save=True)
    m = ev(r, rx="mode")
    check("status-knobs: left again (30 on, 30 off)", [x["on"] for x in m] == [1, 0], m)
    check("status-knobs: A and LEVEL on SIDECAR never edited the voice",
          not [x for x in r["events"] if "param" in x], [x for x in r["events"] if "param" in x][:3])
    check("status-knobs: after leaving the stock screen is the one of a Rytm that had no turn",
          r["oled"] is not None and r["oled"] == ref["oled"], f"{diff_bytes(r['oled'], ref['oled'])} bytes differ")


def t_splash(e):
    play = [(1200, "press", "PLAY"), (1400, "press", "BD")]
    os.environ["RUI_STATUS"] = "1,0,2,0,192.168.1.20,-|-|-|-|-"   # the Pi on Ethernet: the address on the splash
    try:
        r = e.run("splash", 2000, play)
    finally:
        os.environ.pop("RUI_STATUS", None)
    h = ev(r, rx="0x35")
    sp = ev(r, tx="0x36")
    check("splash: the Rytm's hello (35, up > 20 s, caps 1) answered with the splash (frame, 36 64)",
          len(h) == 1 and h[0]["up"] > 400 and h[0]["caps"] == 1 and sp[:1] and sp[0]["body"] == [1, 64], (h, sp))
    c = ev(r, tx="commit")
    check("splash: after the first (whole) frame, the icon slides in (<= 18 tiles a frame), then the line runs "
          "down (<= 3)",
          len(c) >= 10 and c[0]["tiles"] == 128 and all(x["tiles"] <= 18 for x in c[1:4]) and
          all(x["tiles"] <= 3 for x in c[4:]), [x["tiles"] for x in c][:12])
    items = last_texts("splash", r)
    out, blank = panel_matches(r["oled"], last_frame("splash", r), items)
    check("splash: the panel shows the core's last splash frame (CONNECTED, the address, drawn by the Rytm)",
          out == 0 and not blank and any(t[4] == "CONNECTED" for t in items) and any(t[4] == "192.168.1.20" for t in items),
          f"{out} pixels differ outside, not drawn: {blank}")
    shot("splash_on_2000ms", os.path.join(OUT, "splash.png"))
    rt = [x["b"] for x in ev(r, rx="rt")]
    check("splash: PLAY ran the sequencer and the pad played under it; no key reached the core, the page stayed closed",
          0xFA in rt and bool(ev(r, rx="note")) and not ev(r, rx="key") and not ev(r, rx="mode"), [hex(b) for b in rt])
    r = e.run("splash-end", 4500, play)
    ref = e.run("splash-end-stock", 4500, play, link=False)
    sp = ev(r, tx="0x36")
    check("splash: 36 0 at the end (3 s); then the stock screen equals the same presses without a splash",
          [x["body"] for x in sp] == [[1, 64], [1, 0]] and r["oled"] == ref["oled"],
          (sp, f"{diff_bytes(r['oled'], ref['oled'])} bytes differ"))
    r = e.run("splash-no", 2600, [(1500, "press", "NO")])
    ref = e.run("splash-no-stock", 2600, [], link=False)
    check("splash: NO ends it early and is eaten (the stock screen as without the press)", r["oled"] == ref["oled"],
          f"{diff_bytes(r['oled'], ref['oled'])} bytes differ")
    r = e.run("splash-open", 2600, [(1300, "press", "TRK", 300), (1400, "press", "YES")])
    m = ev(r, rx="mode")
    out, blank = panel_matches(r["oled"], last_frame("splash-open", r), last_texts("splash-open", r))
    late = [x for x in ev(r, tx="0x36") if m and x["t"] > m[0]["t"]]
    check("splash: TRK + YES during it opens the SIDECAR page (30 on), the page's frame on the panel, no 36 after",
          m[:1] and m[0]["on"] == 1 and out == 0 and not blank and not late and
          any(t[4] == "SIDECAR" for t in last_texts("splash-open", r)), (m, out, blank, late))
    r = e.run("splash-gone", 3000, [], stop_at=1.0)
    ref = e.run("splash-gone-stock", 3000, [], link=False)
    check("splash: the core gone mid-splash: the stock screen back (the Rytm's 1 s without a message)",
          r["oled"] == ref["oled"], f"{diff_bytes(r['oled'], ref['oled'])} bytes differ")
    if SHOTS:   # the panel along the splash: the hello comes ~0.5 s in, then the slide (50 ms frames) and the line
        t0 = h[0]["t"] * 1000 if h else 500
        for ms in (0, 50, 100, 150, 1500, 2800):
            at = int(t0 + 120 + ms)
            rs = e.run(f"splash-shot-{ms}", at, [])
            shot(f"splash_{ms:04d}ms", os.path.join(OUT, f"splash-shot-{ms}.png"))


def rect_bytes(rects):
    """The 38 messages' bytes of a commit (6 header + 5 + the 7-in-8 packed tiles + F7)."""
    return sum(12 + (nc * np_ * 8 * 8 + 6) // 7 for _, _, nc, np_ in rects)


def t_scope(e):
    os.environ["RUI_TAP"] = "synth"
    try:
        r = e.run("scope", 4200, ENTER + [(800, "press", "LEFT"), (3000, "turn", "1:+1", 1, 20)])
    finally:
        os.environ.pop("RUI_TAP", None)
    st = [x for x in r["events"] if "stats" in x]
    page = st[-1]["stats"][14] if st else None
    check("scope: LEFT on SIDECAR opens SCOPE (the core on an extra page)", page == 3, page)
    masks = ev(r, tx="mask")
    check("scope: its catch mask takes encoders A..C only, the stock keys as every page", bool(masks) and masks[-1]["encs"] == [0, 1, 2]
          and 0x51 not in masks[-1]["keys"] and 0x90 not in masks[-1]["keys"], masks[-1:] if masks else None)
    c = [x for x in ev(r, tx="commit") if 1.4 <= x["t"] < 2.9]
    secs = 1.5
    fps, tps = len(c) / secs, sum(x["tiles"] for x in c) / secs
    bps = sum(rect_bytes(x["rects"]) + 8 for x in c) / secs
    check("scope: at most 15 frames a second while the music plays, and it moves (>= 8)", 8 <= fps <= 15.5, f"{fps:.1f}")
    check("scope: tiles within the budget (<= 900 a second, the panel link ~1560)", tps <= 900, f"{tps:.0f}")
    print(f"    scope link: {fps:.1f} frames/s, {tps:.0f} tiles/s, {bps:.0f} B/s ({bps / max(fps, 0.1):.0f} B a frame)")
    stl = r["status"]
    w = [x for x in stl if e.clock + 1400 / 5 <= x[0] <= e.clock + 2900 / 5]
    renders = (w[-1][1].get(RENDER, 0) - w[0][1].get(RENDER, 0)) if len(w) >= 2 else 0
    rs = (w[-1][0] - w[0][0]) / 200 if len(w) >= 2 else 1
    check("scope: the Rytm's UI keeps rendering 20 times a second while the scope streams", abs(renders / rs - 20) <= 1,
          f"{renders} in {rs:.1f} s")
    turns = [x for x in ev(r, rx="enc") if x["enc"] == 1]
    lag = None
    if turns:
        nxt = [y for y in ev(r, tx="commit") if y["t"] >= turns[0]["t"]]
        lag = 1000 * (nxt[0]["t"] - turns[0]["t"]) if nxt else None
    check("scope: TIME (encoder B) reaches the core, a frame follows within 30 ms", lag is not None and lag <= 30, lag)
    tx = last_texts("scope", r)
    check("scope: the bottom bar shows the new timebase (85MS), the source and the meters' labels as Rytm text",
          any("85MS" in t[4] for t in tx) and any(t[4] == "MAIN" for t in tx) and any(t[4] == "IN" for t in tx),
          [t[4] for t in tx])
    panel_check("scope", r, live=True)
    print(f"    screenshot {os.path.join(OUT, 'scope.png')}")
    shot("scope", os.path.join(OUT, "scope.png"))


def t_wave(e):
    os.environ["RUI_VOICE1"] = "0x20:0,64,64,64,0,64,64,100,64,64,1,0,10,110,0,100"
    os.environ["CORE_SAMPLE_RAM_MB"] = "16"
    try:
        r = e.run("wave", 3400, ENTER + [(800, "press", "RIGHT"), (1000, "press", "RIGHT"), (1600, "turn", "4:+1", 10, 20),
                                         (2400, "turn", "3:+2", 28, 20)])
    finally:
        os.environ.pop("RUI_VOICE1", None)
        os.environ.pop("CORE_SAMPLE_RAM_MB", None)
    st = [x for x in r["events"] if "stats" in x]
    page = st[-1]["stats"][14:16] if st else None
    check("wave: RIGHT twice opens voice 1's page", page == [1, 0], page)
    early = texts_of("wave", r, 1.1)
    mid = texts_of("wave", r, 1.55)
    check("wave: DSP SAMPLE's page, the overview loading then shown with its mode (ONE)",
          "DSP SAMPLE" in mid and "ONE" in mid and "LOADING" not in mid and "EMPTY SLOT" not in mid, (early, mid))
    fr = last_frame("wave", r)
    wavepx = sum(px(fr, x, y) for x in range(1, 127) for y in range(11, 25)) if fr else 0
    check("wave: the waveform is drawn", wavepx > 150, wavepx)
    ps = [x["param"] for x in r["events"] if "param" in x]
    sta = [p for p in ps if p[0] == 0 and p[1] == 12]
    check("wave: STA (encoder E) moved +10 in the engine", bool(sta) and sta[-1][3] - sta[0][2] == 10, sta[-3:])
    c = [x for x in ev(r, tx="commit") if 1.6 <= x["t"] < 2.3]
    tiles = [x["tiles"] for x in c]
    check("wave: each STA frame carries few tiles (the marker, the knob, the bottom bar: <= 24)", bool(tiles) and max(tiles) <= 24,
          tiles)
    tx = texts_of("wave", r)
    check("wave: MOD into GRAIN's zone: the GRAIN page (POS SIZE DENS)", all(w in tx for w in ("GRAIN", "POS", "SIZE", "DENS")), tx)
    panel_check("wave", r)
    print(f"    tiles per STA frame {tiles}; screenshot {os.path.join(OUT, 'wave.png')}")
    shot("wave", os.path.join(OUT, "wave.png"))


def t_rec(e):
    os.environ["RUI_REC"] = "1"
    os.environ["RUI_TAP"] = "synth"
    try:
        r = e.run("rec", 4200, ENTER + [(800, "press", "LEFT"), (1000, "press", "LEFT"), (1300, "press", "YES"),
                                        (3200, "press", "YES"), (3600, "turn", "0:+1", 1, 20)])
    finally:
        os.environ.pop("RUI_REC", None)
        os.environ.pop("RUI_TAP", None)
    st = [x for x in r["events"] if "stats" in x]
    page = st[-1]["stats"][14:16] if st else None
    check("rec: LEFT twice from SIDECAR opens REC (extra page 1: LFO, REC, SCOPE) and YES keeps it", page == [3, 1], page)
    masks = ev(r, tx="mask")
    check("rec: its catch mask takes encoders A..D (A arming, B C D playback)", bool(masks) and masks[-1]["encs"] == [0, 1, 2, 3],
          masks[-1:] if masks else None)
    reqs = [x["rec"] for x in r["events"] if "rec" in x]
    check("rec: YES started, YES stopped, A right armed (the platform's requests)", reqs == ["start", "stop", "arm"], reqs)
    mid = texts_of("rec", r, 3.1)
    check("rec: while recording: RECORDING, the take's name, YES STOP", all(w in mid for w in ("RECORDING", "TAKE-0008", "YES STOP")), mid)
    fr = None
    c = [x for x in ev(r, tx="commit") if x["t"] < 3.1]
    if c:
        fr = open(os.path.join(OUT, "frames_rec", f"frame_{c[-1]['frame']}.bin"), "rb").read()
    big = sum(px(fr, x, y) for x in range(19, 70) for y in range(12, 26)) if fr else 0
    check("rec: the elapsed time drawn as large digits (0:01)", big > 80, big)
    end = texts_of("rec", r)
    check("rec: armed at the end (ARMED, PLAY: REC)", "ARMED" in end and any("PLAY: REC" in t for t in end), end)
    panel_check("rec", r, live=True)
    print(f"    screenshot {os.path.join(OUT, 'rec.png')}")
    shot("rec", os.path.join(OUT, "rec.png"))


def t_gfx(e):
    os.environ["RUI_VOICE1"] = "0x48:0,64,64,100,64,64,100,127,64,64,64,64,64,64,64,64"
    try:
        r = e.run("gfx", 2600, ENTER + [(800, "press", "RIGHT"), (1000, "press", "RIGHT"), (1500, "turn", "0:+1", 5, 40)])
    finally:
        os.environ.pop("RUI_VOICE1", None)
    st = [x for x in r["events"] if "stats" in x]
    page = st[-1]["stats"][14:16] if st else None
    tx = texts_of("gfx", r)
    check("gfx: voice 1's 6-OP FM page (PAT TIM under the algorithm)", page == [1, 0] and all(w in tx for w in ("6-OP FM", "PAT", "TIM")),
          (page, tx))
    before = None
    c = [x for x in ev(r, tx="commit") if x["t"] < 1.5]
    if c:
        before = open(os.path.join(OUT, "frames_gfx", f"frame_{c[-1]['frame']}.bin"), "rb").read()
    after = last_frame("gfx", r)
    region = lambda fb: [px(fb, x, y) for x in range(0, 64) for y in range(10, 26)] if fb else None   # noqa: E731
    check("gfx: PAT turned: another voice, the algorithm diagram redrawn", before and after and region(before) != region(after))
    panel_check("gfx", r)
    print(f"    screenshot {os.path.join(OUT, 'gfx.png')}")
    shot("gfx", os.path.join(OUT, "gfx.png"))


def t_play(e):
    """Playback on the REC page against the recorder model's synthetic takes (RUI_REC)."""
    os.environ["RUI_REC"] = "1"
    os.environ["RUI_TAP"] = "synth"
    try:
        r = e.run("play", 3200, ENTER + [(800, "press", "LEFT"), (1000, "press", "LEFT"), (1300, "turn", "1:+1", 1, 20),
                                         (1600, "press", "PUSH_B"), (2200, "turn", "3:+1", 3, 20)])
        r2 = e.run("play-rec", 3600, ENTER + [(800, "press", "LEFT"), (1000, "press", "LEFT"), (1300, "press", "PUSH_B"),
                                             (1800, "turn", "2:+1", 1, 20), (2200, "press", "PUSH_B"),
                                             (2600, "press", "YES"), (3000, "press", "PUSH_B"), (3200, "turn", "1:+1", 1, 20)])
    finally:
        os.environ.pop("RUI_REC", None)
        os.environ.pop("RUI_TAP", None)
    st = [x for x in r["events"] if "stats" in x]
    page = st[-1]["stats"][14:16] if st else None
    check("play: on REC (extra page 1) after B / D turns and B pushed (the page's own)", page == [3, 1], page)
    got = [(k, x[k]) for x in r["events"] for k in ("play", "play_ch", "play_level", "play_take") if k in x]
    check("play: B turned picked BD, B pushed started it, D +3 raised the level (the platform's input, summed per report)",
          [g for g in got if g[0] != "play_level"] == [("play_ch", "BD"), ("play", "start")] and got[-1] == ("play_level", 103), got)
    end = texts_of("play", r)
    check("play: while playing: PLAYING, the take, BD 2/3, B STOP and the level and the length",
          all(w in end for w in ("PLAYING", "TAKE-0007", "BD 2/3", "B STOP  D LEV 103  1:30")), end)
    fr = last_frame("play", r)
    track = sum(px(fr, x, 28) for x in range(0, 128, 2)) if fr else 0
    mark = sum(px(fr, x, y) for x in range(3, 13) for y in range(12, 25)) if fr else 0
    check("play: the progress track (row 28) and the filled play mark drawn", track >= 60 and mark > 30, (track, mark))
    panel_check("play", r, live=True)
    shot("play", os.path.join(OUT, "play.png"))
    got2 = [(k, x[k]) for x in r2["events"] for k in ("play", "play_take", "rec") if k in x]
    check("play-rec: B played, C took the older take (stopped), B played it, YES recorded and stopped it, B / B turned "
          "during the take did nothing", got2 == [("play", "start"), ("play_take", "take-0006"), ("play", "start"),
                                                  ("rec", "start"), ("play", "stop")], got2)
    end2 = texts_of("play-rec", r2)
    check("play-rec: the recorder's view while the take records (RECORDING, YES STOP)",
          "RECORDING" in end2 and "YES STOP" in end2 and "PLAYING" not in end2, end2)
    print(f"    screenshots {os.path.join(OUT, 'play.png')}, {os.path.join(OUT, 'play-rec.png')}")
    shot("play-rec", os.path.join(OUT, "play-rec.png"))
    # C turned quickly through many takes with long names (hardware: "glitchy" while picking a take): every frame from
    # the first turn to 2.5 s after the last is the playback view (none of the recorder's in between), few tiles a turn
    os.environ.update(RUI_REC="1", RUI_TAP="synth", RUI_PLAY_TAKES="40")
    turns = [(1400 + 150 * k, "turn", "2:+1", 1, 20) for k in range(12)]
    try:
        r3 = e.run("play-takes", 4400, ENTER + [(800, "press", "LEFT"), (1000, "press", "LEFT")] + turns)
    finally:
        for k in ("RUI_REC", "RUI_TAP", "RUI_PLAY_TAKES"):
            os.environ.pop(k, None)
    tk = [x for x in r3["events"] if "play_take" in x]
    heads, tiles = [], []
    if tk:
        t0, t1 = tk[0]["t"], tk[-1]["t"] + 2.5
        for x in ev(r3, tx="commit"):
            if t0 <= x["t"] <= t1:
                p_ = os.path.join(OUT, "frames_play-takes", f"texts_{x['frame']}.json")
                tx = [i[4] for i in json.load(open(p_))] if os.path.exists(p_) else []
                heads.append("PLAY" if "PLAY" in tx else "IDLE" if "IDLE" in tx else "?")
                tiles.append(x["tiles"])
    check("play-takes: C turned 12 times: each turn a take, the playback view in every frame (no recorder frame between)",
          len(tk) == 12 and heads and all(h == "PLAY" for h in heads) and max(tiles) <= 40,
          (len(tk), "".join(h[0] for h in heads), max(tiles) if tiles else None))
    end3 = texts_of("play-takes", r3)
    check("play-takes: the last take's long name and channel n/N", "SESSION_028" in end3 and any("/13" in t for t in end3), end3[:6])
    panel_check("play-takes", r3, live=True)
    shot("play-takes", os.path.join(OUT, "play-takes.png"))


# Sidecar voice -> its tracks, as the Rytm plays them (0021's 04s; core/ui/remote.cpp kTrackVoice)
VOICE_TRACKS = {0: ["BD"], 1: ["BT"], 2: ["SD"], 3: ["LT"], 4: ["CH", "OH"], 5: ["MT", "HT"], 6: ["CY", "CB"], 7: ["RS", "CP"]}
EXT_DOWNS = {"JUNO": 21, "6-OP FM": 20}   # the machine list from track BD on the base (rytm-mods tools/emu/us_ui.json)


def t_extmach(e):
    """The machine chosen on the Rytm (SRC SRC, the machine list, YES: 0021's EXT machine) and the voice page."""
    for title, downs in EXT_DOWNS.items():
        name = "extmach_" + ("juno" if title == "JUNO" else "fm")
        evs, t = [], 300
        for k in ["SRC", "SRC"] + ["DOWN"] * downs + ["YES"]:   # 500 ms a step: the list takes them all
            evs.append((t, "press", k))
            t += 500
        t_open = t + 300
        t_pad = t_open + 1600
        evs += [(t_open, "press", "TRK", 300), (t_open + 100, "press", "YES"), (t_open + 600, "press", "RIGHT"),
                (t_open + 800, "press", "RIGHT"), (t_pad, "press", "BD"), (t_pad + 400, "press", "NO"),
                (t_pad + 1200, "press", "RIGHT"), (t_pad + 1400, "press", "RIGHT")]
        os.environ["RUI_FRESH"] = "1"
        try:
            r = e.run(name, t_pad + 2000, evs)
        finally:
            os.environ.pop("RUI_FRESH", None)
        vs = ev(r, rx="voice", voice=0)
        pad_s = (t_pad - 400) / 1000   # the link's clock runs within a few 100 ms of the emulator's
        check(f"{name}: no report from the Rytm until the track sounds (the list's YES sends none)",
              not [x for x in vs if x["t"] < pad_s], vs[:2])
        before = texts_of(name, r, pad_s)
        stl = [x for x in r["events"] if "stats" in x]
        if ev(r, rx="machines"):   # 1025+: the Rytm's 33 names the machine before any trig (sim/rui_e2e.py machines)
            check(f"{name}: before the trig: the Rytm's 33 already gave voice 1's {title} page, no PLAITS guessed",
                  "PLAITS" not in before and title in before, before[:6])
        else:
            check(f"{name}: before the trig: SIDECAR then SYSTEM, no voice page and no PLAITS guessed",
                  "PLAITS" not in before and title not in before, before[:6])
        m = 0x08 if title == "JUNO" else 0x48
        check(f"{name}: the BD pad (passed to the Rytm while the page shows) trigs the track: 0x04, machine {m:#04x}",
              any(x["cmd"] == 4 and x["machine"] == m for x in vs), vs)
        side = texts_of(name, r, (t_pad + 1000) / 1000)
        short = "JUNO" if title == "JUNO" else "FM"
        check(f"{name}: NO: SIDECAR names track 1's machine ({short})", short in side, side[:8])
        page = stl[-1]["stats"][14:16] if stl else None
        tx = texts_of(name, r)
        check(f"{name}: RIGHT twice: voice 1's {title} page", page == [1, 0] and title in tx, (page, tx[:6]))
        panel_check(name, r)
        print(f"    screenshot {os.path.join(OUT, name + '.png')}")
        shot(name, os.path.join(OUT, name + ".png"))


def rates(r):
    """Per status line (20M = 100 ms): the UI renders and the audio interrupt's (vector 174) count, as deltas."""
    ren = [c.get(RENDER, 0) for _, c in r["status"]]
    irq = [int(m) for m in re.findall(r"^\s*\d+M pc=.*?v174:(\d+)", r["out"], re.M)]
    return [b - a for a, b in zip(ren, ren[1:])], [b - a for a, b in zip(irq, irq[1:])]


def t_connect(e):
    """2026-10-19, a hardware freeze: PLAY pressed while the Sidecar was still connecting. The sequencer playing an
    EXT machine (0021's trig reports from the audio interrupt) and pads hammered while the USB host attaches and
    re-attaches N times (rytm-emu --usb-host-gone: VBUS off, a fresh host, a new enumeration, 0026's hello, the splash,
    0033's machine reports, 0030's 27), PLAY / STOP pressed at different points of each enumeration: the Rytm's UI
    keeps its 20 renders a second and the audio interrupt its 1500 blocks a second throughout, no fault, every attach
    says hello; the core silences its voices at each hello (core::link_reset: nothing hangs across a reconnect)."""
    if "--usb-host-gone" not in subprocess.run([os.path.join(EMU, "target/release/dtboot"), "--help"],
                                              capture_output=True, text=True).stdout + subprocess.run(
            [os.path.join(EMU, "target/release/dtboot"), "--help"], capture_output=True, text=True).stderr:
        check("connect: rytm-emu's dtboot has --usb-host-gone", False, "rebuild rytm-emu with the usb-host-gone work")
        return
    evs, t = [], 300
    for k in ["SRC", "SRC"] + ["DOWN"] * EXT_DOWNS["JUNO"] + ["YES"]:   # BD on JUNO (extmach's list)
        evs.append((t, "press", k))
        t += 500
    os.environ["RUI_FRESH"] = "1"
    try:
        base = e.run("connect-base", t + 1500, evs + [(t + 300, "press", "NO"), (t + 800, "press", "BD")], save=True)
        snap, clock = e.snap, e.clock
        e.snap, e.clock = base["snap"], e.clock + (t + 1500) / 5
        n, gap, ex, ev2 = int(os.environ.get("RUI_CONNECTS", "8")), 1200, [], []
        for k in range(n):
            t0 = 800 + k * gap
            up = t0 + 300 + 37 * k
            ex += ["--usb-host-gone", f"{e.at(t0)}:{e.at(up)}"]
            ev2.append((up + [0, 10, 30, 60, 120, 250, 400, 600][k % 8], "press", "PLAY" if k % 2 == 0 else "STOP"))
            ev2 += [(x, "press", "BD", 20) for x in range(t0 + 100, t0 + 900, 40)]
        ms = 800 + n * gap + 1000
        r = e.run("connect", ms, [(100, "press", "PLAY")] + ev2, extra=ex)
    finally:
        e.snap, e.clock = snap, clock
        os.environ.pop("RUI_FRESH", None)
    check("connect: JUNO on BD reported (0021's trig report from the audio interrupt)",
          any(x["cmd"] == 4 and x["machine"] == 0x08 for x in ev(base, rx="voice")), ev(base, rx="voice")[:2])
    ren, irq = rates(r)
    check(f"connect: {n} attaches while playing: the UI renders 20 a second throughout (2 every 100 ms)",
          ren and min(ren) >= 2, (min(ren) if ren else None, sum(x == 0 for x in ren)))
    check("connect: the audio interrupt keeps its 1500 blocks a second (150 every 100 ms)",
          irq and min(irq) >= 149, (min(irq) if irq else None, max(irq) if irq else None))
    m = re.search(r"enumerations: (\d+)", r["out"])
    w = re.search(r"waits from interrupt level: (\d+)", r["out"])
    check(f"connect: {n + 1} enumerations, no USB MIDI wait from interrupt level",
          m and int(m.group(1)) == n + 1 and w and w.group(1) == "0", (m and m.group(0), w and w.group(0)))
    h = ev(r, rx="0x35")
    lt = open(os.path.join(OUT, "connect.link.txt")).read()
    check("connect: a hello (35) for every re-attach, the core's voices silenced at each (link_reset)",
          len(h) >= n and lt.count("link hello") >= 1, (len(h), lt.count("link hello")))


def t_machines(_e):
    """1025 (33): the Rytm names the tracks' machines itself: JUNO picked in BD's SRC list, no trig -> its page."""
    tag = os.environ.get("RUI_TAG_1025", "rui1025")
    if not os.path.exists(os.path.join(EMU, f"sections/us_{tag}.json")):
        check(f"machines: the {tag} base in rytm-emu (rytm-mods-fxin tools/emu/us_base.sh {tag})", False)
        return
    e = Emu(tag)
    evs, t = [], 300
    for k in ["SRC", "SRC"] + ["DOWN"] * EXT_DOWNS["JUNO"] + ["YES"]:
        evs.append((t, "press", k))
        t += 500
    t_open = t + 300
    evs += [(t_open, "press", "TRK", 300), (t_open + 100, "press", "YES"), (t_open + 600, "press", "RIGHT"),
            (t_open + 800, "press", "RIGHT")]
    t_leave = t_open + 1600
    evs += [(t_leave, "press", "TRK", 300), (t_leave + 100, "press", "YES")]
    t = t_leave + 600
    for k in ["SRC", "SRC"] + ["UP"] * 8 + ["YES"]:   # the list again, up to an analog machine
        evs.append((t, "press", k))
        t += 500
    t_back = t + 300
    evs += [(t_back, "press", "TRK", 300), (t_back + 100, "press", "YES"), (t_back + 1000, "press", "RIGHT"),
            (t_back + 1200, "press", "RIGHT")]
    os.environ["RUI_FRESH"] = "1"
    try:
        r = e.run("machines", t_back + 1800, evs)
    finally:
        os.environ.pop("RUI_FRESH", None)
    m = ev(r, rx="machines")
    snaps = [x for x in m if len(x["tracks"]) == 12]
    bd = [tuple(x["tracks"][0]) for x in m if x["t0"] == 0]
    check("machines: the Rytm's 33s: snapshots (hello, page open), BD JUNO (41, 0x08) on the way, BD analog (vb 0x7F) last",
          len(snaps) >= 2 and (41, 0x08) in bd and bd[-1][1] == 0x7F and bd[-1][0] < 35, (len(snaps), bd[-4:]))
    vs = ev(r, rx="voice")
    check("machines: no voice report at all (nothing sounded: the page comes from 33 alone)", not vs, vs[:2])
    on = texts_of("machines", r, (t_leave - 200) / 1000)
    check("machines: TRK + YES, RIGHT twice: voice 1's JUNO page without a trig", "JUNO" in on and "PAT" in on, on[:6])
    side = texts_of("machines", r, (t_back + 700) / 1000)
    check("machines: BD back on an analog machine: SIDECAR names no machine for track 1", "JUNO" not in side and
          "SIDECAR" in side, side[:8])
    st = [x for x in r["events"] if "stats" in x]
    page = st[-1]["stats"][14:16] if st else None
    end = texts_of("machines", r)
    check("machines: RIGHT twice now passes no voice page (SCOPE), the JUNO page gone", page and page[0] != 1 and
          "JUNO" not in end, (page, end[:3]))
    panel_check("machines", r, live=True)
    shot("machines", os.path.join(OUT, "machines.png"))


def t_sdvoice(_e):
    """JUNO on SD (track 2), whose voice is not 0026's 33 table's: the Rytm plays SD on voice 2 (0021's 04 carries
    0x0A), 0026's 33 says 0x09 (voice 1). The Sidecar takes the voice from the 04s' table (core/ui/remote.cpp
    kTrackVoice): SIDECAR names track 2 JUNO, the voice page is V3's, and an edit there goes to voice 2."""
    tag = os.environ.get("RUI_TAG_1025", "rui1025")
    if not os.path.exists(os.path.join(EMU, f"sections/us_{tag}.json")):
        check(f"sdvoice: the {tag} base in rytm-emu", False)
        return
    e = Emu(tag)
    evs, t = [(300, "press", "SD")], 900   # SD selected by its pad (rytm-mods tools/emu/us_ui.json downs_sd: U1 18)
    for k in ["SRC", "SRC"] + ["DOWN"] * (18 + 6) + ["YES"]:   # U1..U5 18..22, 6-OP FM 23, JUNO 24
        evs.append((t, "press", k))
        t += 500
    t_pad = t + 300
    t_open = t_pad + 800
    evs += [(t_pad, "press", "SD"), (t_open, "press", "TRK", 300), (t_open + 100, "press", "YES"),
            (t_open + 600, "press", "RIGHT"), (t_open + 800, "press", "RIGHT")]
    os.environ["RUI_FRESH"] = "1"
    try:
        r = e.run("sdvoice", t_open + 1600, evs)
    finally:
        os.environ.pop("RUI_FRESH", None)
    sd = [tuple(x["tracks"][0]) for x in ev(r, rx="machines") if x["t0"] == 1 and len(x["tracks"]) == 1]
    check("sdvoice: SD's 33 on the way: JUNO (41) with 0026's vb 0x09 (voice 1)", (41, 0x09) in sd, sd[-4:])
    vs = [x for x in ev(r, rx="voice") if x["cmd"] == 4 and x["machine"] == 0x08]
    check("sdvoice: SD's pad: the Rytm's 04 for JUNO is voice 2's (0x0A), not 33's voice 1",
          vs and all(x["voice"] == 2 for x in vs), [(x["voice"], x["machine"]) for x in vs][:3])
    st = [x for x in r["events"] if "stats" in x]
    page = st[-1]["stats"][14:16] if st else None
    tx = texts_of("sdvoice", r)
    check("sdvoice: TRK + YES, RIGHT twice: voice 2 (index; SD plays it) JUNO page, named SD", page == [1, 2] and "JUNO" in tx and "SD" in tx,
          (page, tx[:6]))
    panel_check("sdvoice", r, live=True)
    shot("sdvoice", os.path.join(OUT, "sdvoice.png"))


def t_noexit(e):
    """NO on SIDECAR leaves the page (3A 0 0), as TRK + YES: TRK + YES, RIGHT, NO -> SIDECAR, NO -> the stock screen."""
    ref = e.run("stock", 2600, [], link=False)
    r = e.run("noexit", 2600, ENTER + [(900, "press", "RIGHT"), (1100, "press", "NO"), (1400, "press", "NO")])
    side = texts_of("noexit", r, 1.35)
    check("noexit: RIGHT, NO: back on SIDECAR", "SIDECAR" in side, side[:4])
    m = ev(r, rx="mode")
    check("noexit: NO on SIDECAR: the Rytm leaves the page (30 off)", len(m) == 2 and m[1]["on"] == 0, m)
    late = [x for x in ev(r, tx="commit") if len(m) == 2 and x["t"] > m[1]["t"]]
    check("noexit: nothing drawn after leaving", not late, len(late))
    check("noexit: the stock screen is back, equal to a Rytm that never opened the page",
          r["oled"] is not None and r["oled"] == ref["oled"], f"{diff_bytes(r['oled'], ref['oled'])} bytes differ")


def t_srcgfx(_e):
    """The real path to the machine graphics (rui1025's 33): JUNO picked in BD's SRC list (no trig) -> 33 -> the page
    list -> RIGHT twice: the JUNO page with its envelope and patch; then 6-OP FM in the same list -> its algorithm."""
    tag = os.environ.get("RUI_TAG_1025", "rui1025")
    if not os.path.exists(os.path.join(EMU, f"sections/us_{tag}.json")):
        check(f"srcgfx: the {tag} base in rytm-emu", False)
        return
    e = Emu(tag)
    evs, t = [], 300
    for k in ["SRC", "SRC"] + ["DOWN"] * EXT_DOWNS["JUNO"] + ["YES"]:
        evs.append((t, "press", k))
        t += 500
    t_open = t + 300
    evs += [(t_open, "press", "TRK", 300), (t_open + 100, "press", "YES"), (t_open + 600, "press", "RIGHT"),
            (t_open + 800, "press", "RIGHT")]
    t_leave = t_open + 1600
    evs += [(t_leave, "press", "TRK", 300), (t_leave + 100, "press", "YES")]
    t = t_leave + 600
    for k in ["SRC", "SRC", "UP", "YES"]:   # the list's cursor on JUNO (41): one up is 6-OP FM (40)
        evs.append((t, "press", k))
        t += 500
    t_fm = t + 300
    evs += [(t_fm, "press", "TRK", 300), (t_fm + 100, "press", "YES"), (t_fm + 600, "press", "RIGHT"),
            (t_fm + 800, "press", "RIGHT")]
    r = e.run("srcgfx", t_fm + 1600, evs)   # voice 1 reported on PLAITS first (as on a Sidecar that heard a trig)
    bd = [tuple(x["tracks"][0]) for x in ev(r, rx="machines") if x["t0"] == 0]
    check("srcgfx: BD's 33s: JUNO (41, 0x08), then 6-OP FM (40, 0x48)", (41, 0x08) in bd and bd[-1] == (40, 0x48), bd[-4:])

    def frame_before(sec):
        c = [x for x in ev(r, tx="commit") if x["t"] < sec]
        return open(os.path.join(OUT, "frames_srcgfx", f"frame_{c[-1]['frame']}.bin"), "rb").read() if c else None

    ink = lambda fb, x0, x1, y0, y1: sum(px(fb, x, y) for x in range(x0, x1) for y in range(y0, y1)) if fb else 0  # noqa: E731
    on = texts_of("srcgfx", r, (t_leave - 200) / 1000)
    fj = frame_before((t_leave - 200) / 1000)
    check("srcgfx: JUNO: TRK + YES, RIGHT twice: voice 1's JUNO page (PAT CUT ... LEN)", "JUNO" in on and "LEN" in on, on[:6])
    check("srcgfx: JUNO: the envelope drawn (cell row 2, columns 1..3) and the patch track (cell 1)",
          ink(fj, 2, 94, 32, 48) > 60 and ink(fj, 2, 30, 20, 26) > 6, (ink(fj, 2, 94, 32, 48), ink(fj, 2, 30, 20, 26)))
    if fj:
        frame_png(os.path.join(OUT, "srcgfx_juno.png"), fj)
    end = texts_of("srcgfx", r)
    ff = last_frame("srcgfx", r)
    check("srcgfx: 6-OP FM: the list again, one up, YES: voice 1's 6-OP FM page", "6-OP FM" in end and "TIM" in end, end[:6])
    check("srcgfx: 6-OP FM: the algorithm diagram drawn (the two-column cell)", ink(ff, 1, 63, 11, 30) > 40,
          ink(ff, 1, 63, 11, 30))
    panel_check("srcgfx", r)
    shot("srcgfx-juno", os.path.join(OUT, "srcgfx_juno.png"))
    shot("srcgfx-fm", os.path.join(OUT, "srcgfx.png"))


def t_srcwave(_e):
    """The real path to DSP SAMPLE's waveform and GRAIN (rui1025's 33, no voice forced): DSP SAMPLE picked in BD's SRC
    list, the BD pad (its report: the voice on DSP SAMPLE), TRK + YES, RIGHT twice -> its page (SMP OFF, the kit's), SMP
    (C) to pool slot 1 -> the sample's waveform; MOD (D) turned on the page into GRAIN's zone -> the GRAIN view."""
    tag = os.environ.get("RUI_TAG_1025", "rui1025")
    e = Emu(tag)
    evs, t = [], 300
    for k in ["SRC", "SRC"] + ["DOWN"] * (EXT_DOWNS["JUNO"] + 3) + ["YES"]:   # JUNO 41, 42, 44, DSP SAMPLE 45
        evs.append((t, "press", k))
        t += 500
    evs += [(t + 400, "press", "BD")]
    t_open = t + 1000
    t_mod = t_open + 1600
    evs += [(t_open, "press", "TRK", 300), (t_open + 100, "press", "YES"), (t_open + 600, "press", "RIGHT"),
            (t_open + 800, "press", "RIGHT"), (t_open + 1100, "turn", "2:+1", 1, 20), (t_mod, "turn", "3:+2", 28, 20)]
    os.environ.update(RUI_SAMPLES="1", CORE_SAMPLE_RAM_MB="16")
    try:
        r = e.run("srcwave", t_mod + 1800, evs)
    finally:
        os.environ.pop("RUI_SAMPLES", None)
        os.environ.pop("CORE_SAMPLE_RAM_MB", None)
    bd = [tuple(x["tracks"][0]) for x in ev(r, rx="machines") if x["t0"] == 0]
    check("srcwave: BD's 33: DSP SAMPLE (45, 0x20)", bool(bd) and bd[-1] == (45, 0x20), bd[-3:])
    vs = ev(r, rx="voice", voice=0)
    check("srcwave: the BD pad trigs the track: 0x04 machine 0x20", any(x["cmd"] == 4 and x["machine"] == 0x20 for x in vs), vs[:3])
    mid = texts_of("srcwave", r, (t_mod - 100) / 1000)
    c = [x for x in ev(r, tx="commit") if x["t"] < (t_mod - 100) / 1000]
    fr = open(os.path.join(OUT, "frames_srcwave", f"frame_{c[-1]['frame']}.bin"), "rb").read() if c else None
    wavepx = sum(px(fr, x, y) for x in range(1, 127) for y in range(11, 25)) if fr else 0
    check("srcwave: DSP SAMPLE's page with the pool sample's waveform", "DSP SAMPLE" in mid and "SMP OFF" not in mid and
          wavepx > 150, (mid[:8], wavepx))
    if fr:
        frame_png(os.path.join(OUT, "srcwave_dsps.png"), fr)
    end = texts_of("srcwave", r)
    check("srcwave: MOD into GRAIN's zone: the GRAIN view (POS SIZE DENS)", all(w in end for w in ("GRAIN", "POS", "SIZE", "DENS")),
          end[:8])
    panel_check("srcwave", r)
    shot("srcwave-dsps", os.path.join(OUT, "srcwave_dsps.png"))
    shot("srcwave-grain", os.path.join(OUT, "srcwave.png"))


def t_srcsmp(_e):
    """1027, the SRC view on DSP SAMPLE (rui1027): DSP SAMPLE picked in BD's SRC list, the stock SMPL page, SMP (D: TUN FIN BR SMP)
    turned to pool slot 1 there, then SRC: the view draws that sample's waveform (the Rytm's P11 from 2A, the overview
    read while the view shows), never SMP OFF; the voice never sounded (no trig)."""
    tag = os.environ.get("RUI_TAG_1027", "rui1027")
    if not os.path.exists(os.path.join(EMU, f"sections/us_{tag}.json")):
        check(f"srcsmp: the {tag} base in rytm-emu", False)
        return
    e = Emu(tag)
    evs, t = [], 300
    for k in ["SRC", "SRC"] + ["DOWN"] * (EXT_DOWNS["JUNO"] + 3) + ["YES"]:   # JUNO 41, 42, 44, DSP SAMPLE 45
        evs.append((t, "press", k))
        t += 500
    t_smp = t + 500
    # an enum steps by one with 6 counts of +8 (rytm-mods' test); SRC at once: the 29 t 1's 2A may carry the old SMP, the
    # changed span follows when the kit word lands (~0.5-1.3 s)
    evs += [(t_smp, "press", "SMPL"), (t_smp + 600, "turn", "3:+8", 6, 50), (t_smp + 1500, "press", "SRC")]
    t_end = t_smp + 4500
    os.environ.update(RUI_SAMPLES="1", CORE_SAMPLE_RAM_MB="16", RUI_FRESH="1")
    try:
        r = e.run("srcsmp", t_end, evs)
    finally:
        for k in ("RUI_SAMPLES", "CORE_SAMPLE_RAM_MB", "RUI_FRESH"):
            os.environ.pop(k, None)
    vals = [x for x in ev(r, rx="srcvals") if x["track"] == 0]
    smp = [x["values"][10 - x["first"]] for x in vals if x["first"] <= 10 < x["first"] + len(x["values"])]
    check("srcsmp: the Rytm's 2A carries SMP (P11) = slot 1 by the end", smp and int(smp[-1]) == 1, smp[-4:])
    end = texts_of("srcsmp", r)
    fr = last_frame("srcsmp", r)
    wavepx = sum(px(fr, x, y) for x in range(1, 101) for y in range(11, 25)) if fr else 0
    check("srcsmp: SRC: the view's DSP SAMPLE page with the sample's waveform, not SMP OFF", "DSP SAMPLE" in end and
          "SMP OFF" not in end and "LOADING" not in end and wavepx > 150, (end[:8], wavepx))
    check("srcsmp: no voice report (nothing sounded: the view reads the Rytm's values)", not ev(r, rx="voice"), "")
    if fr:
        frame_png(os.path.join(OUT, "srcsmp.png"), fr)
    shot("srcsmp", os.path.join(OUT, "srcsmp.png"))


def t_srcreplug(_e):
    """1027, the SRC view across a replug (rytm-emu --usb-host-gone, rui1027): BD on JUNO, its SRC page shows the view;
    the USB host goes and comes back (a) while the SRC page shows, (b) while TRIG shows, then SRC: after each the
    Rytm's hello, its 29 0 1, the core's whole frame, and the panel showing the core's frame (not stock SRC)."""
    tag = os.environ.get("RUI_TAG_1027", "rui1027")
    if not os.path.exists(os.path.join(EMU, f"sections/us_{tag}.json")):
        check(f"srcreplug: the {tag} base in rytm-emu", False)
        return
    for case in ("onsrc", "ontrig"):
        e = Emu(tag)
        evs, t = [], 300
        for k in ["SRC", "SRC"] + ["DOWN"] * EXT_DOWNS["JUNO"] + ["YES"]:
            evs.append((t, "press", k))
            t += 500
        t_gone = t + 1500
        if case == "ontrig":
            evs.append((t_gone - 600, "press", "TRIG"))
        t_up = t_gone + int(os.environ.get("SRCREPLUG_GONE_MS", "3000"))   # past the Rytm's 1 s timeout, as a hand replug
        if case == "ontrig":
            evs.append((t_up + 4500, "press", "SRC"))
        t_end = t_up + 7000
        name = f"srcreplug-{case}"
        r = e.run(name, t_end, evs, extra=["--usb-host-gone", f"{e.at(t_gone)}:{e.at(t_up)}"])
        hel = [x["t"] for x in ev(r, rx="0x35")]
        # the link's clock (Overbridge frames) stands while the host is gone: "after" = from the last hello on
        on = [x["t"] for x in ev(r, rx="srcview") if x["on"] == 1 and hel and x["t"] >= hel[-1]]
        com = [x["t"] for x in ev(r, tx="commit") if on and x["t"] > on[-1]]
        print(f"    {name}: hellos {[round(h, 2) for h in hel]}, 29 on after {[round(x, 2) for x in on[:4]]}, "
              f"commits after the last {len(com)}", flush=True)
        check(f"{name}: a hello after the replug, then the Rytm's 29 0 1", hel and on, (hel, on[:3]))
        check(f"{name}: the core answers with frames", bool(com), len(com))
        end = texts_of(name, r)
        check(f"{name}: the core's last frame is BD's JUNO view", "JUNO" in end, end[:4])
        panel_check(name, r)
        shot(name, os.path.join(OUT, f"{name}.png"))


def t_srcview(_e):
    """1027, the SRC view (rui1027's base, RUI_TAG_1027): JUNO picked in BD's SRC list, the stock SRC page of BD stays:
    the Rytm says 29 0 1 and sends BD's values (2A), the core answers with a whole frame of BD's JUNO page drawn from
    them (no 37, no 3F, no 29 back); CUT (encoder B) turned on the Rytm: its 2As, the bottom bar names CUT, the engine's voice the Rytm's (the
    page only reads); TRK + YES opens the remote page (it comes first), leaving it shows the view again; the list up to
    an analog machine: the Rytm's 29 0 0, the core draws nothing more."""
    tag = os.environ.get("RUI_TAG_1027", "rui1027")
    if not os.path.exists(os.path.join(EMU, f"sections/us_{tag}.json")):
        check(f"srcview: the {tag} base in rytm-emu (sidecar-mods tools/emu/us_base.sh {tag})", False)
        return
    e = Emu(tag)
    evs, t = [], 300
    for k in ["SRC", "SRC"] + ["DOWN"] * EXT_DOWNS["JUNO"] + ["YES"]:
        evs.append((t, "press", k))
        t += 500
    t_turn = t + 2500
    evs.append((t_turn, "turn", "1:+8", 6, 50))   # CUT up on the Rytm's own SRC page (stock: ~3 counts a step)
    t_open = t_turn + 1200
    evs += [(t_open, "press", "TRK", 300), (t_open + 100, "press", "YES"), (t_open + 500, "press", "RIGHT"),
            (t_open + 700, "press", "RIGHT")]   # SIDECAR, SYSTEM, then no voice page (the SRC view has them)
    t_leave = t_open + 1200
    evs += [(t_leave, "press", "TRK", 300), (t_leave + 100, "press", "YES")]
    t_back = t_leave + 1000
    t = t_back
    for k in ["SRC", "SRC"] + ["UP"] * 8 + ["YES"]:   # the list again (rytm-mods mrep list_open), up to an analog machine
        evs.append((t, "press", k))
        t += 500
    t_end = t + 1500
    os.environ["RUI_FRESH"] = "1"
    try:
        r = e.run("srcview", t_end, evs)
    finally:
        os.environ.pop("RUI_FRESH", None)
    views = ev(r, rx="srcview")
    vals = [x for x in ev(r, rx="srcvals") if x["track"] == 0]
    check("srcview: the Rytm's 29 0 1 (BD's SRC page on JUNO) and BD's values (2A)",
          any(x["track"] == 0 and x["on"] == 1 for x in views) and vals, (views[:4], len(vals)))
    on1 = [x["t"] for x in views if x["track"] == 0 and x["on"] == 1]
    commits = ev(r, tx="commit")
    check("srcview: the core answers BD's 29 0 1 with a whole frame (a commit after it), never a 29 back",
          on1 and any(x["t"] > on1[0] for x in commits) and not ev(r, tx="0x29"), (on1[:2], len(commits)))
    masks = [x for x in ev(r, tx="mask") if x["t"] < t_open / 1000]
    check("srcview: no catch mask in the view (every key and knob the Rytm's)", not masks, masks[:2])
    view = texts_of("srcview", r, (t_open - 100) / 1000)
    check("srcview: BD's JUNO page drawn in the view, the bottom bar names CUT", "JUNO" in view and
          any(str(x).startswith("BD CUT:") for x in view), view[:10])
    one = [x for x in vals if len(x["values"]) == 1 and x["first"] == 1]
    check("srcview: the turn came as 2A n 1 for P2 (CUT), above its 64", one and one[-1]["values"][0] > 64,
          [x["values"] for x in one])
    pg = texts_of("srcview", r, (t_leave - 50) / 1000)
    check("srcview: TRK + YES, RIGHT twice: not BD's JUNO page (no voice pages once the Rytm has the SRC view)",
          pg and "PAT" not in pg and "CUT" not in pg, pg[:6])
    back = texts_of("srcview", r, (t_back - 100) / 1000)
    check("srcview: after TRK + YES left: the view again", "JUNO" in back, back[:6])
    off = [x["t"] for x in views if x["track"] == 0 and x["on"] == 0]
    late = [x for x in commits if off and x["t"] > off[-1] + 0.1]
    check("srcview: BD analog: the Rytm's 29 0 0, nothing drawn after it", off and not late, (off[-1:], len(late)))
    shot("srcview", os.path.join(OUT, "srcview.png"))


def t_srcturn(_e):
    """1027, the SRC view's responsiveness (rui1027): BD on JUNO, its stock SRC page, CUT (encoder B) turned for 2 s
    (+8 a count every 20 ms, then -8): per 2A the time from the turn that caused it (the Rytm's part), per 2A the time
    to the core's commit (the host's part), the tiles and bytes a frame. Prints the numbers; checks the host's part."""
    tag = os.environ.get("RUI_TAG_1027", "rui1027")
    if not os.path.exists(os.path.join(EMU, f"sections/us_{tag}.json")):
        check(f"srcturn: the {tag} base in rytm-emu", False)
        return
    e = Emu(tag)
    evs, t = [], 300
    for k in ["SRC", "SRC"] + ["DOWN"] * EXT_DOWNS["JUNO"] + ["YES"]:
        evs.append((t, "press", k))
        t += 500
    t0 = t + 2500
    n = int(os.environ.get("SRCTURN_N", "50"))
    evs += [(t0, "turn", "1:+8", n, 20), (t0 + 20 * n + 500, "turn", "1:-8", n, 20)]
    r = e.run("srcturn", t0 + 40 * n + 2000, evs)
    turns = [(t0 + 20 * k) / 1000 for k in range(n)] + [(t0 + 20 * n + 500 + 20 * k) / 1000 for k in range(n)]
    vals = [x for x in ev(r, rx="srcvals") if x["track"] == 0 and x["t"] >= t0 / 1000]
    commits = [x for x in ev(r, tx="commit") if x["t"] >= t0 / 1000]
    lat_r, lat_h, prev = [], [], None
    for x in vals:   # the Rytm: from the first turn since the previous 2A
        since = [tt for tt in turns if tt <= x["t"] and (prev is None or tt > prev)]
        if since:
            lat_r.append(1000 * (x["t"] - since[0]))
        nxt = [c for c in commits if c["t"] >= x["t"]]
        if nxt:
            lat_h.append(1000 * (nxt[0]["t"] - x["t"]))
        prev = x["t"]
    gaps = [1000 * (b["t"] - a["t"]) for a, b in zip(vals, vals[1:])]
    link = open(os.path.join(OUT, "srcturn.link.txt")).read() if os.path.exists(os.path.join(OUT, "srcturn.link.txt")) else ""
    tiles = [c["tiles"] for c in commits]
    med = lambda v: sorted(v)[len(v) // 2] if v else None  # noqa: E731
    print(f"  srcturn: {len(turns)} turns, {len(vals)} 2As (gap median {med(gaps)} ms, max {max(gaps) if gaps else None}),"
          f" {len(commits)} commits; turn->2A median {med(lat_r)} ms max {max(lat_r) if lat_r else None};"
          f" 2A->commit median {med(lat_h)} ms max {max(lat_h) if lat_h else None}; tiles/commit median {med(tiles)}"
          f" max {max(tiles) if tiles else None}", flush=True)
    nb = [sum(12 + a[2] * a[3] * 8 + (a[2] * a[3] * 8 + 6) // 7 for a in c["rects"]) + 9 for c in commits]   # 38s + 39
    print(f"  srcturn: bytes/commit (38s + 39, without 3D) median {med(nb)} max {max(nb) if nb else None}", flush=True)
    check("srcturn: a 2A every Rytm UI tick while the knob turns (gap median <= 60 ms)", gaps and med(gaps) <= 60,
          (len(vals), med(gaps)))
    check("srcturn: the core commits within 10 ms of each 2A (the input path)", lat_h and max(lat_h) <= 10, lat_h[:8])
    check("srcturn: a turn's frame is a few tiles, not the whole page", tiles and med(tiles) <= 24, tiles[:10])


def lfo_des(d):
    """The LFO JSON's design hex -> [(value, shape)] x 16."""
    return [(int(d[3 * k:3 * k + 2], 16) - 64, int(d[3 * k + 2], 16)) for k in range(16)]


def t_lfo(e):
    # L1 on FX1's P2 (a square at 0.02 Hz), L2 a fast sine on FX2's P3, L3 on voice 1's HRM (the engine's own); 1026: no
    # Rytm targets, so not a single CC goes to the Rytm
    os.environ["RUI_LFO"] = ";".join(["lfo=1:tgt:fx1", "lfo=1:par:0", "lfo=1:wav:4", "lfo=1:rate:0", "lfo=1:dep:40",
                                      "lfo=2:tgt:fx2", "lfo=2:par:1", "lfo=2:wav:0", "lfo=2:rate:92", "lfo=2:dep:50",
                                      "lfo=3:tgt:v1", "lfo=3:par:1", "lfo=3:dep:20", "lfo=4:tgt:bd"])
    try:
        r = e.run("lfo", 3600, ENTER + [(800, "press", "LEFT"), (1000, "press", "LEFT"), (1400, "turn", "3:+1", 3, 20),
                                        (1800, "press", "YES"), (2100, "press", "T5"), (2400, "turn", "1:-1", 5, 20),
                                        (2800, "press", "NO")])
    finally:
        os.environ.pop("RUI_LFO", None)
    masks = ev(r, tx="mask")
    main = [m for m in masks if m["encs"] == list(range(8))]
    des = [m for m in masks if m["encs"] == [0, 1, 2, 3] and all(k in m["keys"] for k in range(16))]
    check("lfo: LEFT x 2 from SIDECAR opens the LFO page (no REC on this board): its mask takes all eight encoders, no trig key",
          bool(main) and not any(k in main[0]["keys"] for k in range(16)), main[:1])
    check("lfo: YES opens the design view: A..D and the trig keys T1..T16 caught; NO back to the main view's mask",
          bool(des) and masks[-1]["encs"] == list(range(8)) and not any(k in masks[-1]["keys"] for k in range(16)),
          [m["encs"] for m in masks])
    lj = [x["lfo"] for x in r["events"] if "lfo" in x and x["lfo"]]
    l = lj[-1]["l"] if lj else None
    check("lfo: encoder D on the page set L1's depth (40 + 3)", bool(l) and l[0]["dep"] == 43, l[0] if l else None)
    check("lfo: L4 on BD refused (no Rytm targets): OFF", bool(l) and l[3]["tgt"] == "OFF", l[3] if l else None)
    d = lfo_des(l[0]["des"]) if l else None
    check("lfo: T5 picked step 5, encoder B lowered its value by 5 (the triangle's 0 -> -5)", bool(d) and d[4][0] == -5,
          d[:6] if d else None)
    cc = ev(r, tx="cc")
    check("lfo: not a single CC to the Rytm", not cc, cc[:3])
    sx = [x for x in r["events"] if x.get("tx") == "commit"]
    check("lfo: the page keeps the link light (the dot every 50 ms: < 400 tiles a second)",
          sum(x["tiles"] for x in sx if 1.2 <= x["t"] < 1.8) / 0.6 < 400, sum(x["tiles"] for x in sx if 1.2 <= x["t"] < 1.8))
    panel_check("lfo", r, live=True)
    shot("lfo-page", os.path.join(OUT, "lfo.png"))


TESTS = dict(connect=t_connect, sidecar=t_sidecar, plaits=t_plaits, status=t_status, leave=t_leave, gone=t_gone, keys=t_keys,
             splash=t_splash, latency=t_latency, scope=t_scope, wave=t_wave, rec=t_rec, play=t_play, gfx=t_gfx, extmach=t_extmach,
             machines=t_machines, sdvoice=t_sdvoice, srcgfx=t_srcgfx, srcwave=t_srcwave, noexit=t_noexit, lfo=t_lfo,
             srcview=t_srcview, srcturn=t_srcturn, srcsmp=t_srcsmp, srcreplug=t_srcreplug)
DEFAULT = ["connect", "sidecar", "plaits", "status", "leave", "gone", "keys", "splash", "scope", "wave", "rec", "play", "gfx", "extmach",
           "machines", "srcgfx", "srcwave", "noexit", "lfo"]


def main():
    names = sys.argv[1:] or DEFAULT
    e = Emu()
    for n in names:
        print(f"[{n}]", flush=True)
        TESTS[n](e)
    print("ALL PASS" if not FAILS else f"FAILED: {FAILS}")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()
