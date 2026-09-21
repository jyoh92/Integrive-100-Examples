#!/bin/sh
# run_fecps.sh -- FEC-on-PS (span65) with image rejection, on the HTWAVE board.
# PREREQUISITE: span65 already loaded into the PL (via JTAG from the PC).
IIO=/sys/bus/iio/devices/iio:device0; DBG=/sys/kernel/debug/iio/iio:device0
mount -t debugfs none /sys/kernel/debug 2>/dev/null
echo "== 1. AD9361 + FPGA config (rf_loopback 15.36 MSPS, no initialize) =="
SELFTEST=0 RFBW=skip sh /root/ad9361_autoconfig.sh rf_loopback 15360000 2484000000 -30 >/tmp/ac.log 2>&1
grep -q DECODE-READY /tmp/ac.log && echo "  DECODE-READY" || { echo "  autoconfig tail:"; tail -3 /tmp/ac.log; }
echo 0 > $IIO/in_out_voltage_filter_fir_en 2>/dev/null
cat /root/fint2.ftr > $IIO/filter_fir_config
echo 1 > $IIO/in_out_voltage_filter_fir_en 2>/tmp/fir.err && echo "  FIR on" || echo "  FIR REJECT $(cat /tmp/fir.err)"
echo manual > $IIO/in_voltage0_gain_control_mode
echo 0 > $DBG/loopback
devmem 0x83c20010 32 0x8; devmem 0x83c00034 32 0x40; devmem 0x83c30014 32 0x100; devmem 0x83c2002c 32 0x13
echo -27 > $IIO/out_voltage0_hardwaregain; echo 31 > $IIO/in_voltage0_hardwaregain
usleep 900000
# warmup
w=0; while [ $w -lt 20 ]; do w=$((w+1)); devmem 0x83c30000 32 1; devmem 0x83c30000 32 0; devmem 0x83c2000c 32 0x800; devmem 0x83c2000c 32 0x0; devmem 0x83c0001c 32 0x0; devmem 0x83c0001c 32 0x8; devmem 0x83c0001c 32 0x0; usleep 60000; done
echo "== 2. image rejection (apply this board's coefficients) =="
sh /root/khuanh.sh
echo "== 3. fec_on_ps 40 =="
cd /root && ./fec_on_ps 40
