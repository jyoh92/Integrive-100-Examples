#!/usr/bin/env bash
# ===========================================================================
# campaign.sh -- run the cosim MANY TIMES with different seeds, then aggregate.
#
#   ./cosim/campaign.sh [NUM_SEEDS] [CODEWORDS_PER_RUN]     default 20 50
#
# Why: one run with a fixed seed is only ONE sample.  The conclusion "the RTL
# fails at exactly t = 20" is only firm if it repeats across many independent
# vector sets, and if the sub-t levels still agree EXACTLY on a large sample.
#
# Aggregated by TRUE error count (not the number of injections -- random
# positions can collide and cancel).
# ===========================================================================
set -u
NSEED=${1:-20}
NCW=${2:-50}
D=$(cd "$(dirname "$0")" && pwd)
G=/tmp/rs_chiendich
rm -rf $G; mkdir -p $G

echo "== run $NSEED seeds x $NCW codewords, error levels 17..21 =="
tong=0
for e in 17 18 19 20 21; do
  for s in $(seq 1 $NSEED); do
    seed=$(( 1000 * e + 7919 * s ))
    COSIM_WORK=$G/w "$D/run_cosim.sh" "$NCW" "$e" "$seed" > $G/lan.log 2>&1
    # ★ run_cosim returns 1 when there is a mismatch WITHIN the bound -- that is
    #   a result, not a run failure.  Only a MISSING CSV is a real failure.
    #   Report LOUDLY, do not swallow: an earlier version of this script printed
    #   "100 runs done" while all 100 had actually failed.
    if [ ! -s $G/w/results.csv ]; then
      echo "  ★ RUN FAILED (level $e, seed $seed) -- no results produced:"
      tail -6 $G/lan.log | sed "s/^/      /"
      exit 1
    fi
    cat $G/w/results.csv >> $G/aggregate.csv
    tong=$((tong+1))
  done
  printf "  level %2d: %d runs done\n" "$e" "$NSEED"
done
echo "  $tong simulation runs total"

echo
python3 - <<'PY'
import collections, sys
T = 20
d = collections.defaultdict(lambda: [0,0,0,0])   # nerr -> [n, sw_ok, rtl_ok, match]
for ln in open('/tmp/rs_chiendich/aggregate.csv'):
    a = ln.strip().split(',')
    if len(a) != 4: continue
    k = int(a[0]); r = d[k]
    r[0] += 1; r[1] += int(a[1]); r[2] += int(a[2]); r[3] += int(a[3])

print(f"  {'errors':>6}  {'codewords':>7}  {'software':>10}  {'RTL':>10}  {'SW == RTL':>11}")
print(f"  {'------':>6}  {'-------':>7}  {'----------':>10}  {'----------':>10}  {'-----------':>11}")
diffs_within = 0
for k in sorted(d):
    n, sw, rtl, kh = d[k]
    note = "" if k <= T else "   (beyond the bound, not required)"
    print(f"  {k:>6}  {n:>7}  {100*sw/n:>9.1f}%  {100*rtl/n:>9.1f}%  {100*kh/n:>10.1f}%{note}")
    if k <= T and kh != n:
        diffs_within += n - kh

print()
if diffs_within:
    print(f"  ★ {diffs_within} codewords WITHIN the bound where the two disagree.")
else:
    print("  ★ Within the correction bound, the two agree EXACTLY across the whole sample.")
PY
