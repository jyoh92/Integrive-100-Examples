#!/usr/bin/env bash
# ===========================================================================
# run_cosim.sh -- compare the SOFTWARE decoder against the RTL decoder.
#
#   ./cosim/run_cosim.sh [NUM_CODEWORDS]        default 24
#
# Why: substituting the block onto the PS is only legitimate if the software
# module gives IDENTICAL results to the RTL block it replaces.  "It runs" does
# not prove that -- the two have to be compared byte for byte on the same vectors.
#
# ★ The RTL has no erasure port.  The comparison runs on the shared capability:
#   errors only, t = 20.  The PS module can also correct 2f + e <= 40, and there
#   is nothing to compare that against.
#
# Needs:
#   - Vivado xsim  (default /tools/Xilinx/Vivado/2022.2)
#   - the RTL source (variable RTL_DIR) -- NOT vendored in this repo
# ===========================================================================
set -u
NCW=${1:-24}
NERR=${2:-}   # fixed error count per codeword; empty = ramp 0..21
SEED=${3:-}   # seed; empty = default
LO=${4:-}     # sweep the error level from LO..HI within one simulation
HI=${5:-}
D=$(cd "$(dirname "$0")" && pwd)
VIVADO=${VIVADO:-/tools/Xilinx/Vivado/2022.2}
RTL_DIR=${RTL_DIR:-/home/nttien3i/Desktop/crowdsupply/boards/neptunesdr/src/ipshared/c40a/verilog}
W=${COSIM_WORK:-/tmp/rs_cosim}

for f in rs_255_215_decoder.sv gf256_pkg.sv; do
  [ -f "$RTL_DIR/$f" ] || {
    echo "MISSING $RTL_DIR/$f"
    echo "  The RTL source is not vendored in this repo (not part of the example)."
    echo "  Point at it with:  RTL_DIR=/your/path ./cosim/run_cosim.sh"
    exit 1; }
done
[ -x "$VIVADO/bin/xvlog" ] || { echo "MISSING xsim at $VIVADO"; exit 1; }

rm -rf "$W"; mkdir -p "$W"; cd "$W"

echo "== 1. build the software decoder + generate vectors ($NCW codewords) =="
gcc -O2 -std=c99 -Wall -I"$D/../include" \
    -DPS_RS_NO_MAIN -c -o ps_rs_decode.o "$D/../examples/ps_rs_decode.c" || exit 1
gcc -O2 -std=c99 -Wall -o genvec "$D/gen_vectors.c" ps_rs_decode.o || exit 1
./genvec "$NCW" "${NERR:--1}" "${SEED:-20260914}" "${LO:--1}" "${HI:--1}" || exit 1

# the testbench reads a flat array, one byte per line
awk '{ for (i = 1; i <= length($0); i += 2) print substr($0, i, 2) }' coded.hex > coded_flat.hex
echo "  coded_flat.hex: $(wc -l < coded_flat.hex) bytes  (expected $((NCW*255)))"

echo
echo "== 2. run the RTL decoder on those exact vectors =="
source "$VIVADO/settings64.sh" >/dev/null 2>&1 || true
xvlog -sv "$RTL_DIR/gf256_pkg.sv" "$RTL_DIR/rs_255_215_decoder.sv" > xvlog.log 2>&1
if grep -qiE '^ERROR' xvlog.log; then echo "  RTL COMPILE ERROR:"; grep -iE '^ERROR' xvlog.log | head -5; exit 1; fi
xvlog -sv -d NCW=$NCW "$D/tb_rs_compare.sv" >> xvlog.log 2>&1
if grep -qiE '^ERROR' xvlog.log; then echo "  TB COMPILE ERROR:"; grep -iE '^ERROR' xvlog.log | head -5; exit 1; fi
xelab -L xpm --relax -s rscosim work.tb_rs_compare > xelab.log 2>&1
if grep -qiE '^ERROR' xelab.log; then echo "  XELAB ERROR:"; grep -iE '^ERROR' xelab.log | head -5; exit 1; fi
xsim rscosim -runall 2>&1 | grep -E 'TB-RS|ERROR' | sed 's/^/  /'

echo
echo "== 3. compare byte for byte =="
[ -s rtl_out.hex ] || { echo "  RTL produced no output -- see $W"; exit 1; }
python3 - "$NCW" <<'PY'
import sys
ncw = int(sys.argv[1])
T = 20                      # RS(255,215) correction capability: t = (255-215)/2

def doc(p):
    # ★ xsim's $fwrite("%02X") prints LOWERCASE, while the C side prints UPPERCASE.
    #   A verbatim compare would report "not equivalent" on identical data --
    #   exactly the false alarm this test is meant to avoid.
    return [l.strip().upper() for l in open(p) if l.strip()]

exp, sw, rtl = doc('expect.hex'), doc('sw_out.hex'), doc('rtl_out.hex')
nerr = [int(x) for x in doc('nerr.txt')]
try:
    fail = {int(l.split()[0]): int(l.split()[1]) for l in doc('rtl_flags.txt')}
except Exception:
    fail = {}
n = min(len(exp), len(sw), len(rtl), len(nerr))
print(f"  codewords compared: {n}")

trong, ngoai = [], []
for i in range(n):
    (trong if nerr[i] <= T else ngoai).append(i)

def dem(idx, a, b):
    return sum(1 for i in idx if a[i] == b[i])

print()
print(f"  WITHIN correction capability (<= {T} errors): {len(trong)} codewords  -- MUST agree")
print(f"    software correct msg  : {dem(trong, sw, exp)}/{len(trong)}")
print(f"    RTL      correct msg  : {dem(trong, rtl, exp)}/{len(trong)}")
print(f"    SOFTWARE == RTL       : {dem(trong, sw, rtl)}/{len(trong)}")

lech = [i for i in trong if sw[i] != rtl[i]]
if ngoai:
    print()
    print(f"  BEYOND correction capability (> {T} errors): {len(ngoai)} codewords  -- both results are LEGAL")
    print(f"    error counts: {sorted(set(nerr[i] for i in ngoai))}")
    print(f"    software == RTL       : {dem(ngoai, sw, rtl)}/{len(ngoai)}  (not required)")
    print(f"    Beyond t = {T}, the decoder may miscorrect to a different valid codeword.")
    print(f"    Divergence here is CORRECT in theory, not a bug.")

# Machine-readable table for aggregating many runs: TRUE error count, three comparisons.
with open('results.csv', 'w') as f:
    for i in range(n):
        f.write(f"{nerr[i]},{int(sw[i]==exp[i])},{int(rtl[i]==exp[i])},{int(sw[i]==rtl[i])}\n")

print()
if lech:
    print(f"  codewords differing WITHIN capability: {lech[:10]}{' ...' if len(lech)>10 else ''}")
    for i in lech[:5]:
        print(f"    #{i}: {nerr[i]} real symbol errors, RTL o_fail={fail.get(i,'?')}, "
              f"software {'RIGHT' if sw[i]==exp[i] else 'WRONG'}, RTL {'RIGHT' if rtl[i]==exp[i] else 'WRONG'}")
    print("  ★ NOT EQUIVALENT -- the block must not be substituted onto the PS.")
    sys.exit(1)
print(f"  ★ EQUIVALENT byte for byte on all {len(trong)} codewords within capability.")
PY
rc=$?
echo
echo "working directory: $W"
exit $rc
