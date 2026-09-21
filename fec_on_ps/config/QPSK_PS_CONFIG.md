# Trial procedure — QPSK modulation selected VIA CONFIG FROM THE PS (writable dac_replay)

**Feature:** the `dac_replay` golden-waveform ROM (the RF transmit path) is now **writable
from the PS** over AXI, so whichever modulation waveform the PS loads (QPSK/64/256-QAM) is
what the board transmits — the RX (`RX_ADAPTIVE_MOD`) reads the SIGNAL field and decodes the
right modulation. The shared FFT is kept (to fit the Zynq-7020). A rebuild is still
unavoidable because the modulation is compile-time, but **at runtime the PS can select** any
pre-loaded waveform.

**Bitstream:** `system_top_span65.bit.bin` md5 `6359c17a420b245ece71dfc1500798ec`
(RTL: `ip/tx_intf/src/tx_intf.v` — the `dac_replay` memory changed from a `$readmemh` ROM to
a **writable block RAM**: `slv_reg9`=data with auto-increment, pointer reset = `slv_reg0[3]`;
synchronous read `rd_q<=mem[rd_addr]`, `rd_addr=consume?ptr+1:ptr` to fit block RAM + get the
backpressure/priming timing right).

## Verified (2026-09-17)
- **PS pokes QPSK → internal loopback → `capchk_qpsk tong=0` (byte-exact), first=b23ac9.**
  Proves dac_replay is PS-writable + adaptive-RX-QPSK + poke-from-PS all work.
- The DIGITAL rx_replay path (256-QAM, untouched) = 20/20 byte-exact → the receive chain is intact.

## ⚠️ Two hardware uncertainties (NOT RTL)
1. **openofdm init phase** (clean procedure §6): `initialize` does not reset the FPGA phase →
   it lands on a different phase each time → you must **repeat initialize** until it decodes
   (RUN.sh repeats 15 times).
2. **AD9361 chip contamination**: each toggle of `loopback`/`rf_bandwidth`/quadrature/repeated
   `autoconfig` degrades the chip → **only a power cycle recovers it**. Do not toggle at random.

## On-board tools (/root)
`poke_golden` (load a waveform), `qpsk_golden.bin` (14674 words, QPSK zero-padded),
`capchk_qpsk`+`expected_qpsk.bin` (byte-exact check over 1290B), `ad9361_autoconfig.sh` (710 lines),
`fint2.ftr`.

## Trial procedure (each time from a CLEAN boot — do not toggle at random)

### Step 1 — boot the QPSK bitstream
Power-cycle. At `zynq-uboot>`:
```
fatload mmc 0 0x1000000 qpsk.bin
fpga load 0 0x1000000 ${filesize}
fatload mmc 0 0x3000000 uImage
fatload mmc 0 0x2A00000 devicetree.dtb
setenv bootargs 'console=ttyPS0,115200 root=/dev/mmcblk0p2 rw rootwait rootfstype=ext4 init=/bin/sh earlyprintk sdhci.debug_quirks=0x60'
bootm 0x3000000 - 0x2A00000
```
```sh
mount -t proc proc /proc; mount -t sysfs sysfs /sys
mount -t debugfs none /sys/kernel/debug
cp /root/capchk_qpsk /tmp/cq; chmod +x /tmp/cq; cp /root/expected_qpsk.bin /tmp/eq.bin
[ -f /root/fir.ftr ] || cp /root/fint2.ftr /root/fir.ftr
```

### Step 2 — GATE: repeat initialize + cfg + poke QPSK until byte-exact
```sh
IIO=/sys/bus/iio/devices/iio:device0; DBG=/sys/kernel/debug/iio/iio:device0
kich(){ devmem 0x83c0001c 32 0; devmem 0x83c0001c 32 8; devmem 0x83c0001c 32 0; }
one(){ devmem 0x83c30000 32 1; devmem 0x83c30000 32 0; devmem 0x83c2000c 32 0x800; devmem 0x83c2000c 32 0; kich; usleep 60000; }
cfg(){ echo 1 > $DBG/initialize; sleep 6
  SELFTEST=0 RFBW=skip sh /root/ad9361_autoconfig.sh rf_loopback 15360000 2484000000 -30 >/dev/null 2>&1
  cat /root/fint2.ftr > $IIO/filter_fir_config; echo 1 > $IIO/in_out_voltage_filter_fir_en
  echo manual > $IIO/in_voltage0_gain_control_mode
  devmem 0x83c20010 32 0x8; devmem 0x83c00034 32 0x40; devmem 0x83c30014 32 0x100; devmem 0x83c2002c 32 0x13
  echo -27 > $IIO/out_voltage0_hardwaregain; echo 31 > $IIO/in_voltage0_hardwaregain
  echo 1 > $DBG/loopback   # =1 internal (proves the feature, no cable needed); =0 to measure RF-over-cable (needs image rejection)
  usleep 900000; for w in 1 2 3 4 5; do one; done; }
a=1
while [ $a -le 15 ]; do            # up to 15 tries like RUN.sh — phase is non-deterministic
  cfg; /root/poke_golden /root/qpsk_golden.bin >/dev/null
  ex=0; k=0; while [ $k -lt 10 ]; do k=$((k+1)); one; /tmp/cq /tmp/eq.bin | grep -q tong=0 && ex=$((ex+1)); done
  echo "@@GATE a$a QPSK byte-exact=$ex/10"
  [ $ex -ge 8 ] && { echo "@@DONE good phase on try $a"; break; }
  a=$((a+1))
done
```
Expected: some `a` gives a high `$ex` → good phase → QPSK byte-exact. **The real measurement
is `capchk_qpsk` (`tong=0`), NOT the `pass` counter (which is unreliable on this bitstream).**

### Step 3 — change modulation from the PS (demonstration)
- QPSK: `poke_golden /root/qpsk_golden.bin` → capchk_qpsk.
- 256-QAM (default): reset the FPGA (reload the bitstream) OR poke a 256-QAM waveform → capchk (6450).
  → same bitstream, PS changes the waveform = changes the modulation at runtime.

## RF-over-cable path (loopback=0)
You must **re-measure image rejection for THAT specific boot** (the image coefficients drift
each boot; old coefficients do not work). Use `refine2.py`/`blind.py` (image-rejection
procedure) to measure the IQCOR coefficients and load them. QPSK has a wide SNR margin, so
with correct image rejection + a good phase it gets over the cable better than 256-QAM.

## Reproducibility notes
Phase non-determinism + chip contamination mean each session needs: **power-cycle → run the
gate ONCE straight through → do not re-toggle loopback/bandwidth**. If the gate does not pass
in 15 tries, power-cycle and start over.
