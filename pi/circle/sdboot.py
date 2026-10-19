#!/usr/bin/env python3
"""The Pi engine's SD card boot files, over the running engine's TCP port 7310 (pi/engine/sdfiles.h).

docs/pi-engine.md, "Boot from the SD card". Every write goes to <name>.new, is read back and checked on the Pi, then
put in place; the replaced file is kept as <name>.prev, and config.txt/cmdline.txt keep their original as <name>.bak.

  sdboot.py ls                         the card's root
  sdboot.py get NAME [OUT]             a file (to stdout, or OUT)
  sdboot.py scratch                    the write path's test (64 KB written, read back, deleted)
  sdboot.py install [IMG]              IMG (default pi/engine/kernel8-rpi4.img) -> engine.img (header + image)
  sdboot.py previous                   engine.img.prev back as engine.img (the two swap: run it again to undo)
  sdboot.py bootloader [IMG]           IMG (default pi/circle/out/sdboot.img, build-sdboot.sh) -> sdboot-try.img
  sdboot.py tryboot                    tryboot.txt = config.txt + the sdboot lines (kernel=sdboot-try.img if it is on
                                       the card, else sdboot.img), then a one-shot tryboot reboot (the next reboot
                                       or power cycle boots config.txt as before)
  sdboot.py commit                     sdboot-try.img -> sdboot.img (after a tryboot that worked)
  sdboot.py enable | disable           add / remove the sdboot lines at the end of config.txt
  sdboot.py restore-config             config.txt.bak (the original) back as config.txt
  sdboot.py cmdline [LINE]             show cmdline.txt, or replace it with LINE (the original kept as cmdline.txt.bak)
  sdboot.py reboot                     the engine stops and reboots
  sdboot.py usbreset                   a USB reset of the FT232R serial adapter (0403:6001; needs Homebrew's libusb):
                                       after a flasher killed mid-upload the Mac's writes to it can hang (measured
                                       2026-10-16: the Pi then hears nothing, not even the reboot magic)
The link: TCP to SIDECAR_HOST (default: PI_IP, else sidecar.local) port 7310; if that does not connect within 3 s
(or SIDECAR_LINK=serial), the serial port (SIDECAR_SERIAL, default /dev/cu.usbserial-*, 921600; nothing else may have
it open): the console's ":sdf", then the same commands, data as base64 lines (about 50 KB/s, a 1.5 MB engine in ~35 s).
Run as: uv run --with pyserial python pi/circle/sdboot.py ...
"""
import base64, binascii, glob, os, socket, struct, sys, time

PORT = 7310
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
ENGINE_ADDR = 0x02000000          # sdboot.patch's SDB_ADDR
BEGIN = "# sidecar sdboot begin (pi/circle/sdboot.py disable removes this block; config.txt.bak is the original)"
END = "# sidecar sdboot end"
def block(kernel="sdboot.img"):
    # disable_splash: no rainbow square; boot_delay=0: the firmware does not wait its default 1 s before the kernel
    return (f"{BEGIN}\n[all]\nkernel={kernel}\ninitramfs engine.img 0x{ENGINE_ADDR:08x}\n"
            f"disable_splash=1\nboot_delay=0\n{END}\n")


