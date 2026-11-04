#!/usr/bin/env python3
"""pi/ui/host/check_bounds.py DIR1024 DIR1280 - the 1280x720 layouts against the 1024x600 ones, from shot's .bounds files
(every shown object of every page: path, box in screen pixels, clickable). 1280x720 fails when
- a page's objects differ from 1024x600's (not the same page),
- a shown object leaves the screen,
- a page does not use the screen: the objects' union must reach within 4 % of every edge the 1024x600 page reaches (nothing
  left in the 1024x600 corner),
- an object sits away from its 1024x600 place, by more than 12 % of the screen (a control lost, not moved with the layout),
- a touch target is not bigger than at 1024x600: each side at least 1.12 x its size there, or already 60 px,
- two clickable siblings overlap (one inside the other is a hit area on a panel, and a dialog of a quarter of the screen sits on top: neither an overlap).
The PROJECT pages build as many list rows as fit, so their objects are not compared with 1024x600's (the rest still is)."""
import glob, os, sys

W0, H0, W1, H1 = 1024, 600, 1280, 720


def load(p):
    o = {}
    for ln in open(p):
        k, x1, y1, x2, y2, c = ln.split()
        o[k] = (int(x1), int(y1), int(x2), int(y2), int(c))
    return o


def main():
    a, b = sys.argv[1], sys.argv[2]
    names = sorted(os.path.basename(p) for p in glob.glob(os.path.join(b, "*.bounds")))
    fails = {}
    def fail(kind, page, what):
        fails.setdefault(kind, []).append("%s %s" % (page, what))
    npages = nobj = 0
    for n in names:
        page = n[:-7]
        pa = os.path.join(a, n)
        if not os.path.exists(pa):
            fail("page missing at 1024x600", page, "")
            continue
        o0, o1 = load(pa), load(os.path.join(b, n))
        npages += 1
        nobj += len(o1)
        rows = "-project" in page
        if set(o0) != set(o1) and not rows:
            d = sorted(set(o0) ^ set(o1))
            fail("objects differ", page, "%d: %s" % (len(d), " ".join(d[:4])))
        for k, (x1, y1, x2, y2, c) in o1.items():
            if x2 < x1 or y2 < y1:
                continue
            if x1 < 0 or y1 < 0 or x2 >= W1 or y2 >= H1:
                fail("off the screen", page, "%s %d,%d..%d,%d" % (k, x1, y1, x2, y2))
        def extent(o, W, H):
            b = [v for k, v in o.items() if k.count(".") >= 1 and v[2] - v[0] < W - 1 or k.count(".") >= 1 and v[3] - v[1] < H - 1]
            return min(v[0] for v in b), min(v[1] for v in b), max(v[2] for v in b), max(v[3] for v in b)
        e0, e1 = extent(o0, W0, H0), extent(o1, W1, H1)
        if e1[2] < (e0[2] + 1) * W1 / W0 - W1 * 0.04 or e1[3] < (e0[3] + 1) * H1 / H0 - H1 * 0.04:
            fail("does not fill the screen", page, "%s of %s" % (e1, e0))
        for k in set() if rows else set(o0) & set(o1):
            v0, v1 = o0[k], o1[k]
            if v1[2] < v1[0] or v0[2] < v0[0]:
                continue
            for i, (S0, S1) in enumerate(((W0, W1), (H0, H1))):
                c0 = (v0[i] + v0[i + 2]) / 2 / S0
                c1 = (v1[i] + v1[i + 2]) / 2 / S1
                if abs(c0 - c1) > 0.12:
                    fail("moved", page, "%s %s -> %s" % (k, v0[:4], v1[:4]))
                    break
            if v1[4]:
                w0, h0, w1, h1 = v0[2] - v0[0] + 1, v0[3] - v0[1] + 1, v1[2] - v1[0] + 1, v1[3] - v1[1] + 1
                if (w1 < w0 * 1.12 and w1 < 60) or (h1 < h0 * 1.12 and h1 < 60):
                    fail("touch target", page, "%s %dx%d -> %dx%d" % (k, w0, h0, w1, h1))
        kids = {}
        for k, v in o1.items():
            if v[4] and v[2] >= v[0] and "." in k:
                kids.setdefault(k.rsplit(".", 1)[0], []).append((k, v))
        for p, l in kids.items():
            for i in range(len(l)):
                for j in range(i + 1, len(l)):
                    u, v = l[i][1], l[j][1]
                    inside = (u[0] <= v[0] and u[1] <= v[1] and u[2] >= v[2] and u[3] >= v[3]) or (v[0] <= u[0] and v[1] <= u[1] and v[2] >= u[2] and v[3] >= u[3])
                    modal = max((u[2] - u[0]) * (u[3] - u[1]), (v[2] - v[0]) * (v[3] - v[1])) >= W1 * H1 / 4	# a dialog takes the screen's taps
                    if u[0] <= v[2] and v[0] <= u[2] and u[1] <= v[3] and v[1] <= u[3] and not inside and not modal:
                        fail("overlap", page, "%s %s / %s %s" % (l[i][0], u[:4], l[j][0], v[:4]))
    for k, l in sorted(fails.items()):
        print("%s: %d, e.g." % (k, len(l)))
        for x in l[:int(os.environ.get("SHOW", 6))]:
            print("   ", x)
    print("check_bounds: %d pages, %d objects, %d failures" % (npages, nobj, sum(len(l) for l in fails.values())))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
