"""pi/engine/tests/pi5_flags_src.py ENGINE_DIR - test_pi5_flags.sh's source check: which lines of pi/engine's
sources the compiler sees for a Pi 5 build, by its #if lines, without a compiler.

Tracked macros: RASPPI, SIDECAR_NO_GADGET, SIDECAR_PI5_TESTMODE. A condition that names anything else counts as
"maybe" (both of its branches are seen), so a test-mode word under such a condition is reported, never missed.
Comments are dropped first (the binary has none of them)."""
import re
import sys
from pathlib import Path

TRACKED = {"RASPPI", "SIDECAR_NO_GADGET", "SIDECAR_PI5_TESTMODE"}
# the engine test mode's names: none of them may reach a release kernel's compile (card_guard.* is not linked there:
# the Makefile check)
TEST_WORDS = re.compile(r"testsecs|usbtest|TestLog|g_bCardOff|g_bCardGuard|m_nTestSecs|m_bTestCard|UsbTestTick|testcard")
SKIP = {"card_guard.cpp", "card_guard.h"}


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    out = []
    for line in text.split("\n"):
        # a // outside a string literal (good enough for these sources: no // inside their strings before a comment)
        i, q = 0, None
        while i < len(line):
            c = line[i]
            if q:
                if c == "\\":
                    i += 1
                elif c == q:
                    q = None
            elif c in "\"'":
                q = c
            elif line.startswith("//", i):
                line = line[:i]
                break
            i += 1
        out.append(line)
    return out


def evaluate(cond, macros):
    """True, False or None (maybe) for an #if expression."""
    names = set(re.findall(r"\b[A-Za-z_]\w*\b", cond)) - {"defined"}
    if names - TRACKED:
        return None
    e = re.sub(r"defined\s*\(\s*(\w+)\s*\)|defined\s+(\w+)",
               lambda m: "1" if (m.group(1) or m.group(2)) in macros else "0", cond)
    e = re.sub(r"\b[A-Za-z_]\w*\b", lambda m: str(macros.get(m.group(0), 0)), e)
    e = e.replace("&&", " and ").replace("||", " or ")
    e = re.sub(r"!(?!=)", " not ", e)
    return bool(eval(e, {"__builtins__": {}}))


def visible(lines, macros):
    """The line numbers the compiler may see (a stack of [active, any branch taken, maybe])."""
    stack, seen = [], []
    for n, line in enumerate(lines, 1):
        m = re.match(r"\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", line)
        if m:
            d, rest = m.group(1), m.group(2).strip()
            parent = all(s[0] for s in stack)
            if d in ("if", "ifdef", "ifndef"):
                v = evaluate(rest if d == "if" else ("defined (%s)" % rest if d == "ifdef" else "!defined (%s)" % rest), macros)
                stack.append([v is not False, v is True, v is None])
            elif d == "elif":
                top = stack[-1]
                v = evaluate(rest, macros)
                if top[2] or v is None:
                    top[:] = [not top[1], top[1] or v is True, True]
                else:
                    top[:] = [not top[1] and v, top[1] or v, False]
            elif d == "else":
                top = stack[-1]
                top[0] = not top[1] or top[2]
                top[1] = True
            else:
                stack.pop()
            continue
        if all(s[0] for s in stack):
            seen.append(n)
    return seen


def scan(eng, macros):
    words, gadget = [], False
    for p in sorted(list(eng.glob("*.cpp")) + list(eng.glob("*.h"))):
        if p.name in SKIP:
            continue
        lines = strip_comments(p.read_text(errors="replace"))
        for n in visible(lines, macros):
            if TEST_WORDS.search(lines[n - 1]):
                words.append("%s:%d: %s" % (p.name, n, lines[n - 1].strip()))
            if p.name == "kernel.cpp" and "new CSidecarGadget (&m_Interrupt)" in lines[n - 1]:
                gadget = True
    return words, gadget


def main():
    eng = Path(sys.argv[1])
    release = {"RASPPI": 5}
    test = {"RASPPI": 5, "SIDECAR_PI5_TESTMODE": 1}
    nogadget = {"RASPPI": 5, "SIDECAR_NO_GADGET": 1}
    words, gadget = scan(eng, release)
    if words:
        sys.exit("pi5_flags_src: FAIL: the release build (PI5_GADGET=1) sees the test mode:\n  " + "\n  ".join(words))
    if not gadget:
        sys.exit("pi5_flags_src: FAIL: the release build does not start the USB-C Sidecar (kernel.cpp)")
    words, gadget = scan(eng, test)
    if not gadget or not any("testsecs" in w for w in words) or not any("UsbTestTick" in w for w in words):
        sys.exit("pi5_flags_src: FAIL: the test build (PI5_TESTMODE=1) lost the test mode or the gadget")
    words, _ = scan(eng, nogadget)
    if words:
        sys.exit("pi5_flags_src: FAIL: PI5_GADGET=0 sees the test mode:\n  " + "\n  ".join(words))
    # the selftest of this script: an unknown condition is "maybe", so a word under it is reported
    probe = strip_comments("#if FOO\nint testsecs;\n#endif\n#ifdef SIDECAR_PI5_TESTMODE\nint m_nTestSecs;\n#else\nint ok;\n#endif\n")
    vis = visible(probe, release)
    if 2 not in vis or 5 in vis or 7 not in vis:
        sys.exit("pi5_flags_src: FAIL: the #if tracker itself (%s)" % vis)
    print("pi5_flags_src: ok (release: gadget, no test mode; PI5_TESTMODE=1: both)")


if __name__ == "__main__":
    main()