class Pi:
    def __init__(self):
        host = os.environ.get("SIDECAR_HOST") or os.environ.get("PI_IP") or "sidecar.local"
        self.ser = None
        if os.environ.get("SIDECAR_LINK") != "serial":
            try:
                self.s = socket.create_connection((host, PORT), timeout=3)
                self.s.settimeout(30)
                self.f = self.s.makefile("rb")
                print(f"link: tcp {host}:{PORT}", file=sys.stderr)
                return
            except OSError as e:
                print(f"link: tcp {host}:{PORT} failed ({e}), the serial port", file=sys.stderr)
        import serial
        port = os.environ.get("SIDECAR_SERIAL") or (glob.glob("/dev/cu.usbserial-*") or [None])[0]
        if not port:
            raise SystemExit("no serial adapter")
        self.ser = serial.Serial(port, 921600, timeout=0.5, write_timeout=5)
        self.ser.reset_input_buffer()
        self.ser.write(b"\n:sdf\n")
        t = time.time()
        while time.time() - t < 5:
            l = self.ser.readline().decode(errors="replace").strip()
            if l.startswith("SDF READY"):
                print(f"link: serial {port}", file=sys.stderr)
                return
        raise SystemExit("the engine did not answer :sdf on the serial port (not running? an older build?)")

    def close(self):
        if self.ser:
            self.ser.write(b"BYE\n")
            t = time.time()
            while time.time() - t < 3:
                if self.ser.readline().startswith(b"SDF BYE"):
                    break

    def send(self, b):
        if self.ser:
            self.ser.write(b)
        else:
            self.s.sendall(b)

    def line(self, timeout=30):
        if not self.ser:
            l = self.f.readline().decode(errors="replace").strip()
            if not l:
                raise SystemExit("the Pi closed the connection")
            return l
        t = time.time()
        while time.time() - t < timeout:      # the log's lines pass by; ours start "SDF "
            l = self.ser.readline().decode(errors="replace").strip()
            if l.startswith("SDF "):
                return l[4:]
        raise SystemExit("no answer on the serial port")

    def cmd(self, c, data=None):
        self.send(c.encode() + b"\n")
        if data is not None:
            if not self.ser:
                self.s.sendall(data)
            else:
                t0 = time.time()
                for i in range(0, len(data), 1024):
                    self.ser.write(b"D " + base64.b64encode(data[i:i + 1024]) + b"\n")
                    r = self.line(10)
                    if r != "K":
                        return r
                    if i % (64 * 1024) == 0 and len(data) > 64 * 1024:
                        print(f"  {i // 1024} / {len(data) // 1024} KB, {time.time() - t0:.0f} s", file=sys.stderr)
        return self.line()

    def ls(self):
        r = self.cmd("LS")
        if not r.startswith("OK"):
            raise SystemExit(r)
        return [self.line() for _ in range(int(r.split()[1]))]

    def get(self, name, missing_ok=False):
        r = self.cmd(f"GET {name}")
        if not r.startswith("OK"):
            if missing_ok:
                return None
            raise SystemExit(f"GET {name}: {r}")
        size, crc = int(r.split()[1]), int(r.split()[2], 16)
        b = b""
        while len(b) < size:
            if self.ser:
                l = self.line(10)
                if not l.startswith("D "):
                    raise SystemExit(f"GET {name}: unexpected {l!r}")
                b += base64.b64decode(l[2:])
                continue
            k = self.f.read(size - len(b))
            if not k:
                raise SystemExit("short read")
            b += k
        if binascii.crc32(b) != crc:
            raise SystemExit(f"GET {name}: crc mismatch")
        return b

    def put(self, name, data):
        r = self.cmd(f"PUT {name} {len(data)} {binascii.crc32(data):08x}", data)
        print(f"PUT {name} ({len(data)} bytes): {r}")
        if not r.startswith("OK"):
            raise SystemExit(1)


def wrap(img):
    img += b"\0" * (-len(img) % 4)
    s1 = s2 = 0
    for (w,) in struct.iter_unpack("<I", img):
        s1 = (s1 + w) & 0xFFFFFFFF
        s2 = (s2 + s1) & 0xFFFFFFFF
    return b"SDCRENG1" + struct.pack("<IIIIII", len(img), s1, s2, 0, 0, 0) + img


def without_block(cfg):
    out, skip = [], False
    for l in cfg.splitlines(keepends=True):
        if l.startswith(BEGIN):
            skip = True
        elif skip and l.startswith(END):
            skip = False
        elif not skip:
            out.append(l)
    return "".join(out)


def with_block(cfg, kernel="sdboot.img"):
    cfg = without_block(cfg)
    if cfg and not cfg.endswith("\n"):
        cfg += "\n"
    return cfg + block(kernel)


def names(pi):
    return {l.split(" ", 1)[1] for l in pi.ls()}


