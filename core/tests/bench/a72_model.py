#!/usr/bin/env python3
"""core/tests/bench/a72_model.py BINARY TRACE.u32 CALLS [-n TOP]: a Cortex-A72's rough cost of a traced path (trace_steps.c):
instructions by class at the A72 optimisation guide's throughputs (3 decoded a cycle; 2 scalar FP, 1 q-form FP; 1 load, 1 store
a cycle), a 48 KB 3-way L1I (12 cycles a miss), taken branches 1 cycle, a bimodal predictor (13 cycles a miss). No data cache,
no dependencies: a model for comparing two builds of one path, not a clock."""
import array, collections, re, subprocess, sys

def cls(m, ops):
    q = re.search(r'\bv\d+\.\d*[bhsd]', ops) or re.match(r'q\d+', ops)
    if m.startswith(('ldr', 'ldur', 'ldp', 'ldar', 'ldx', 'ld1', 'ld2')): return 'load'
    if m.startswith(('str', 'stur', 'stp', 'stlr', 'stx', 'st1', 'st2')): return 'store'
    if m.startswith('b.') or m in ('b', 'bl', 'blr', 'br', 'ret', 'cbz', 'cbnz', 'tbz', 'tbnz'): return 'branch'
    if q: return 'q'
    if m in ('fdiv', 'fsqrt'): return 'div'
    if re.search(r'\b[sd]\d+\b', ops) and (m[0] == 'f' or m in ('scvtf', 'ucvtf')): return 'fp'
    return 'int'
COST = {'int': 1 / 3, 'load': 1.0, 'store': 1.0, 'branch': 1 / 3, 'fp': 0.5, 'q': 1.0, 'div': 7.0}

a = sys.argv[1:]
top = int(a[a.index('-n') + 1]) if '-n' in a else 10
dis = subprocess.run(['objdump', '-dC', '--no-show-raw-insn', a[0]], capture_output=True, text=True).stdout
ins, f = {}, ''
for l in dis.splitlines():
    m = re.match(r'^[0-9a-f]+ <(.*)>:$', l)
    if m: f = m.group(1)[:60]; continue
    m = re.match(r'^\s+([0-9a-f]+):\s+(\S+)\s*(.*)$', l)
    if m: ins[int(m.group(1), 16)] = (cls(m.group(2), m.group(3)), f)
t = array.array('I'); t.frombytes(open(a[1], 'rb').read())
calls = int(a[2])
sets = [[] for _ in range(48 * 1024 // 64 // 3)]
bim = collections.Counter()
c = collections.Counter(); fn = collections.Counter(); fi = collections.Counter(); cyc = 0.0; miss = 0; mp = 0; taken = 0
prev = None; pline = -1
for i, pc in enumerate(t):
    k, f = ins.get(pc, ('int', '?'))
    w = COST[k]
    line = pc >> 6
    if line != pline:
        s = sets[line % len(sets)]
        if line in s: s.remove(line)
        else:
            miss += 1; w += 12
            if len(s) == 3: s.pop(0)
        s.append(line); pline = line
    if i + 1 < len(t):
        nx = t[i + 1]
        if k == 'branch':
            tk = nx != pc + 4
            if tk: w += 1; taken += 1
            b = bim.get(pc, 2)
            if (b >= 2) != tk: w += 13; mp += 1
            bim[pc] = min(3, b + 1) if tk else max(0, b - 1)
    c[k] += 1; fn[f] += w; fi[f] += 1; cyc += w
print(f'{len(t) / calls:.0f} instr, {cyc / calls:.0f} model cycles a block; I-misses {miss / calls:.2f}, branch misses {mp / calls:.2f}, taken {taken / calls:.0f}')
print('  ' + ' '.join(f'{k} {v / calls:.0f}' for k, v in c.most_common()))
for k, v in fn.most_common(top): print(f'  {v / calls:7.0f} cyc {fi[k] / calls:6.0f} ins  {k}')
