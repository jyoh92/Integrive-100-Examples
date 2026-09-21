# Standard run procedure — FEC-on-PS (span65) with image rejection

**Verified on hardware 2026-09-17: `fec_on_ps 40` = 1200/1200 codewords byte-exact
(100.0%), reproduced 4/4 runs.** neptunesdr board (Zynq-7020 + AD9364), TX↔RX cable,
span65 atomic-fix (md5 `44891a6c`), this board's image-rejection coefficients.

## 0. Prerequisites

- SMA cable connecting the board's **TX↔RX**.
- SD card already prepared (files pre-copied):
  - FAT: `span65.bin` (md5 44891a6c), `uimage`, `devicetree.dtb`.
  - `/root/`: `fec_on_ps`, `golden6450.bin`, `khuanh.sh`, `fint2.ftr`, `run_fecps.sh`,
    `ad9361_autoconfig.sh` (md5 f0571f49).
- Board UART (FT232R) → PC, 115200 8N1.

## 1. Boot to u-boot

Power-cycle. The board stops at the `zynq-uboot>` prompt (if it auto-continues, press
any key during the countdown to stop it).

## 2. Load span65 into the PL, then boot Linux — type at `zynq-uboot>`

```
fatload mmc 0 0x1000000 span65.bin
fpga load 0 0x1000000 ${filesize}
fatload mmc 0 0x3000000 uimage
fatload mmc 0 0x2A00000 devicetree.dtb
setenv bootargs 'console=ttyPS0,115200 root=/dev/mmcblk0p2 rw rootwait rootfstype=ext4 init=/bin/sh earlyprintk sdhci.debug_quirks=0x60'
bootm 0x3000000 - 0x2A00000
```

Expected: `4045568 bytes read` (span65) → `fpga load` returns to a clean prompt (no error) →
`Starting kernel` → `ad9361 ... probed ADC AD9364` → `Run /bin/sh as init process`.

★ **`fpga load` loads span65 COLD before the kernel probes** — so the AD9361/openofdm
drivers bind to span65 correctly. Do not use fpga_manager (hot) — it risks a clock glitch.

## 3. At the `/bin/sh` shell

```sh
mount -t proc  proc  /proc
mount -t sysfs sysfs /sys
sh /root/run_fecps.sh
```

`run_fecps.sh` does everything: autoconfig `rf_loopback 15360000` (no `initialize`) →
FIR `fint2.ftr` → `loopback=0` + gain (TX −27 / RX 31) → 20 warm-up frames →
**image rejection** (`khuanh.sh`: freeze quadrature + load coefficients
`0x3FFE0229`/`0x02254002`) → `fec_on_ps 40`.

## 4. Expected result

```
== 2. image rejection (apply this board's coefficients) ==
image rejection: 0x79020414=0x3FFE0229 0x79020454=0x02254002 enb=0x00000251 qtrack=0
== 3. fec_on_ps 40 ==
  RS codewords processed         : 1200
  codewords UNRECOVERABLE        : 0
  codewords BYTE-EXACT vs golden : 1200/1200  (100.0%)
  frames received                : 40
  PER                            : 0.0000
```

**1200/1200 (100%)** = the message decoded on the ARM is byte-exact to the transmitted
one for all 30 codewords per frame (including cw#29 — the "atomic-frame" fix in `dot11.v`
resolved the dropped-symbol-10 problem). This is FEC running on the PS (ARM); the rest of
the chain runs in the PL.

## 5. Notes

- **Image rejection & fec_on_ps:** `fec_on_ps` uses digital rx_replay. Image rejection
  corrects the analog path — applying it here cleans up the RF state, but the 100% figure
  comes mainly from the atomic-frame fix + a clean config. For a real analog OTA measurement
  (`do_ota.sh`), image rejection has a more direct effect.
- **Per-boot image-rejection coefficients:** `khuanh.sh` loads coefficients measured on this
  board (2026-09-17). The image-rejection procedure notes that the coefficients drift with the
  chip's QEC state across each power cycle; this is a good default for this board, re-derive
  with `refine2.py`/`blind.py` if you need precision.
- **Quick re-run** (no reboot): just `cd /root && ./fec_on_ps 40`.