def need(pi, names):
    have = {l.split(" ", 1)[1] for l in pi.ls()}
    miss = [n for n in names if n not in have]
    if miss:
        raise SystemExit(f"missing on the card: {', '.join(miss)} (sdboot.py install / bootloader + tryboot + commit first)")


def usbreset():
    import ctypes.util
    import usb.backend.libusb1, usb.core
    lib = next((p for p in ("/opt/homebrew/lib/libusb-1.0.dylib", "/usr/local/lib/libusb-1.0.dylib",
                            ctypes.util.find_library("usb-1.0")) if p and os.path.exists(p)), None)
    be = usb.backend.libusb1.get_backend(find_library=lambda _: lib)
    d = usb.core.find(idVendor=0x0403, idProduct=0x6001, backend=be)
    if d is None:
        raise SystemExit("no FT232R (0403:6001)")
    d.reset()
    time.sleep(2)
    print("usbreset: the FT232R was reset")


def main(a):
    if not a or a[0] in ("-h", "--help"):
        print(__doc__)
        return
    if a[0] == "usbreset":
        usbreset()
        return
    c, pi = a[0], Pi()
    try:
        run(c, a, pi)
    finally:
        pi.close()


def run(c, a, pi):
    if c == "ls":
        print("\n".join(pi.ls()))
    elif c == "get":
        b = pi.get(a[1])
        if len(a) > 2:
            open(a[2], "wb").write(b)
        else:
            sys.stdout.write(b.decode(errors="replace"))
    elif c == "scratch":
        print(pi.cmd("SCRATCH"))
    elif c == "install":
        p = a[1] if len(a) > 1 else os.path.join(REPO, "pi/engine/kernel8-rpi4.img")
        pi.put("engine.img", wrap(open(p, "rb").read()))
    elif c == "previous":
        pi.put("engine.img", pi.get("engine.img.prev"))
    elif c == "bootloader":
        p = a[1] if len(a) > 1 else os.path.join(HERE, "out/sdboot.img")
        pi.put("sdboot-try.img", open(p, "rb").read())
    elif c == "tryboot":
        k = "sdboot-try.img" if "sdboot-try.img" in names(pi) else "sdboot.img"
        need(pi, [k, "engine.img"])
        cfg = pi.get("config.txt").decode()
        pi.put("tryboot.txt", with_block(cfg, k).encode())
        print(f"tryboot with {k}:", pi.cmd("TRYBOOT"))
    elif c == "commit":
        need(pi, ["sdboot-try.img"])
        pi.put("sdboot.img", pi.get("sdboot-try.img"))
        print("DEL sdboot-try.img:", pi.cmd("DEL sdboot-try.img"))
    elif c == "enable":
        need(pi, ["sdboot.img", "engine.img"])
        cfg = pi.get("config.txt").decode()
        new = with_block(cfg)
        if new == cfg:
            print("config.txt already has the sdboot lines")
        else:
            pi.put("config.txt", new.encode())
    elif c == "disable":
        cfg = pi.get("config.txt").decode()
        new = without_block(cfg)
        if new == cfg:
            print("config.txt has no sdboot lines")
        else:
            pi.put("config.txt", new.encode())
    elif c == "restore-config":
        pi.put("config.txt", pi.get("config.txt.bak"))
    elif c == "cmdline":
        if len(a) > 1:
            pi.put("cmdline.txt", (" ".join(a[1:]).strip() + "\n").encode())
        else:
            b = pi.get("cmdline.txt", missing_ok=True)
            print(b.decode().strip() if b is not None else "(no cmdline.txt)")
    elif c == "reboot":
        print(pi.cmd("REBOOT"))
    else:
        raise SystemExit(f"unknown command {c} (sdboot.py --help)")


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except Exception as e:
        if type(e).__name__ == "SerialTimeoutException":
            print("the serial adapter does not send (a flasher killed mid-upload?): sdboot.py usbreset", file=sys.stderr)
            os._exit(2)             # a close would wait for the stuck output
        raise
