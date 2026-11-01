#!/usr/bin/env python3
"""core/tests/bench/bench_prof.py BINARY PROF.txt [-n TOP] [-m BENCH]: bench_render's PC samples per bench: the top
functions and the instruction mix (scalar FP, NEON, integer, load/store, branch). Run where objdump reads the binary
(the rytm-circle image); the mix ranks loops by what an A72 would run, the samples' times are the host's."""
import re, subprocess, sys, collections

def cls(m, ops):
    v = re.search(r'\bv\d+\.', ops) is not None
    q = re.search(r'\bv\d+\.\d*[bhsd]', ops) is not None
    if m in ('ldr', 'ldur', 'ldp', 'ldrb', 'ldrh', 'ldrsh', 'ldrsw', 'ldrsb', 'ldar', 'ldaxr', 'ld1', 'ld2', 'ld1r', 'ldxr'):
        return 'load-q' if re.match(r'q\d+', ops) or q else 'load'
    if m in ('str', 'stur', 'stp', 'strb', 'strh', 'stlr', 'stxr', 'st1', 'st2'):
        return 'store-q' if re.match(r'q\d+', ops) or q else 'store'
    if m[0] == 'b' and (m in ('b', 'bl', 'blr', 'br') or m.startswith('b.')) or m in ('cbz', 'cbnz', 'tbz', 'tbnz', 'ret'):
        return 'branch'
    if q:
        return 'neon'
    if m in ('fdiv', 'fsqrt'):
        return 'fdiv/sqrt'
    if re.search(r'\b[sd]\d+\b', ops) and (m[0] == 'f' or m in ('scvtf', 'ucvtf')):
        return 'fp-scalar'
    return 'int'

def main():
    a = sys.argv[1:]
    top = int(a[a.index('-n') + 1]) if '-n' in a else 8
    only = a[a.index('-m') + 1] if '-m' in a else None
    dis = subprocess.run(['objdump', '-dC', '--no-show-raw-insn', a[0]], capture_output=True, text=True).stdout
    ins, fn, f = {}, {}, ''
    for l in dis.splitlines():
        m = re.match(r'^[0-9a-f]+ <(.*)>:$', l)
        if m:
            f = m.group(1)
            continue
        m = re.match(r'^\s+([0-9a-f]+):\s+(\S+)\s*(.*)$', l)
        if m:
            k = int(m.group(1), 16)
            ins[k] = cls(m.group(2), m.group(3)), f
    sec, name = {}, None
    for l in open(a[1]):
        if l[0] == '#':
            name = l[2:].strip()
            sec[name] = []
        else:
            p, c = l.split()
            sec[name].append((int(p, 16), int(c)))
    tot = collections.Counter()
    for n, rows in sec.items():
        if only and only not in n:
            continue
        fc, mc, t = collections.Counter(), collections.Counter(), 0
        for p, c in rows:
            i = ins.get(p)
            t += c
            if i:
                mc[i[0]] += c
                fc[i[1][:70]] += c
                tot[i[0]] += c
            else:
                fc['?'] += c
        if not t:
            continue
        if '-l' in a:   # source lines of the samples, inlines resolved to their leaf
            lines = collections.Counter()
            ad = [hex(p) for p, c in rows]
            out = subprocess.run(['addr2line', '-e', a[0]] + ad, capture_output=True, text=True).stdout.split()
            for (p, c), l in zip(rows, out):
                lines['/'.join(l.split('/')[-2:]).split(' ')[0]] += c
            print(f'== {n}: {t} samples, lines')
            for k, v in lines.most_common(top):
                print(f'   {100 * v / t:5.1f} % {k}')
            continue
        print(f'== {n}: {t} samples')
        print('   mix: ' + ' '.join(f'{k} {100 * v // t}%' for k, v in mc.most_common()))
        for k, v in fc.most_common(top):
            print(f'   {100 * v / t:5.1f} % {k}')

main()
