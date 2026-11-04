#!/usr/bin/env python3
"""pi/circle/bootimg.py: edit a Sidecar rpiboot ramdisk boot.img (FAT32) in place, with only Python (no mtools).

    python bootimg.py boot.img                                   # print cmdline.txt and engine.img's size and sha256
    python bootimg.py boot.img --cmdline "sdport=0 usbmidi=os:on usbreboot=1"
    python bootimg.py boot.img --engine engine.img               # an engine.img already wrapped (SDCRENG1 header)
    python bootimg.py boot.img --kernel kernel8-rpi4.img         # wraps it (sdboot.wrap), then as --engine

A file is rewritten along its own cluster chain, so the new one must fit in the clusters the old one has (the stock
ramdisk's engine.img is 4.9 MB, its cmdline.txt one cluster); a shorter one gives its spare clusters back.
"""
import hashlib, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sdboot import wrap  # noqa: E402


class Fat:
    def __init__(self, f):
        self.f = f
        bs = f.read(512)
        self.bps, spc = struct.unpack_from("<HB", bs, 11)
        self.rsvd, self.nfats = struct.unpack_from("<HB", bs, 14)
        self.fatsz = fatsz = struct.unpack_from("<I", bs, 36)[0]
        self.root, self.fsinfo = struct.unpack_from("<IH", bs, 44)
        self.clus = self.bps * spc
        self.data0 = (self.rsvd + self.nfats * fatsz) * self.bps

    def off(self, c):
        return self.data0 + (c - 2) * self.clus

    def chain(self, c):
        out = []
        while 2 <= c < 0x0FFFFFF8:
            out.append(c)
            self.f.seek(self.rsvd * self.bps + c * 4)
            c = struct.unpack("<I", self.f.read(4))[0] & 0x0FFFFFFF
        return out

    def set_fat(self, c, v):
        for k in range(self.nfats):                     # every copy of the FAT
            at = (self.rsvd + k * self.fatsz) * self.bps + c * 4
            self.f.seek(at); old = struct.unpack("<I", self.f.read(4))[0]
            self.f.seek(at); self.f.write(struct.pack("<I", (old & 0xF0000000) | v))

    def find(self, name83):
        for c in self.chain(self.root):
            self.f.seek(self.off(c)); d = self.f.read(self.clus)
            for i in range(0, self.clus, 32):
                e = d[i:i + 32]
                if e[0] == 0: return None
                if e[0] != 0xE5 and e[11] != 0x0F and e[:11] == name83:
                    first = struct.unpack_from("<H", e, 20)[0] << 16 | struct.unpack_from("<H", e, 26)[0]
                    return self.off(c) + i, first, struct.unpack_from("<I", e, 28)[0]
        return None

    def read(self, name83):
        ent, first, size = self.find(name83)
        b = b"".join((self.f.seek(self.off(c)), self.f.read(self.clus))[1] for c in self.chain(first))
        return b[:size]

    def write(self, name83, data):
        hit = self.find(name83)
        if hit is None: sys.exit(f"no {name83!r} in the root directory")
        ent, first, size = hit
        ch = self.chain(first)
        if len(data) > len(ch) * self.clus:
            sys.exit(f"{name83!r}: {len(data)} bytes do not fit in its {len(ch)} clusters")
        need = max(1, -(-len(data) // self.clus))
        for k, c in enumerate(ch[:need]):
            part = data[k * self.clus:(k + 1) * self.clus]
            self.f.seek(self.off(c)); self.f.write(part + b"\0" * (self.clus - len(part)))
        if need < len(ch):                              # a shorter file: end its chain there, free the rest
            self.set_fat(ch[need - 1], 0x0FFFFFFF)
            for c in ch[need:]: self.set_fat(c, 0)
            if self.fsinfo not in (0, 0xFFFF):          # FSInfo's free count: unknown now (fsck and FAT drivers recount)
                self.f.seek(self.fsinfo * self.bps + 488); self.f.write(struct.pack("<I", 0xFFFFFFFF))
        self.f.seek(ent + 28); self.f.write(struct.pack("<I", len(data)))


def main():
    a = sys.argv[1:]
    if not a: sys.exit(__doc__)
    with open(a[0], "r+b") as f:
        fat = Fat(f)
        i = 1
        while i < len(a):
            opt, val = a[i], a[i + 1]
            if opt == "--cmdline":
                fat.write(b"CMDLINE TXT", (val.strip() + "\n").encode())
            elif opt in ("--engine", "--kernel"):
                data = open(val, "rb").read()
                if opt == "--kernel": data = wrap(data)
                if data[:8] != b"SDCRENG1": sys.exit("not a wrapped engine.img (use --kernel for a kernel8-rpi4.img)")
                fat.write(b"ENGINE  IMG", data)
            else:
                sys.exit(f"unknown option {opt}")
            i += 2
        print("cmdline.txt:", fat.read(b"CMDLINE TXT").decode().strip())
        e = fat.read(b"ENGINE  IMG")
        print("engine.img:", len(e), "bytes, sha256", hashlib.sha256(e).hexdigest())


if __name__ == "__main__":
    main()
