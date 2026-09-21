#!/usr/bin/env bash
# ===========================================================================
# run_new_span.sh -- run the cosim for EACH boundary, three kinds of test:
#
#   1. OFF        every span off  -> must be bit-exact with the unpatched tree
#   2. NEGATIVE   span on, nothing injected -> the pipeline must STALL (byte_phat=0).
#                 This is the "fail-fast" test: if the injection point is not on
#                 the live path, turning the span on changes nothing, and this
#                 test exposes it.
#   3. POSITIVE   real data injected through the PS -> the message must come back
#                 byte for byte
#
#   ./cosim/run_new_span.sh [RTL_DIR]
# ===========================================================================
set -u
D=$(cd "$(dirname "$0")" && pwd)
echo "See rtl/README.md, section 'Cosim, every case', for the full results table."
echo "Three kinds of test, run with the RTL tree's tools_cosim/runmod2.sh:"
echo "  1. every span OFF          -> tong=0"
echo "  2. B2/B3/B4/B5 on, no inject -> byte_phat=0"
echo "  3. tb_span_b5 / tb_span_b4  -> message bytes match byte for byte"
