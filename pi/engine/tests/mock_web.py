#!/usr/bin/env python3
"""pi/engine/tests/mock_web.py - the Sidecar page (pi/engine/www/index.html) against a stand-in for web.cpp, to try
the page in a browser without the Pi: [MOCK_REFUSE=0.3] python3 pi/engine/tests/mock_web.py [PORT] [CARD_DIR] (default 8765, a temp
folder seeded with the boot files, /Samples, /.rytm and some macOS dot-files).

/wifi/* as wifi.cpp and web.cpp: fake networks (Fake-*), a join that "restarts" (503 for 3 s) then tries for a few
seconds: the password "wrong-pass" fails as wrong_password, an SSID not in the scan as not_found, else it is up; a
failed one reverts (the previous settings back). join/forget/power need the same-origin headers.

The pairing code (pi/engine/paircode.h) as web.cpp gates it: /pair/status, /pair, /pair/resume, /pair/forget,
/pair/revoke, /pair/show, the same list of gated endpoints, the token from the X-Sidecar-Auth header (or tok= in a
POST's body - never the query), the "401 not paired" answers with the X-Sidecar-Pair header, and the devices remembered in
<CARD_DIR>/.rytm/pair.txt so a restart of this mock is a reboot of the Pi. The code is random per run and written to
<CARD_DIR>/sidecar-paircode.txt, as the engine does when no touchscreen is attached (there is none here); a test reads
it from there. MOCK_PAIR=0 is cmdline.txt paircode=0 (the gate off), MOCK_PAIR=<8 digits> a fixed code.
MOCK_PAIR_LOCK / MOCK_PAIR_GAP (seconds, default 60 and 1 as paircode.h) shorten the lockout for a test. The limiter
is per peer as paircode.h's (its global backstop is not mirrored); a test plays a second address with the mock-only
header X-Mock-Peer.
A sketch for the page's tests; make -C pi/engine/tests (test_paircode) is the reference.

/status, /log (with the periodic stats line), /cmd (logged; stor= runs a pretend bench) and /fs/* over CARD_DIR with
fsweb.cpp's rules: /Samples only (the boot files, /.rytm and anything else unreachable), the kept folders /.rytm and
/Samples ("ro":2), put via <path>.part with its CRC, mkdir?p=1. A sketch of the
rules for the page's tests, not their reference: make -C pi/engine/tests (test_fsweb) is."""
import gzip, json, math, os, re, struct, sys, tempfile, time, zlib
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlsplit, parse_qs

HERE = os.path.dirname(os.path.abspath(__file__))
PAGE = os.path.join(HERE, "..", "www", "index.html")
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
ROOT = sys.argv[2] if len(sys.argv) > 2 else tempfile.mkdtemp(prefix="sidecar-card-")
T0 = time.time()
import random
REFUSE = float(os.environ.get("MOCK_REFUSE", "0") or 0)
REFUSED = [0]
# the supply (power_state.cpp's json): MOCK_POWER=weak: a Pi 5 on a hub port (3 A mode, under-voltage since boot)
POWER = ({"known": True, "throttled": 0x50000, "uvNow": False, "uvBoot": True, "throttledNow": False, "throttledBoot": True,
          "cappedNow": False, "cappedBoot": False, "maxCurrent": 3000,
          "warn": "power supply too weak (no 5 A USB-PD: 3 A mode): use the 27 W (5 V 5 A) adapter directly"}
         if os.environ.get("MOCK_POWER") == "weak" else
         {"known": True, "throttled": 0, "uvNow": False, "uvBoot": False, "throttledNow": False, "throttledBoot": False,
          "cappedNow": False, "cappedBoot": False, "maxCurrent": 5000, "warn": None})
LOG = []
BOOT = {"config.txt", "config.txt.bak", "cmdline.txt", "bootcode.bin", "licence.broadcom", "issue.txt"}
BOOTP = ("sdboot", "engine.img", "kernel", "start", "fixup", "armstub", "tryboot.txt", "bcm27")
KEPT = {"/.rytm", "/samples"}
UP = {}
def page():
    """index.html with its /*@include web/X.js@*/ lines replaced as build.sh does (the exports dropped)"""
    out = []
    for line in open(PAGE, encoding="utf-8").read().split("\n"):
        m = re.match(r"\s*/\*@include (\S+)@\*/\s*$", line)
        if m:
            src = open(os.path.join(HERE, "..", "..", "..", m.group(1)), encoding="utf-8").read()
            line = re.sub(r"^export ", "", src, flags=re.M)
        out.append(line)
    return "\n".join(out).encode()


# the Wi-Fi settings (wifi.cpp's Settings JSON); only fake networks and passwords
WIFI = {"mode": "off", "chip": False, "on": False, "ssid": None, "open": False, "scans": 0, "scanAt": 0, "scanEnd": 0,
        "join": "none", "reason": "", "t0": 0, "outcome": None, "prev": None, "rebootUntil": 0}
