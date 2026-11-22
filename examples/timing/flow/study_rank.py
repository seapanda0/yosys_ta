#!/usr/bin/env python3
"""usage: study_rank.py [results.tsv] [reference_script] [variant]
Ranks ABC scripts by the geometric mean of the sized min period, normalized per design to the reference script."""
import sys, math, collections
fn = sys.argv[1] if len(sys.argv) > 1 else __import__('os').path.join(__import__('os').path.dirname(__import__('os').path.abspath(__file__)), '..', 'study', 'results.tsv')
ref = sys.argv[2] if len(sys.argv) > 2 else 'yosys_default'
variant = sys.argv[3] if len(sys.argv) > 3 else 'default'
data = collections.OrderedDict()
for line in open(fn):
    f = line.rstrip('\n').split('\t')
    if len(f) != 8 or f[1] != variant or 'FAIL' in f: continue
    data[(f[0], f[2])] = tuple(float(x) for x in f[3:8])   # last entry wins
designs = sorted({d for d, s in data}); scripts = sorted({s for d, s in data})
rows = []
for s in scripts:
    rp, ra, rpre, n, t = [], [], [], 0, 0.0
    for d in designs:
        if (d, s) in data and (d, ref) in data:
            a, b = data[(d, s)], data[(d, ref)]
            rpre.append(a[0] / b[0]); rp.append(a[2] / b[2]); ra.append(a[3] / b[3]); n += 1; t += a[4]
    if n:
        gm = lambda v: math.exp(sum(math.log(x) for x in v) / len(v))
        rows.append((gm(rp), s, n, gm(rpre), gm(ra), max(rp), min(rp), t))
rows.sort()
print(f"variant={variant} reference={ref} designs={len(designs)}")
print(f"{'script':28s} {'n':>3s} {'period(sized)':>13s} {'period(abc)':>11s} {'area':>7s} {'worst':>7s} {'best':>7s} {'time_s':>8s}")
for gp, s, n, gpre, ga, mx, mn, t in rows:
    print(f"{s:28s} {n:3d} {gp:13.3f} {gpre:11.3f} {ga:7.3f} {mx:7.3f} {mn:7.3f} {t:8.0f}")
if '-v' in sys.argv:
    print()
    print('design'.ljust(16) + ''.join(s[:11].rjust(12) for s in scripts))
    for d in designs:
        print(d.ljust(16) + ''.join((f"{data[(d,s)][2]:.3f}" if (d, s) in data else '-').rjust(12) for s in scripts))
