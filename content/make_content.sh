#!/bin/bash
# content/make_content.sh - the starter content: a zip with the Sidecar's SD-card folders, to extract to the root of a
# Sidecar's or a Sidecar Mini's card. Rebuilt from this tree's own sources, so it is reproducible:
#   - /JUNO/Factory/: the Juno-106's 128 factory patches as 24-byte patch dumps (F0 41 30 00 <n> <18 bytes> F7), from
#     sim/juno/juno_patches.h (generated from shorepine/amy amy/juno.py, MIT);
#   - /DX7/: the three 32-voice DX7 banks the 6-OP FM engine plays from ROM, as standard 4104-byte packed bank dumps
#     (F0 43 00 09 20 00 <4096> <checksum> F7), from Plaits' resources.cc (Mutable Instruments eurorack, MIT; fetched
#     by sim/plaits/build.sh, which this script runs when it is missing);
#   - README.txt and NOTICE.txt (the templates next to this script).
#   content/make_content.sh [VERSION]
# Writes build/content/sidecar-content-<VERSION>/ (the tree), build/content/sidecar-content-<VERSION>.zip (stored,
# fixed dates) and build/content/SHA256SUMS. VERSION defaults to the VERSION file. The same generator as the release's
# scripts/package_content.sh (sidecar-expander), run in place.
set -euo pipefail
co=$(cd "$(dirname "$0")/.." && pwd)
ver=${1:-$(cat "$co/VERSION" 2>/dev/null || echo 0.1.0)}
juno="$co/sim/juno/juno_patches.h"
res="$co/sim/plaits/vendor/eurorack/plaits/resources.cc"
[ -f "$res" ] || res="$co/teensy/libraries/Plaits/src/plaits/resources.cc"
[ -f "$res" ] || { "$co/sim/plaits/build.sh" >/dev/null; res="$co/sim/plaits/vendor/eurorack/plaits/resources.cc"; }
[ -f "$juno" ] && [ -f "$res" ] || { echo "need $juno and Plaits' resources.cc (sim/plaits/build.sh fetches it)"; exit 1; }
out="$co/build/content"
mkdir -p "$out"
python3 - "$juno" "$res" "$out" "$ver" "$co/content" "$(git -C "$co" rev-parse HEAD 2>/dev/null || echo unknown)" <<'PY'
import hashlib, os, re, shutil, sys, zipfile
juno, res, out, ver, tmpl, commit = sys.argv[1:]
root = os.path.join(out, f"sidecar-content-{ver}")
shutil.rmtree(root, ignore_errors=True)

# JUNO: {"A11 Brass Set 1", {18 bytes}, gain}
pat = re.findall(r'\{"([^"]+)", \{([0-9, ]+)\}, [0-9.]+f\}', open(juno).read())
assert len(pat) == 128, len(pat)
d = os.path.join(root, "JUNO", "Factory"); os.makedirs(d)
for i, (name, b) in enumerate(pat):
    data = [int(x) for x in b.split(",")]
    assert len(data) == 18 and all(0 <= x < 128 for x in data)
    safe = re.sub(r'[\\/:*?"<>|]', "-", name).strip()
    open(os.path.join(d, f"{i + 1:03d} {safe}.syx"), "wb").write(bytes([0xF0, 0x41, 0x30, 0x00, i] + data + [0xF7]))

# DX7: const uint8_t syx_bank_N[] = { ... };
src = open(res).read()
d = os.path.join(root, "DX7"); os.makedirs(d)
for k in range(3):
    m = re.search(r'const uint8_t syx_bank_%d\[\] = \{(.*?)\};' % k, src, re.S)
    data = [int(x) for x in re.findall(r'\d+', m.group(1))]
    assert len(data) == 4096 and all(0 <= x < 128 for x in data), len(data)
    cs = (-sum(data)) & 0x7F
    open(os.path.join(d, f"Plaits FM bank {k + 1}.syx"), "wb").write(bytes([0xF0, 0x43, 0x00, 0x09, 0x20, 0x00] + data + [cs, 0xF7]))

for f in ("README.txt", "NOTICE.txt"):
    t = open(os.path.join(tmpl, f)).read().replace("@VERSION@", ver).replace("@COMMIT@", commit[:7])
    open(os.path.join(root, f), "w").write(t)

z = os.path.join(out, f"sidecar-content-{ver}.zip")
with zipfile.ZipFile(z, "w", zipfile.ZIP_STORED) as zf:
    for dp, dns, fns in os.walk(root):
        dns.sort()
        for fn in sorted(fns):
            p = os.path.join(dp, fn)
            zi = zipfile.ZipInfo(os.path.relpath(p, root), date_time=(2026, 1, 1, 0, 0, 0))
            zi.external_attr = 0o644 << 16
            zf.writestr(zi, open(p, "rb").read())
h = hashlib.sha256(open(z, "rb").read()).hexdigest()
open(os.path.join(out, "SHA256SUMS"), "w").write(f"{h}  {os.path.basename(z)}\n")
n = sum(len(f) for _, _, f in os.walk(root))
print(f"{z}: {os.path.getsize(z)} B, {n} files, sha256 {h}")
PY
