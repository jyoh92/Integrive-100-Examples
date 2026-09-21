#!/bin/sh
# =============================================================================
#  bringup.sh -- take the board from a fresh boot to DECODE-READY, on-board.
#
#  Runs the two halves of RX bring-up in order:
#      1. ad9361_autoconfig.sh rf_loopback 15360000 2484000000 -30
#      2. rf_setup.sh <TX> <RX>
#
#  After this, examples/fec_on_ps (or ps_hello) will see live frames:
#      frames received = N/N   PER = 0.0000   over the cable loopback.
#
#  Run it from wherever these files live (they read each other by relative
#  path, then fall back to /root):
#      sh bringup.sh [TX_dBm] [RX_dB]        # defaults -27 / 31 (cable)
#
#  Fs is fixed at 15.36 MSPS: the AD9361 owns the sample rate, and every filter
#  and register value here is sized for it.  Do not change it to match a
#  bitstream's processing clock -- they are different clocks.
# =============================================================================
set -u
HERE=$(cd "$(dirname "$0")" 2>/dev/null && pwd) || HERE=.
CTX=${1:--27}
CRX=${2:-31}

AC="$HERE/ad9361_autoconfig.sh"; [ -f "$AC" ] || AC=/root/ad9361_autoconfig.sh
RF="$HERE/rf_setup.sh";         [ -f "$RF" ] || RF=/root/rf_setup.sh
FIR="$HERE/fint2.ftr";          [ -f "$FIR" ] || FIR=/root/fint2.ftr

echo "== 1/2  ad9361_autoconfig (rf_loopback, Fs=15360000, 2484 MHz) =="
SELFTEST=0 sh "$AC" rf_loopback 15360000 2484000000 -30 > /tmp/ac.log 2>&1
if grep -q 'DECODE-READY' /tmp/ac.log; then
    echo "  DECODE-READY"
else
    echo "  autoconfig did NOT report DECODE-READY -- last lines:"
    tail -4 /tmp/ac.log | sed 's/^/    /'
fi

echo "== 2/2  rf_setup (TX=$CTX RX=$CRX) =="
sh "$RF" "$CTX" "$CRX" "$FIR"

# --- image rejection: apply this board's IQ-imbalance correction -------------
# Runs AFTER rf_setup so it overrides the quadrature-tracking the autoconfig
# leaves enabled.  See khuanh.sh for the per-boot caveat on the coefficients.
KA="$HERE/khuanh.sh"; [ -f "$KA" ] || KA=/root/config/khuanh.sh
[ -f "$KA" ] && sh "$KA"

# --- verify the receiver actually locked -------------------------------------
# The AD9361 bring-up is NON-DETERMINISTIC: on some boots it comes up without a
# clean lock (the clean-bring-up procedure measured "3/8 die").  Re-running autoconfig does NOT fix a
# bad lock -- it tends to make it worse.  The only reliable recovery is a clean
# power cycle and a single bring-up.  So verify here and say plainly whether to
# power-cycle and retry, rather than let fec_on_ps decode noise.
if [ -x /root/bytecmp_rxr3 ] && [ -f /root/golden6450.bin ]; then
    R=$(/root/bytecmp_rxr3 /root/golden6450.bin 20 5000 0 2>&1 | \
        grep -oE 'got_lock=[01].*match=[0-9]+/6450' | head -1)
    echo "  lock check: $R"
    case "$R" in
        got_lock=1*match=58*|got_lock=1*match=6[0-4]*)
            echo "  LOCK OK -- fec_on_ps will decode." ;;
        *)
            echo "  ! LOCK POOR.  POWER-CYCLE the board and run bring-up again;"
            echo "    do NOT re-run autoconfig on this boot (it degrades the lock)." ;;
    esac
fi

echo "== bring-up complete.  Now run:  ./fec_on_ps 40  =="
