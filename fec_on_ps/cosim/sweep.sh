#!/usr/bin/env bash
# ===========================================================================
# sweep.sh -- sweep the error count 15..20 to find where the RTL starts failing.
#
# Why: the 08-09 comparison showed the RTL raising o_fail and returning a wrong
# answer at EXACTLY 20 symbol errors, while the software decoder still decodes.
# We need to know whether that is a special case or a systematic RTL limit.
#
#   ./cosim/sweep.sh [CODEWORDS_PER_LEVEL]      default 20
# ===========================================================================
set -u
N=${1:-20}
D=$(cd "$(dirname "$0")" && pwd)
printf "%6s  %14s  %14s  %12s  %10s\n" "errors" "software ok" "RTL ok" "SW == RTL" "RTL o_fail"
printf "%6s  %14s  %14s  %12s  %10s\n" "------" "--------------" "--------------" "------------" "----------"
for e in 15 16 17 18 19 20; do
  out=$(COSIM_WORK=/tmp/rs_quet_$e NERR=$e "$D/run_cosim.sh" "$N" "$e" 2>&1)
  kh=$(echo "$out" | grep -oP 'SOFTWARE == RTL       : \K[0-9]+/[0-9]+' | head -1)
  sw=$(echo "$out"  | grep -oP 'software correct msg  : \K[0-9]+/[0-9]+' | head -1)
  rtl=$(echo "$out" | grep -oP 'RTL      correct msg  : \K[0-9]+/[0-9]+' | head -1)
  nf=$(grep -c ' 1$' /tmp/rs_quet_$e/rtl_flags.txt 2>/dev/null); nf=${nf:-0}
  printf "%6s  %14s  %14s  %12s  %10s\n" "$e" "${sw:-?}" "${rtl:-?}" "${kh:-?}" "$nf"
done