WIFI_TRIAL = float(os.environ.get("MOCK_TRIAL", "6"))
FAKE_NETS = [{"ssid": "Fake-Studio", "rssi": -48, "ch": 6, "band": "2.4", "secured": True},
             {"ssid": "Fake-Studio 5G", "rssi": -55, "ch": 36, "band": "5", "secured": True},
             {"ssid": "Fake-Cafe (open)", "rssi": -71, "ch": 11, "band": "2.4", "secured": False},
             {"ssid": "Fake-Neighbour", "rssi": -83, "ch": 1, "band": "2.4", "secured": True}]


def wifi_tick():
    w, now = WIFI, time.time()
    if w["join"] == "restarting" and now >= w["rebootUntil"]:
        w.update(join="trying", t0=w["rebootUntil"], mode="net", chip=True)
    if w["join"] == "trying" and now - w["t0"] >= WIFI_TRIAL:
        if w["outcome"] is None:
            w.update(join="up")
            log("wifi: join: the new network is up in 5 s (wifi.txt kept, wifi.txt.prev stays as a backup)")
        else:
            prev = w["prev"]
            w.update(join="failed", reason=w["outcome"], ssid=prev["ssid"], open=prev["open"], on=prev["on"],
                     mode="net" if prev["on"] and prev["ssid"] else "scan" if prev["chip"] else "off", chip=prev["chip"])
            log("wifi: join: the new network did not come up (%s): the previous wifi.txt is back, restarting on it" % w["outcome"])


def wifi_status(host):
    w, now = WIFI, time.time()
    wifi_tick()
    net = w["mode"] == "net"
    up = not net or w["join"] in ("up", "none", "failed")
    return {"mode": w["mode"], "chip": w["chip"], "starting": False, "on": w["on"], "net": "wifi" if net else "ethernet",
            "up": up, "ip": "192.168.1.20" if net else "192.168.77.2", "cmdline": None, "ssid": w["ssid"], "open": w["open"],
            "connected": net and up, "scanning": now < w["scanEnd"], "scans": w["scans"],
            "scanAge": int(now - w["scanAt"]) if w["scans"] else -1, "trialSecs": int(WIFI_TRIAL),
            "join": {"state": w["join"], "reason": w["reason"],
                     "left": max(0, int(WIFI_TRIAL - (now - w["t0"]))) if w["join"] == "trying" else 0},
            "networks": FAKE_NETS if w["scans"] and now >= w["scanEnd"] else []}


def not_cross_site(h):   # wifi_conf.h: lenient, for tools that send neither header
    return True if not h.get("Origin") and not h.get("Referer") else same_origin(h)


def same_origin(h):   # web.cpp's FromPage: Host one of ours, Origin (else Referer) that host
    host = h.get("Host", "")
    if host.split(":")[0] not in ("localhost", "127.0.0.1", "sidecar.local"):
        return False
    o, ref = h.get("Origin"), h.get("Referer")
    if o:
        return o == "http://" + host
    return bool(ref) and (ref == "http://" + host or ref.startswith("http://" + host + "/"))


def wifi_post(op, form):
    w = WIFI
    busy = w["join"] in ("restarting", "trying")
    if op == "power":
        if form.get("on") not in ("0", "1"):
            return None
        if busy:
            return "busy: a join is being tried"
        w["on"] = form["on"] == "1"
        if w["on"] and not w["chip"]:
            w.update(chip=True, mode="scan")
        log("wifi: stored on=%s in /.rytm/wifi.txt (the next boot)" % form["on"])
        return ""
    if op == "forget":
        if busy:
            return "busy: a join is being tried"
        w.update(ssid=None, open=False, on=False, join="none", reason="")
        log("wifi: forget: the stored network removed from /.rytm/wifi.txt (and wifi.txt.prev), on=0")
        return ""
    if op == "join":
        if busy:
            return "busy: a join is being tried"
        ssid, psk, op_ = form.get("ssid", ""), form.get("psk", ""), form.get("open") == "1"
        b = ssid.encode()
        if not 1 <= len(b) <= 32 or any(c < 0x20 or c == 0x7f for c in b):
            return "the network name must be 1-32 bytes, no control characters"
        if op_ and psk:
            return "an open network takes no password"
        if not op_ and not (8 <= len(psk) <= 63 and all(0x20 <= ord(c) <= 0x7e for c in psk) or re.fullmatch(r"[0-9a-fA-F]{64}", psk)):
            return 'the password must be 8-63 characters (printable ASCII) or 64 hex digits; an open network needs "open network" ticked'
        names = [n["ssid"] for n in FAKE_NETS]
        out = "wrong_password" if psk == "wrong-pass" else None if ssid in names else "not_found"
        w.update(prev={k: w[k] for k in ("ssid", "open", "on", "chip")}, ssid=ssid, open=op_, on=True, join="restarting",
                 reason="", outcome=out, rebootUntil=time.time() + 3)
        log("wifi: join: a new network stored in /.rytm/wifi.txt (its key, not the passphrase; the previous file kept as "
            "wifi.txt.prev): restarting on Wi-Fi, the previous setting back if it is not up in %d s" % WIFI_TRIAL)
        return ""
    return None


