#!/usr/bin/env python3
# ===========================================================================
# analyze.py -- decide: does the RTL fail because of the ERROR COUNT, or
#               because of STATE LEFT OVER from the previous codeword?
#
#   python3 cosim/analyze.py <working_dir>
#
# Two hypotheses for the same phenomenon:
#   (A) error count -- the RTL only fails when that codeword has exactly t = 20 errors.
#   (B) state       -- the RTL fails depending on the PREVIOUS codeword, because the
#       decoder is not returned to a clean state between codewords.  Real hardware
#       runs continuously, so this would matter; the run-from-scratch harness would not.
#
# How to tell them apart: under (A), the failure rate depends only on nerr[i].
# Under (B), it also depends on nerr[i-1].  The cross-table below shows it at a glance.
# ===========================================================================
import sys, os, collections

W = sys.argv[1] if len(sys.argv) > 1 else '/tmp/rs_lon'
def doc(n):
    with open(os.path.join(W, n)) as f:
        return [l.strip().upper() for l in f if l.strip()]

exp, sw, rtl = doc('expect.hex'), doc('sw_out.hex'), doc('rtl_out.hex')
nerr = [int(x) for x in doc('nerr.txt')]
n = min(len(exp), len(sw), len(rtl), len(nerr))
print(f"codewords compared: {n}")

# ---- 1. by the error count of THIS codeword -------------------------------
d = collections.defaultdict(lambda: [0, 0, 0])
for i in range(n):
    r = d[nerr[i]]
    r[0] += 1
    r[1] += (sw[i] == exp[i])
    r[2] += (rtl[i] == exp[i])
print(f"\n{'errors':>7} {'codewords':>8} {'software':>10} {'RTL':>10}")
print(f"{'-'*7} {'-'*8} {'-'*10} {'-'*10}")
for k in sorted(d):
    c, a, b = d[k]
    print(f"{k:>7} {c:>8} {100*a/c:>9.1f}% {100*b/c:>9.1f}%")

# ---- 2. cross-table: previous codeword's errors x this codeword's ----------
print("\nRTL correct (%) -- row = previous codeword's errors, column = this codeword's")
muc = sorted(d)
cross = collections.defaultdict(lambda: [0, 0])
for i in range(1, n):
    c = cross[(nerr[i-1], nerr[i])]
    c[0] += 1
    c[1] += (rtl[i] == exp[i])
print("      " + "".join(f"{k:>7}" for k in muc))
for p in muc:
    dong = f"{p:>5} "
    for q in muc:
        c = cross.get((p, q))
        dong += f"{100*c[1]/c[0]:>6.0f}%" if c and c[0] >= 3 else "      -"
    print(dong)

# ---- 3. conclusion --------------------------------------------------------
print()
theo_nay = {k: d[k][2] / d[k][0] for k in muc}
bien_thien = []
for q in muc:
    ty = [cross[(p, q)][1] / cross[(p, q)][0]
          for p in muc if cross.get((p, q)) and cross[(p, q)][0] >= 3]
    if len(ty) >= 2:
        bien_thien.append(max(ty) - min(ty))
bt = max(bien_thien) if bien_thien else 0.0
print(f"  Largest variation by the PREVIOUS codeword (within one column): {100*bt:.1f} points")
if bt < 0.05:
    print("  => The failure rate does NOT depend on the previous codeword.  Supports hypothesis (A): error count.")
else:
    print("  => The failure rate DOES depend on the previous codeword.  Supports hypothesis (B): leftover state.")
    print("     This matters more than the t bound: real hardware runs continuously,")
    print("     and is not returned to a clean state between codewords.")
