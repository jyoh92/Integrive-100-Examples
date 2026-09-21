# RX bring-up configuration

The examples in this repository (`fec_on_ps`, `ps_hello`) only see live frames
once the AD9361 transceiver and the FPGA datapath have been configured. This
folder holds the exact configuration that brought the board to a decoding state
on hardware, and the two ways to apply it.

## Proven result

With this configuration, on the neptunesdr board (Zynq-7020 + AD9364), a
65 MHz build with the `(B5,B6)` tap, TX and RX joined by an SMA cable:

```
frames received : 40 / 40      (every fired frame is received and synced)
PER             : 0.0000       (the PL Reed-Solomon decoder passes every frame)
```

reproduced across every run. The modulation is 256-QAM at Fs = 15.36 MSPS.

## Files

| File | Runs on | What it does |
|---|---|---|
| `ad9361_autoconfig.sh` | board | One-shot transceiver + FPGA bring-up to `DECODE-READY`. Called as `rf_loopback 15360000 2484000000 -30`. |
| `fint2.ftr` | board (data) | AD9361 128-tap FIR for Fs = 15.36 MSPS (Fpass 5.9 MHz / Fstop 6.8 MHz). |
| `rf_setup.sh` | board | The datapath settings applied *after* autoconfig that make the receiver actually lock: FIR load, `loopback=0` (analog), gains, warm-up. |
| `bringup.sh` | board | Runs `ad9361_autoconfig.sh` then `rf_setup.sh` in order. Leaves the board ready for `./fec_on_ps`. |
| `board_bringup_from_pc.sh` | PC | Pushes the four files to the board's `/tmp` and runs `bringup.sh` over SSH. Works even when the SD rootfs is mounted read-only. |

## How to run

**From the board** (files in the current directory or `/root`):

```sh
sh bringup.sh            # defaults: TX -27 dBm, RX 31 dB (cable loopback)
./fec_on_ps 40
```

**From the PC** (no writes to the SD; safe on a read-only rootfs):

```sh
./config/board_bringup_from_pc.sh 192.168.200.2 -27 31
ssh root@192.168.200.2 './fec_on_ps 40'
```

## What is baked in, and why

Each of these was a failure that produced *plausible* wrong results — a board
that runs and reports `0` — so they are enforced here rather than left to memory:

- **Fs is 15.36 MSPS, not the bitstream's processing clock.** The AD9361 owns
  the sample rate; every filter and register value here is sized for it. A
  65 MHz or 85 MHz *build* does not change it. Setting the AD9361 to 61.44 MHz
  (a plausible-looking "faster" number) desyncs the receiver completely.
- **Disable the FIR before loading new taps.** Overwriting `filter_fir_config`
  while the FIR is enabled leaves the driver's clock chain half-recomputed; it
  can settle at the wrong ADC rate, the enable write is then *refused*, and the
  only downstream symptom is `0/N` sync. `rf_setup.sh` disables, loads, enables,
  and checks the read-back — printing `@@FIR_REJECTED` if it did not take.
- **`loopback=0` for a cable, `loopback=1` for BIST.** With `loopback=1` the
  AD9361 loops digitally and the cable is irrelevant; for a real cabled
  TX→RX path it must be `0`. Mixing these was one cause of an earlier all-noise
  capture.
- **Never `cat` an AD9361 debugfs attribute you don't have to.** Reading some of
  them can reset the part. These scripts only *write* debugfs.
- **Cable safety ceiling: TX ≤ −27 dBm.** With TX and RX wired together, more
  than −27 dBm damages the receiver front-end. This is a hardware rule, not a
  data one. It does not apply to two loose antennas.
- **Read-only-safe.** Everything writes only to `/sys` and `/dev`. A corrupted
  SD card that has remounted read-only still brings up and decodes.

## The lock is non-deterministic — verify it

The AD9361 does not always come up locked. On some boots the receiver bring-up
lands a clean lock and decodes; on others it does not (the clean-bring-up
procedure measured "3 of 8" bad). **Re-running the bring-up does not fix a bad lock — it makes it
worse.** The only reliable recovery is a clean power cycle followed by a single
bring-up. `bringup.sh` runs a lock check at the end (`bytecmp_rxr3`) and prints
`LOCK OK` or tells you to power-cycle and retry. Treat a poor lock as "power-
cycle and run bring-up once more", never as "run autoconfig again".

## FEC on the PS: working

With this bring-up, `./fec_on_ps 40` decodes real coded symbols on the ARM and
scores them against `/root/golden6450.bin`:

```
RS codewords processed        : 1200
codewords BYTE-EXACT vs golden : 1160/1200  (96.7%)
```

i.e. the message recovered on the PS is byte-exact to the transmitted one for 29
of every 30 codewords. The one remaining codeword per frame — the last, the
tenth OFDM symbol's third — is not emitted at the B5 tap in this bitstream and
needs an `openofdm_rx` change to close; see
[`../docs/conformance.md`](../docs/conformance.md). The radio, the capture path,
and the decoder are all proven here.

## Autoboot

To make the board reach this state on its own at power-on — span65 loaded, Linux
up, radio DECODE-READY, SSH ready, nothing typed — see
[`AUTOBOOT.md`](AUTOBOOT.md). It adds `S99bringup` (runs the bring-up last, after
SSH, in the background) and a span65 `uEnv.txt`. Installing it needs the rootfs
writable, so repair the corrupt SD first (`e2fsck`); the procedure is in that file.
