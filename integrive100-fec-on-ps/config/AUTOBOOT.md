# Autoboot for this bitstream (span65)

Goal: power on → the PL is loaded with **span65** → Linux boots → the RX is
brought to **DECODE-READY** → SSH is up. No PC, no keystrokes, no UART.

Everything below is for the neptunesdr (Zynq-7020 + AD9364) booting from QSPI,
and it builds on the general procedure in the repo's parent notes
(`QUYTRINH_AUTOBOOT_2026-09-07.md`). What is new here is (1) loading *span65*
and (2) running the proven RF bring-up automatically.

## The boot chain

```
power on
  → FSBL (in QSPI flash)              loads u-boot
  → u-boot                            default env patched: bootcmd = run uenvboot
     → reads uEnv.txt on the SD (FAT)
        → fpga load  span65.bin       PL loaded COLD, before Linux  (no hot-reprobe panic)
        → bootm      uImage           Linux starts
  → Linux /etc/init.d
     → S40network                     eth0 (DHCP)
     → S50dropbear                    sshd
     → S99bringup   ← NEW             ad9361_autoconfig + rf_setup → DECODE-READY
```

Two pieces already exist on the card and are correct:

- **The patched u-boot in flash** turns `bootcmd` into `run uenvboot`, so the SD
  `uEnv.txt` is what actually drives boot. (Do not `saveenv`; see the parent
  note — the env region can sit under an embedded bitstream.)
- **`uEnv.txt`** (this folder has the exact copy) loads the bitstream named on
  its `bitstream_image=` line. It is set to `span65.bin`. To switch builds,
  change only that one line.

Two pieces must be **added/changed** on the rootfs:

- **Add `S99bringup`** (this folder) to `/etc/init.d/`. It runs the RF bring-up
  in the background, last, after SSH is already up.
- **Disable `S03verify`.** The card ships an old auto-test at `/etc/init.d/S03verify`
  that hot-loads a *different* bitstream (`system_top_fix.bit.bin`) over span65
  via fpga_manager and runs a bytecmp test. It must not run, or it clobbers the
  PL you just cold-loaded. (On the current card its load happens to fail, so
  span65 survives — but do not rely on that.)

## Prerequisite: a writable, clean rootfs

The rootfs partition (`mmcblk0p2`, ext4) is currently **corrupt and mounted
read-only** — `ext4_mb_generate_buddy: group 5 ... inconsistent`, journal
aborted, remounted ro; one inode (`/root/ad9361_autoconfig.sh`) is already
unreadable. You cannot install init scripts onto a read-only rootfs, so repair
it first, off the board:

```bash
# power the board down cleanly, move the SD to the PC, then:
sudo e2fsck -f -y /dev/<sdcard>p2      # repair; -y answers all fixes yes
sudo mount /dev/<sdcard>p2 /mnt        # verify it mounts rw and files read
```

If e2fsck cannot recover it, restore the rootfs from `_SD_BACKUP_2026-08-27/`.

## Install (rootfs writable)

From the PC with the SD mounted (`/mnt` = rootfs p2, `/mnt/boot` or the FAT
mount = p1), or on the board once it is rw:

```bash
# 1. bring-up scripts + data onto the rootfs
mkdir -p /mnt/root/config
cp config/ad9361_autoconfig.sh config/fint2.ftr \
   config/rf_setup.sh config/bringup.sh          /mnt/root/config/
chmod +x /mnt/root/config/*.sh

# 2. the boot-time hook
cp config/S99bringup /mnt/etc/init.d/S99bringup
chmod +x /mnt/etc/init.d/S99bringup

# 3. stop the old auto-test from clobbering the bitstream
mv /mnt/etc/init.d/S03verify /mnt/etc/init.d/S03verify.disabled

# 4. the SD FAT partition (p1): make sure uEnv points at span65
cp config/uEnv.txt <FAT mount>/uEnv.txt          # bitstream_image=span65.bin
```

Then power-cycle. After boot, with nothing typed:

```bash
ssh root@<board-ip> './fec_on_ps 40'
# expect: codewords BYTE-EXACT vs golden : 1160/1200 (96.7%)
ssh root@<board-ip> 'cat /root/bringup.log'      # what S99bringup did
```

## Notes baked into the choices

- **S99, not earlier.** If bring-up ran before dropbear and hung, you would lose
  the network way in. Running it last and in the background guarantees SSH first.
- **Cold `fpga load` in u-boot, not fpga_manager at runtime.** Loading the PL
  before Linux avoids the `clk_out1` glitch that a hot reconfigure caused (it
  hung the CPU in an earlier audit). This is why S99bringup only touches the
  AD9361 and registers, never the bitstream.
- **Cable defaults (TX −27 dBm).** `S99bringup` calls `bringup.sh -27 31`. For
  two antennas, edit that line. The −27 dBm ceiling is a cable-safety rule.
- **Read-only-safe bring-up.** `rf_setup.sh` writes only `/sys` and `/dev`, so
  even if the rootfs goes read-only again the bring-up still runs; only the log
  falls back to `/tmp`.

## FEC on the PS

After bring-up, `./fec_on_ps 40` decodes real coded symbols on the ARM and
matches the golden payload byte-for-byte on 29 of every 30 codewords (96.7%);
the last codeword per frame is not emitted at the B5 tap in this bitstream (an
`openofdm_rx` edge, see [`../docs/conformance.md`](../docs/conformance.md)).
Autoboot delivers a DECODE-READY radio and the FEC-on-PS path runs on it.
