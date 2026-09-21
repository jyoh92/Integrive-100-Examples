#!/bin/sh
# =============================================================================
#  rf_setup.sh -- second half of RX bring-up, run AFTER ad9361_autoconfig.sh.
#
#  ad9361_autoconfig.sh brings the transceiver to DECODE-READY.  This script
#  applies the remaining datapath settings that made the receiver actually lock
#  and decode over the cable loopback on hardware:
#
#      frames received = 40/40   PER = 0.0000   (256-QAM, cable, TX -27 dBm / RX 31)
#
#  It writes only to /sys (IIO) and /dev (devmem); it never writes the rootfs,
#  so it runs unchanged on a read-only SD card.
#
#  Usage:   sh rf_setup.sh [TX_dBm] [RX_dB] [fint2.ftr path]
#           TX default -27  (the cable-loopback safety ceiling), RX default 31.
#
#  ! CABLE SAFETY: with TX and RX wired together by an SMA cable, TX must not
#    exceed -27 dBm or the receiver front-end is damaged.  Do not raise it for
#    a cabled board.  For two loose antennas the ceiling does not apply.
# =============================================================================
set -u
I=/sys/bus/iio/devices/iio:device0
D=/sys/kernel/debug/iio/iio:device0
CTX=${1:--27}
CRX=${2:-31}
FIR=${3:-/root/fint2.ftr}
[ -f "$FIR" ] || FIR=./fint2.ftr

mount -t debugfs none /sys/kernel/debug 2>/dev/null

# One transmit-and-capture pulse; used only to warm the chain before measuring.
kich(){ devmem 0x83c0001c 32 0x0; devmem 0x83c0001c 32 0x8; devmem 0x83c0001c 32 0x0; }
one(){ devmem 0x83c30000 32 1; devmem 0x83c30000 32 0
       devmem 0x83c2000c 32 0x800; devmem 0x83c2000c 32 0x0; kich; usleep 60000; }

# --- FIR: MUST disable before loading a new set of taps ----------------------
#   Overwriting filter_fir_config while the FIR is ENABLED leaves the driver's
#   clock chain half-recomputed and it can settle at ADC=184.32 MHz instead of
#   245.76.  fint2.ftr has 128 taps; ad9361.c requires (ADC/2)/RX_SAMPL*16 >=
#   taps, i.e. ADC >= 245.76 MHz -- otherwise the ENABLE write is REJECTED and
#   the sample rate is silently wrong, which reads downstream as 0/N sync.
echo 0 > $I/in_out_voltage_filter_fir_en 2>/dev/null
cat "$FIR" > $I/filter_fir_config
if ! echo 1 > $I/in_out_voltage_filter_fir_en 2>/tmp/fir.err; then
    echo "@@FIR_REJECTED $(cat /tmp/fir.err 2>/dev/null)"
    echo "  (the enable was refused -- sample rate is wrong; sync will read 0/N)"
elif [ "$(cat $I/in_out_voltage_filter_fir_en 2>/dev/null)" != "1" ]; then
    echo "@@FIR_REJECTED read-back is not 1"
else
    echo "FIR enabled"
fi

echo manual > $I/in_voltage0_gain_control_mode
echo 0 > $D/loopback                 # 0 = analog path (TX->cable->RX); 1 = BIST

devmem 0x83c20010 32 0x8             # bb_20M_en
devmem 0x83c00034 32 0x40            # TX baseband gain x0.5
devmem 0x83c30014 32 0x100           # ws=0 + fft_sat_en  (0x0 would DISABLE sat)
devmem 0x83c2002c 32 0x13            # bb_gain=3 + decim_bypass

echo "$CRX" > $I/in_voltage0_hardwaregain      # set RX gain FIRST
echo "$CTX" > $I/out_voltage0_hardwaregain     # then raise TX
usleep 900000

w=1; while [ $w -le 20 ]; do one; w=$((w+1)); done   # >= 20 warm-up frames

echo "RF setup done: TX=$CTX dBm  RX=$CRX dB  Fs=$(cat $I/in_voltage_sampling_frequency 2>/dev/null)  loopback=analog"