# the volumes and the storage bench (stor_pi.cpp's /status "stor"): an SD card, a USB 3 stick; stor=bench:VOL runs a
# pretend bench (done after ~5 s), stor=eject:USB ejects the stick, stor=mount:USB brings it back
STOR = {"exfat": False, "vols": [
    {"name": "SD", "state": "mounted", "fs": "FAT32", "totalMB": 30436, "freeMB": 24310, "link": "SD HS 4-bit 50 MHz", "model": "", "why": "", "err": 0, "busy": False, "pulls": 0, "mountMs": 0},
    {"name": "USB", "state": "mounted", "fs": "FAT32", "totalMB": 61056, "freeMB": 58982, "link": "USB 3 SuperSpeed", "model": "0781:5581", "why": "", "err": 0, "busy": False, "pulls": 0, "mountMs": 41},
    {"name": "USB2", "state": "absent", "fs": "", "totalMB": 0, "freeMB": 0, "link": "", "model": "", "why": "", "err": 0, "busy": False, "pulls": 0, "mountMs": 0},
    {"name": "USB3", "state": "absent", "fs": "", "totalMB": 0, "freeMB": 0, "link": "", "model": "", "why": "", "err": 0, "busy": False, "pulls": 0, "mountMs": 0},
    {"name": "NVME", "state": "unavailable", "fs": "", "totalMB": 0, "freeMB": 0, "link": "", "model": "", "why": "", "err": 0, "busy": False, "pulls": 0, "mountMs": 0}],
    "bench": {"state": "idle", "progress": 0, "phase": "", "vol": "SD", "have": False}}
STOR_RUN = {"vol": None, "t0": 0}
STOR_TABLE = ("stor bench USB: 256 MB file, FAT32, USB 3 SuperSpeed, calls <= 64 KB\n"
              "(a pretend result from tests/mock_web.py)\nverify ok, 5 s\n")


def stor_command(c):
    v = c.split(":", 1)[1].rstrip("!") if ":" in c else "SD"
    vol = next((x for x in STOR["vols"] if x["name"] == v.split(":")[0].upper()), None)
    if c.startswith("stor=bench") and vol and vol["state"] == "mounted" and STOR["bench"]["state"] != "running":
        STOR_RUN.update(vol=vol["name"], t0=time.time())
    elif c == "stor=cancel" and STOR["bench"]["state"] == "running":
        STOR_RUN["vol"] = None
        STOR["bench"].update(state="cancelled", phase="cancelled", progress=0)
    elif c.startswith("stor=eject:") and vol and vol["name"] != "SD" and vol["state"] == "mounted":
        vol.update(state="ejected", fs="", totalMB=0, freeMB=0)
    elif c.startswith("stor=mount:") and vol and vol["state"] == "ejected":
        vol.update(state="mounted", fs="FAT32", totalMB=61056, freeMB=58982)


def stor_status():
    if STOR_RUN["vol"]:
        p = int((time.time() - STOR_RUN["t0"]) * 20)
        if p >= 100:
            STOR["bench"].update(state="done", progress=100, phase="done", vol=STOR_RUN["vol"], have=True, table=STOR_TABLE)
            STOR_RUN["vol"] = None
        else:
            STOR["bench"].update(state="running", progress=p, phase="sequential write", vol=STOR_RUN["vol"])
    return STOR


