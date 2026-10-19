#!/usr/bin/env python3
"""Move top-level C++ definitions from one file to another, verbatim, with plain-text substitutions.

    move_code.py SRC DST --start REGEX [--start REGEX ...] [--sub OLD NEW ...] [--before REGEX] [--keep] [--dry]

Each --start REGEX picks the first line of SRC matching it (re.search). The block runs from that line to the
end of the definition:
- a declaration (a ';' before any '{', the start line or a prototype over several lines) ends at that ';';
- otherwise the line where the braces opened from the start line balance again (string and character literals
  and // comments are ignored when counting).
The '//' comment lines directly above the start line move with it.

It refuses (nothing written) a block that looks like more than one top-level definition: inside the braces a
line at column 0 that is not a '}', '#', '//' or an access label (the next definition, after unbalanced
braces, e.g. under #if/#else), a column-0 '}' that leaves the braces open, or a '}' before the first '{'.

The blocks are cut from SRC (unless --keep) and written into DST in --start order, each after the --sub
replacements (plain text, applied in order). They go before the first line matching --before (default: the
end of DST). Nothing is written when a REGEX matches nothing or two blocks overlap. Prints each block's
first line and length.
"""
import argparse
import re
import sys


def strip(line):
    line = re.sub(r'"(\\.|[^"\\])*"', '""', line)
    line = re.sub(r"'(\\.|[^'\\])'", "''", line)
    return re.sub(r"//.*", "", line)


def refuse(start, j, why):
    sys.stderr.write(f"move_code: WARNING the block at line {start + 1} {why} at line {j + 1}: "
                     "it does not look like one top-level definition; nothing written\n")
    sys.exit(1)


def block(lines, start):
    i = start
    while i > 0 and lines[i - 1].lstrip().startswith("//") and not lines[i - 1].startswith(" "):
        i -= 1
    depth, opened = 0, False
    for j in range(start, len(lines)):
        c = strip(lines[j])
        if not opened:
            semi, brace, close = c.find(";"), c.find("{"), c.find("}")
            if close >= 0 and (brace < 0 or close < brace):
                refuse(start, j, "meets a '}' before its first '{'")
            if semi >= 0 and (brace < 0 or semi < brace):
                return i, j   # a declaration: up to its ';'
        elif j > start and depth > 0 and c[:1] not in ("", " ", "\t", "}", "#") \
                and not re.match(r"(public|private|protected)\s*:", c):
            refuse(start, j, f"still has a '{{' open at column-0 text {lines[j].strip()[:60]!r}")
        depth += c.count("{") - c.count("}")
        opened = opened or "{" in c
        if opened and depth == 0:
            return i, j
        if opened and c.startswith("}"):
            refuse(start, j, "leaves its braces open after a column-0 '}'")
    sys.exit(f"move_code: no end for the block at line {start + 1}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--start", action="append", required=True)
    ap.add_argument("--sub", nargs=2, action="append", default=[])
    ap.add_argument("--before")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--dry", action="store_true")
    a = ap.parse_args()
    src = open(a.src).read().split("\n")
    spans = []
    for rx in a.start:
        hit = next((k for k, l in enumerate(src) if re.search(rx, l)), None)
        if hit is None:
            sys.exit(f"move_code: no line of {a.src} matches {rx!r}")
        s, e = block(src, hit)
        for s2, e2 in spans:
            if not (e < s2 or s > e2):
                sys.exit(f"move_code: {rx!r} overlaps another block (lines {s + 1}-{e + 1})")
        spans.append((s, e))
        print(f"  {a.src}:{s + 1}-{e + 1} ({e - s + 1} lines) {src[hit].strip()[:80]}")
    text = []
    for s, e in spans:
        t = "\n".join(src[s:e + 1])
        for old, new in a.sub:
            t = t.replace(old, new)
        text.append(t)
    if a.dry:
        return
    dst = open(a.dst).read().split("\n") if __import__("os").path.exists(a.dst) else []
    at = len(dst)
    if a.before:
        at = next((k for k, l in enumerate(dst) if re.search(a.before, l)), None)
        if at is None:
            sys.exit(f"move_code: no line of {a.dst} matches --before {a.before!r}")
    out = dst[:at] + "\n\n".join(text).split("\n") + [""] + dst[at:]
    open(a.dst, "w").write("\n".join(out))
    if not a.keep:
        for s, e in sorted(spans, reverse=True):
            del src[s:e + 1]
        open(a.src, "w").write("\n".join(src))


if __name__ == "__main__":
    main()