def log(m):
    t = time.time() - T0
    LOG.append("%02d:%02d:%05.2f %s\n" % (t // 3600, t // 60 % 60, t % 60, m))


def seed():
    for d in ("Samples/Kit", "Samples/Loops", ".rytm", "overlays", ".Spotlight-V100", ".fseventsd"):
        os.makedirs(os.path.join(ROOT, d), exist_ok=True)
    for f, b in (("config.txt", b"arm_64bit=1\n"), ("config.txt.bak", b"arm_64bit=1\n"), ("config.txt.prev", b"x\n"),
                 ("cmdline.txt", b"x\n"), ("kernel8-rpi4.img", b"\0" * 1000), ("._config.txt", b"\0"),
                 ("Samples/Kit/kick.wav", b"\0" * 4000), ("Samples/Kit/snare.wav", b"\0" * 8000), ("Samples/Kit/tom.wav", b"\0" * 6000),
                 ("Samples/Loops/loop 2.wav", b"\0" * 9000), ("Samples/Loops/loop 1.wav", b"\0" * 9000),
                 ("Samples/Kit/hat open.wav", b"\0" * 3000), ("Samples/Kit/clap.aif", b"\0" * 5000)):
        p = os.path.join(ROOT, f)
        if not os.path.exists(p):
            open(p, "wb").write(b)


# fsweb.cpp's reachable(): /fs/* is content, 7310 is firmware, /.rytm is neither. Deny by default.
FS_CONTENT = ("/Samples",)


def fs_reachable(p):
    if not path_ok(p):
        return False
    if p == "/":
        return True
    if re.match(r"^/(USB|USB2|USB3|NVME)(/.*)?$", p):   # a drive: none is served (fsweb.cpp drive_of)
        return False
    low = p.lower()
    return any(low == c.lower() or low.startswith(c.lower() + "/") for c in FS_CONTENT)


def path_ok(p):
    if not p or len(p) > 200 or p[0] != "/" or "//" in p:
        return False
    if any(ord(c) < 0x20 or ord(c) > 0x7e or c in '\\:*?"<>|' for c in p):
        return False
    return not ("/../" in p or p.endswith("/..") or "/./" in p or p.endswith("/."))


def read_only(p):
    if p in ("", "/"):
        return True
    lp = p.lower()
    if lp == "/overlays" or lp.startswith("/overlays/") or lp.startswith("/system volume information"):
        return True
    n = lp[1:]
    if "/" in n:
        return False
    return n in BOOT or n.startswith(BOOTP) or n.endswith((".dtb", ".elf", ".dat"))


def kept(p):
    return p.lower() in KEPT


def disk(p):
    return os.path.join(ROOT, p.lstrip("/"))


# ---- the pairing code (pi/engine/paircode.h; the reference is pi/engine/tests/test_paircode.cpp)
import hashlib, secrets
PAIR_PATH = os.path.join(ROOT, ".rytm", "pair.txt")
# The device shows the code on the Rytm's screen and on the touchscreen and writes it nowhere. This mock has neither
# screen, so MOCK_PAIR_OUT names a file to write it to - a stand-in for the screen, outside the card, for a test to
# read. Nothing on the card ever holds it (page_pair.test.mjs checks that).
PAIR_OUT = os.environ.get("MOCK_PAIR_OUT", "")
PAIR_LOCK = float(os.environ.get("MOCK_PAIR_LOCK", "60"))
PAIR_GAP = float(os.environ.get("MOCK_PAIR_GAP", "1"))
PAIR_MAX_FAILS, PAIR_CLIENTS, PAIR_SESSIONS = 5, 16, 16
PAIR_SHOW_GAP = 10          # paircode.h kShowGapS (fixed)
# web.cpp's Open(): the whole open set, and nothing else. Everything else needs the pairing code, so a new endpoint
# is gated unless someone deliberately adds it here (page_pair.test.mjs reads this list out of web.cpp and checks the
# two agree, and that nothing else in web.cpp answers an unpaired client).
PAIR_OPEN = {"/", "/index.html", "/health", "/status", "/log", "/pair", "/pair/status", "/pair/resume",
             "/pair/show"}
NOT_PAIRED = ("not paired: enter the pairing code shown on the Rytm's screen")
_mp = os.environ.get("MOCK_PAIR", "")
PAIR = {"mode": "off" if _mp == "0" else "fixed" if re.fullmatch(r"\d{8}", _mp) else "on",
        "code": _mp if re.fullmatch(r"\d{8}", _mp) else "".join(secrets.choice("0123456789") for _ in range(8)),
        "clients": [], "sessions": {}, "peers": {}, "unpaired": 0, "show_at": 0.0, "show_gap": PAIR_SHOW_GAP}


def pair_load():
    try:
        text = open(PAIR_PATH, encoding="utf-8").read()
    except OSError:
        return
    for l in text.splitlines():
        if not l.startswith("c="):
            continue
        f = l[2:].split(" ", 2)
        if len(f) == 3 and re.fullmatch(r"[0-9a-f]{8}", f[0]) and re.fullmatch(r"[0-9a-f]{64}", f[1]) \
           and len(PAIR["clients"]) < PAIR_CLIENTS:
            PAIR["clients"].append({"id": f[0], "hash": f[1], "name": f[2]})


def pair_save():
    os.makedirs(os.path.dirname(PAIR_PATH), exist_ok=True)
    body = ("# Sidecar paired devices (docs/pi-engine.md \"Pairing code\"). The code itself is never\n"
            "# stored here: these are SHA-256 of each device's own secret. Delete this file to unpair all.\nv=1\n")
    for c in PAIR["clients"]:
        body += "c=%s %s %s\n" % (c["id"], c["hash"], c["name"])
    open(PAIR_PATH + ".part", "w", encoding="utf-8").write(body)      # as wifi.txt is written
    if os.path.exists(PAIR_PATH):
        os.replace(PAIR_PATH, PAIR_PATH + ".prev")
    os.replace(PAIR_PATH + ".part", PAIR_PATH)


def pair_peer(peer):
    return PAIR["peers"].setdefault(peer, {"fails": 0, "locks": 0, "lock_until": 0.0, "last_try": 0.0})


def pair_lock_left(peer):
    now, e = time.time(), PAIR["peers"].get(peer)
    if not e:
        return 0
    if e["lock_until"] > now:
        return int(e["lock_until"] - now) + 1
    if e["last_try"] and now - e["last_try"] < PAIR_GAP:
        return 1
    return 0


def pair_fail(peer):
    e = pair_peer(peer)
    e["last_try"] = time.time()
    e["fails"] += 1
    if e["fails"] >= PAIR_MAX_FAILS:
        e["fails"] = 0
        e["lock_until"] = time.time() + PAIR_LOCK * (2 ** min(e["locks"], 4))
        e["locks"] = min(e["locks"] + 1, 4)


def pair_clear(peer):          # a correct code or a resume from this address: its slate, nobody else's
    PAIR["peers"].pop(peer, None)


def pair_session(cid=""):
    if len(PAIR["sessions"]) >= PAIR_SESSIONS:
        del PAIR["sessions"][min(PAIR["sessions"], key=lambda t: PAIR["sessions"][t]["seen"])]
    tok = secrets.token_hex(16)
    PAIR["sessions"][tok] = {"id": cid, "seen": time.time()}
    return tok


def pair_check(tok):
    if PAIR["mode"] == "off":
        return True
    s = PAIR["sessions"].get(tok or "")
    if not s:
        return False
    s["seen"] = time.time()
    return True


def pair_token(h, q, form):
    """web.cpp's Token(): the X-Sidecar-Auth header, else tok= in a POST's body (never the query string).
    MOCK_PAIR_NOHDR=1 is a kernel built without pi/circle/circle-http-auth.patch: the header never reaches it, so
    pairing succeeds and then nothing stays paired (hardware, 2026-10-25)."""
    hdr = "" if os.environ.get("MOCK_PAIR_NOHDR") else (h.get("X-Sidecar-Auth") or "")
    return hdr or form.get("tok") or ""


def pair_do(op, form, tok, peer=""):
    g = PAIR
    if op == "":
        if g["mode"] == "off":
            return {"ok": False, "code": 403, "wait": 0, "error": "the pairing code is off on this card (cmdline.txt paircode=0)"}
        code = form.get("code", "")
        if not re.fullmatch(r"\d{8}", code):      # not a guess: never counted (paircode.h)
            return {"ok": False, "code": 403, "wait": pair_lock_left(peer), "error": "the pairing code is 8 digits"}
        if pair_lock_left(peer):
            return {"ok": False, "code": 403, "wait": pair_lock_left(peer),
                    "error": "too many wrong codes: pairing is locked for a while (the code on the screen still works after it)"}
        if not secrets.compare_digest(code, g["code"]):
            pair_fail(peer)
            return {"ok": False, "code": 403, "wait": pair_lock_left(peer),
                    "error": "that is not the pairing code on the Sidecar's screen"}
        pair_clear(peer)
        PAIR["show_at"], PAIR["show_gap"] = 0.0, PAIR_SHOW_GAP
        name = "".join(c if 0x20 <= ord(c) <= 0x7e else "?" for c in form.get("name", "")[:24]).strip() or "a client"
        remember = form.get("remember", "1") != "0"
        out = {"ok": True, "token": pair_session()}
        if remember:
            sec = secrets.token_hex(16)
            cid = secrets.token_hex(4)
            if len(g["clients"]) >= PAIR_CLIENTS:   # paircode.cpp victim(): a browser before sdboot.py
                browsers = [c for c in g["clients"] if c["name"] != "sdboot.py"]
                g["clients"].remove(min(browsers or g["clients"], key=lambda c: c.get("used", 0)))
            g["clients"].append({"id": cid, "hash": hashlib.sha256(bytes.fromhex(sec)).hexdigest(), "name": name,
                                 "used": time.time()})
            pair_save()
            PAIR["sessions"][out["token"]]["id"] = cid
            out["client"], out["secret"] = cid, sec
        log("pair: a device paired with the code (\"%s\"): %d paired" % (name, len(g["clients"])))
        return out
    if op == "resume":
        # Deliberately outside the code's lockout, in both directions (paircode.h): a 128-bit secret is not
        # guessable, so rate-limiting it protects nothing, and counting it let a browser with a stale entry lock
        # its own owner out - and out of `sdboot.py install`, which shares the limiter.
        if g["mode"] == "off":
            return {"ok": False, "code": 403, "wait": 0, "error": "the pairing code is off on this card (cmdline.txt paircode=0)"}
        stale = {"ok": False, "code": 403, "wait": 0,
                 "error": "the saved pairing is no longer valid on this Sidecar: the code is needed again"}
        cid, sec = form.get("client", ""), form.get("secret", "")
        if not re.fullmatch(r"[0-9a-fA-F]{8}", cid) or not re.fullmatch(r"[0-9a-fA-F]{32}", sec):
            return stale
        want = hashlib.sha256(bytes.fromhex(sec)).hexdigest()
        hit = [c for c in g["clients"] if c["id"] == cid.lower() and secrets.compare_digest(c["hash"], want)]
        if not hit:
            return stale
        hit[0]["used"] = time.time()
        pair_clear(peer)            # the owner is here: this address's lockout goes (nobody else's)
        return {"ok": True, "token": pair_session(cid.lower())}
    if op in ("forget", "revoke"):
        if not pair_check(tok):
            return None
        if op == "revoke":
            g["clients"], g["sessions"] = [], {}
        else:
            cid = g["sessions"].get(tok, {}).get("id", "")
            g["clients"] = [c for c in g["clients"] if c["id"] != cid]
            g["sessions"] = {t: v for t, v in g["sessions"].items() if t != tok and (not cid or v["id"] != cid)}
        pair_save()
        log("pair: %s (%d paired devices)" % ("every device unpaired" if op == "revoke" else "a device unpaired itself",
                                              len(g["clients"])))
        return {"ok": True}
    return None


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send(self, body, ctype="text/plain; charset=utf-8"):
        if isinstance(body, str):
            body = body.encode()
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def code(self, st, msg):
        self.send("%d %s\n" % (st, msg))

    def do_GET(self):
        self.route()

    def do_POST(self):
        self.route()

    def route(self):
        u = urlsplit(self.path)
        # MOCK_REFUSE=0.3: that share of the requests refused as the Pi's connection cap does (web.cpp CreateWorker):
        # half answered 503 + Retry-After, half closed unanswered (the browser's "NetworkError")
        if REFUSE and random.random() < REFUSE and u.path != "/":
            REFUSED[0] += 1
            if random.random() < 0.5:
                self.close_connection = True
                self.wfile.write(b"HTTP/1.0 503 Service Unavailable\r\nRetry-After: 1\r\nContent-Length: 9\r\n\r\n503 busy\n")
            else:
                self.close_connection = True
                try: self.connection.shutdown(2)
                except OSError: pass
            return
        q = {k: v[0] for k, v in parse_qs(u.query, keep_blank_values=True).items()}
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n) if n else b""
        p = u.path
        if time.time() < WIFI["rebootUntil"] and p != "/":       # a join's restart: the Pi is away
            self.close_connection = True
            self.wfile.write(b"HTTP/1.0 503 Service Unavailable\r\nRetry-After: 1\r\nContent-Length: 9\r\n\r\n503 busy\n")
            return
        form = {k: v[0] for k, v in parse_qs(body.decode(errors="replace"), keep_blank_values=True).items()}
        tok = pair_token(self.headers, q, form)
        peer = self.headers.get("X-Mock-Peer") or self.client_address[0]   # paircode.h: the limiter is per address
        if p not in PAIR_OPEN and not pair_check(tok):       # web.cpp: HTTP 200, the code in the body
            PAIR["unpaired"] += 1
            js = not (p.startswith("/fs/") or p == "/cmd")       # web.cpp's WantsText()
            b = (json.dumps({"ok": False, "code": 401, "pair": True, "wait": 0, "error": NOT_PAIRED})
                 if js else "401 %s\n" % NOT_PAIRED).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json" if js else "text/plain; charset=utf-8")
            self.send_header("X-Sidecar-Pair", "required")
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)
            return
        if p == "/pair" or p.startswith("/pair/"):
            op = p[6:] if len(p) > 5 else ""
            j = lambda o: self.send(json.dumps(o), "application/json")
            if op == "status":
                # hdr: this request carried X-Sidecar-Auth (a kernel without circle-http-auth.patch never sees it,
                # which looks exactly like "pairing does not stick"). MOCK_PAIR_NOHDR=1 plays that kernel.
                hdr = bool(self.headers.get("X-Sidecar-Auth")) and not os.environ.get("MOCK_PAIR_NOHDR")
                return j({"gate": PAIR["mode"], "paired": pair_check(tok), "clients": len(PAIR["clients"]),
                          "sessions": len(PAIR["sessions"]), "wait": pair_lock_left(peer), "digits": 8,
                          "hdr": hdr, "rng": True, "host": "sidecar"})
            if op == "show":        # web.cpp: open, lenient same-origin, rate-limited; the code goes to the Rytm only
                if not same_origin(self.headers) and (self.headers.get("Origin") or self.headers.get("Referer")):
                    return self.send_error(400)
                shown, wait = False, 0
                if PAIR["mode"] != "off" and not pair_check(tok):
                    left = PAIR["show_gap"] - (time.time() - PAIR["show_at"]) if PAIR["show_at"] else 0
                    if left <= 0:               # paircode.h ShowGap: a fixed 10 s, reset by a pairing
                        PAIR["show_at"] = time.time()
                        log("pair: the code is on the Rytm's screen")
                        shown = True
                    else:
                        wait = math.ceil(left)  # ShowGap::wait_s: whole seconds left of the gap
                return j({"ok": True, "shown": shown, "wait": wait})
            if op in ("", "resume", "forget", "revoke"):
                if not same_origin(self.headers) and (op in ("forget", "revoke") or self.headers.get("Origin")
                                                      or self.headers.get("Referer")):
                    return j({"ok": False, "code": 403, "error": "refused: not from this page (the same-origin check)"})
                r = pair_do(op, form, tok, peer)
                if r is None:
                    return j({"ok": False, "code": 401, "pair": True, "error": NOT_PAIRED})
                return j(r)
            return self.send_error(404)
        if p.startswith("/wifi/"):
            op = p[6:]
            if op == "status":
                return self.send(json.dumps(wifi_status(self.headers.get("Host"))), "application/json")
            if op == "scan":
                if not WIFI["chip"]:
                    return self.send(json.dumps({"ok": False, "code": 409, "error": "Wi-Fi is off: turn it on to scan"}), "application/json")
                WIFI.update(scans=WIFI["scans"] + 1, scanAt=time.time() + 2, scanEnd=time.time() + 2)
                log("wifi: scan: %d networks in 2100 ms (their names on the Wi-Fi settings only)" % len(FAKE_NETS))
                return self.send(json.dumps({"ok": True}), "application/json")
            if op in ("join", "forget", "power"):
                if not same_origin(self.headers):
                    return self.send(json.dumps({"ok": False, "code": 403, "error": "refused: not from this page (the same-origin check)"}), "application/json")
                form = {k: v[0] for k, v in parse_qs(body.decode(), keep_blank_values=True).items()}   # the body only
                e = wifi_post(op, form)
                if e is None:
                    return self.send_error(400)
                return self.send(json.dumps({"ok": True} if e == "" else {"ok": False, "code": 400, "error": e}), "application/json")
            return self.send_error(404)
        if p in ("/", "/index.html"):           # as web.cpp: gzip, no-cache; ?raw plain
            if "raw" in u.query:
                return self.send(page(), "text/html; charset=utf-8")
            pg = page()
            body = gzip.compress(pg if isinstance(pg, bytes) else pg.encode(), 9, mtime=0)
            self.send_response(200)
            for k, v in (("Content-Type", "text/html; charset=utf-8"), ("Content-Encoding", "gzip"),
                         ("Vary", "Accept-Encoding"), ("Cache-Control", "no-cache"), ("Content-Length", str(len(body)))):
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(body)
            return
        if p == "/health":
            return self.send(json.dumps({
                "alive": True, "version": "0.1.0", "commit": "abc1234", "dirty": False, "uptime": int(time.time() - T0),
                "temp": 57, "streaming": True, "rytmMidi": True, "underruns": 0,
                "pair": PAIR["mode"], "power": POWER}), "application/json")
        if p == "/status":
            return self.send(json.dumps({
                "host": "sidecar", "uptime": int(time.time() - T0), "web": {"requests": 1, "maxus": 900, "commands": 0, "refused": REFUSED[0], "idle": 0, "open": 1},
                "power": POWER,
                "ip": "169.254.61.60", "link": "usb", "gadget": "sidecar", "sidecar": True, "mac": True, "streaming": True,
                "rytmMidi": True, "temp": 57, "fifoTarget": 96, "underruns": 0,
                "version": "0.1.0", "commit": "abc1234", "dirty": False, "build": "Oct 17 2026 12:00:00", "circle": "Circle 50.0", "stor": stor_status(),
                "pair": {"gate": PAIR["mode"], "clients": len(PAIR["clients"]), "sessions": len(PAIR["sessions"]),
                         "rng": True, "locked": 0},
                "wifi": {"mode": WIFI["mode"], "chip": WIFI["chip"], "stored": bool(WIFI["ssid"]), "on": WIFI["on"],
                         "connected": WIFI["join"] == "up", "rejoins": 0, "scanning": False, "scans": WIFI["scans"],
                         "networks": len(FAKE_NETS) if WIFI["scans"] else 0, "join": WIFI["join"]}}), "application/json")
        if p == "/log":
            # the hardware pass 2026-10-23's figures (a busy pattern): the STATUS cards' widest values (fix1049)
            log("engine: generic | cpu c3 10.4 % | pass max 1234 us | midi 0 B (lost 0) sysex 12 notes 0 | to-host sent 900 B 40 msgs fail 0 dropped 0 | 57 C")
            text = "".join(LOG)
            since = int(q.get("since", "0") or 0)
            data = text.encode()[since:]
            return self.send(("%d %d\n" % (since, since + len(data))).encode() + data)
        if p == "/cmd":
            c = parse_qs(body.decode()).get("c", [q.get("c", "")])[0]
            log("web: command :%s" % c)
            if c.startswith("stor"):
                stor_command(c)
            return self.send("ok\n")
        if p.startswith("/fs/"):
            try:
                return self.fs(p[4:], q, body)
            except OSError as e:                 # the engine answers a code; this must not drop the connection
                return self.code(409, "cannot: %s" % e.__class__.__name__)
        self.send_error(404)

    def fs(self, op, q, body):
        # web.cpp: /fs/* had no origin check at all before 0.3.0, so a page on another site could reach it by DNS
        # rebinding. Reads strict (the page sends a Referer on its own GETs), the four writes lenient (a tool).
        if not (same_origin(self.headers) if op in ("ls", "get") else not_cross_site(self.headers)):
            return self.code(403, "refused: not from this page (the same-origin check)")
        path = q.get("path", "") or "/"
        to = q.get("to", "")
        if not fs_reachable(path) or (op == "mv" and not fs_reachable(to)):
            return self.code(404, "no such path (the file browser reaches the content folders only)")
        if op == "ls":
            if not path_ok(path) or not os.path.isdir(disk(path)):
                return self.code(404, "no such folder")
            es = []
            for nm in sorted(os.listdir(disk(path))):
                full = (path if path != "/" else "") + "/" + nm
                if not fs_reachable(full):      # the root shows the content folders, not the boot files or /.rytm
                    continue
                d = os.path.isdir(disk(full))
                es.append({"n": nm, "d": int(d), "s": 0 if d else os.path.getsize(disk(full)),
                           "t": time.strftime("%Y-%m-%d %H:%M", time.localtime(os.path.getmtime(disk(full)))),   # fsweb.h
                           "ro": 1 if read_only(full) else 2 if kept(full) else 0})
            return self.send(json.dumps({"path": path, "free": 30 << 30, "total": 32 << 30, "more": 0, "entries": es}),
                             "application/json")
        if not path_ok(path):
            return self.code(400, "bad path")
        if op == "get":
            off, ln = int(q.get("off", 0)), int(q.get("len", 65536))
            with open(disk(path), "rb") as f:
                f.seek(off)
                return self.send(f.read(min(ln, 65536)), "application/octet-stream")
        if op == "mkdir":
            if read_only(path):
                return self.code(400, "bad path")
            if q.get("p") == "1":
                acc, made = "", 0
                for part in path.strip("/").split("/"):
                    acc += "/" + part
                    if os.path.isfile(disk(acc)):
                        return self.code(409, "a file of that name is in the way")
                    if not os.path.isdir(disk(acc)):
                        os.mkdir(disk(acc))
                        made += 1
                return self.code(200, "made" if made else "exists")
            if os.path.exists(disk(path)):
                return self.code(409, "exists")
            os.mkdir(disk(path))
            return self.code(200, "made")
        if op == "del":
            if read_only(path):
                return self.code(403, "a boot file: read-only here")
            if kept(path):
                return self.code(403, "kept: the Sidecar's own folder (its contents can go)")
            try:
                os.rmdir(disk(path)) if os.path.isdir(disk(path)) else os.remove(disk(path))
            except OSError:
                return self.code(409, "the folder is not empty")
            return self.code(200, "deleted")
        if op == "mv":
            to = q.get("to", "")
            if not path_ok(to):
                return self.code(400, "bad path")
            if read_only(path) or read_only(to):
                return self.code(403, "a boot file: read-only here")
            if kept(path):
                return self.code(403, "kept: the Sidecar's own folder cannot be renamed")
            if os.path.exists(disk(to)):
                return self.code(409, "the new name exists")
            os.rename(disk(path), disk(to))
            return self.code(200, "renamed")
        if op == "put":
            if read_only(path):
                return self.code(403, "a boot file: read-only here (sdfiles installs it)")
            m = re.search(rb"\r\n\r\n", body)
            b = re.search(rb"^--(.+?)\r\n", body)
            data = body[m.end():body.rindex(b"\r\n--" + b.group(1))] if m and b else b""
            off, total, crc = int(q.get("off", 0)), int(q.get("total", 0)), int(q.get("crc", 0))
            if off == 0:
                if not os.path.isdir(os.path.dirname(disk(path))):
                    return self.code(404, "its folder does not exist")
                UP[path] = [0, 0]
                open(disk(path) + ".part", "wb").close()
            u = UP.get(path)
            if not u or u[0] != off:
                return self.code(409, "expected %d" % (u[0] if u else 0))
            with open(disk(path) + ".part", "r+b") as f:
                f.seek(off)
                f.write(data)
            u[0] += len(data)
            u[1] = zlib.crc32(data, u[1])
            time.sleep(0.05)   # a card's pace, so the progress shows
            if u[0] < total:
                return self.code(200, "ok %d" % u[0])
            del UP[path]
            if u[1] != crc:
                os.remove(disk(path) + ".part")
                return self.code(400, "CRC of the upload wrong: not saved (try again)")
            os.replace(disk(path) + ".part", disk(path))
            return self.code(200, "done %d" % total)
        self.send_error(404)


if __name__ == "__main__":
    seed()
    pair_load()
    if PAIR_OUT and PAIR["mode"] != "off":
        open(PAIR_OUT, "w", encoding="utf-8").write("%s %s\n" % (PAIR["code"][:4], PAIR["code"][4:]))
    # this mock has no display, so it writes the code to the card's root as a screenless engine does. Nothing on the
    # network can read it: /fs/* does not reach the root's files at all, and every read needs the code anyway.
    recourse = os.path.join(ROOT, "sidecar-paircode.txt")
    if PAIR["mode"] != "off":
        open(recourse, "w", encoding="utf-8").write("Sidecar pairing code: %s %s\n" % (PAIR["code"][:4], PAIR["code"][4:]))
    elif os.path.exists(recourse):
        os.remove(recourse)
    print("pair: the gate is %s, %d device(s) remembered" % (PAIR["mode"], len(PAIR["clients"])), flush=True)
    print("http://127.0.0.1:%d/  card %s" % (PORT, ROOT), flush=True)
    ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
