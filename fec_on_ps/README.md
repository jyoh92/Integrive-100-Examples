# FEC decoder on the ARM, the rest of the PHY in the FPGA

A working example of **PS substitution** on the Integrive-100 (Zynq-7020 +
AD9361): the Reed–Solomon decoder is replaced by C code running on the ARM PS,
while every other block of the receive chain keeps running in fabric (PL).

```
 digital RX  ->  sync  ->  FFT/demap  ->  chan. proc  ->  demod  -> [ FEC ] ->  CRC
     PL           PL           PL             PL            PL        PS         PL
                                                              B5 ^     ^ B6
```

Coded symbols leave the data plane at **RX B5**, the decoder runs on the ARM,
message bytes re-enter at **RX B6**, and every other block stays in fabric.

The whole point is to substitute **one** receive-chain block onto the PS and
measure it against real hardware, without writing a line of RTL. When the
algorithm is right, *then* it is worth the cost of implementing it in hardware.

This implements chapter 6A of the Software Platform Guide (`ITG-100-SPG-006A`)
against the boundary contract of the PHY Design Specification §3.3.

---

## The two files you edit

| file | what it is | edit it to change |
|---|---|---|
| [`examples/fec_on_ps.c`](examples/fec_on_ps.c) | the runnable harness: opens the device, sets the PS span B5→B6, installs the decoder callback, scores the output byte-exact against a golden frame | the **test / measurement** |
| [`examples/ps_rs_decode.c`](examples/ps_rs_decode.c) | the actual RS(255,215) decoder over GF(2⁸) — the function `ps_rs_decode()` | the **decode algorithm** |

Everything below walks through: **edit `fec_on_ps.c` → build → load onto the
board → run.**

---

## 1. Edit

`examples/fec_on_ps.c` is the harness. It opens the device, sets the receive
span to `(B5, B6)`, registers `ps_rs_decode` as the callback, runs a number of
packets, and prints how many decoded codewords are byte-exact against a golden
frame.

- To change **how the test runs or what is measured** (packet count, the golden
  file, which statistics are printed), edit `fec_on_ps.c`.
- To change **the decode algorithm itself**, edit `ps_rs_decode.c` (the
  `ps_rs_decode()` function). Keep the boundary contract below — get it wrong
  and the decoder runs and returns plausible garbage.

The default packet count is a command-line argument; `fec_on_ps 40` runs 40.

## 2. Build

The build **must be soft-float.** The board's rootfs loader is
`/lib/ld-linux.so.3`; a hard-float `arm-linux-gnueabihf-` binary will not load
there, and the error does not explain why.

```bash
make CROSS=arm-linux-gnueabi-      # cross-compile for the board  <-- what you want
make                               # host build: compile check + RS self-test
```

Both commands first check that their compiler can link a program using the C
library, `libm`, and pthread. If a dependency is missing on Debian/Ubuntu, the
build installs it with `apt-get` (using `sudo` when needed). The host build uses
`build-essential`; the board build uses `gcc-arm-linux-gnueabi`,
`libc6-dev-armel-cross`, and `binutils-arm-linux-gnueabi`. On other Linux
distributions, install the equivalent packages manually. Set `AUTO_INSTALL=0`
to only check dependencies without installing packages.

The board build writes to `build/`; the host build writes to `build/host/`.
Running `make` after the cross build therefore leaves `build/fec_on_ps` ready
for the board.

`make CROSS=arm-linux-gnueabi-` produces `build/fec_on_ps`. Verify the float ABI:

```bash
file build/fec_on_ps
# ... interpreter /lib/ld-linux.so.3   -> soft-float, correct
# (it must NOT say "hard-float")
```

`make` alone does a host compile and runs the decoder's self-test (`rs_selftest`),
which needs no hardware — a fast way to check a change to `ps_rs_decode.c`.

The build produces four binaries:

| binary | what it is |
|---|---|
| `fec_on_ps` | **the example** — FEC on the ARM, the rest in fabric |
| `ps_hello` | minimal check that the bitstream has a tap at all — run this first when in doubt |
| `rs_selftest` | the decoder alone: encode, inject errors, decode, verify. No hardware |
| `integrive-cli` | `info \| caps \| boundaries \| run \| ps` |

## 3. Load onto the board

Copy `build/fec_on_ps` to the board at `/root/fec_on_ps`. Over the network:

```bash
scp -O build/fec_on_ps root@192.168.200.2:/root/fec_on_ps
```

The board default login is **`root` / `root`**. It comes up at
**192.168.200.2** on a direct PC↔board link (PC side **192.168.200.1/24**).

> Ethernet on this board only links reliably at **100 Mbps** — its RX path is
> broken at 1 Gbps (the PHY is bound by the generic driver in `rgmii-id` mode
> without the RGMII skew). Force the PC to advertise 100M-only; see
> [`config/pc_eth_setup.sh`](config/pc_eth_setup.sh) and
> [`config/RUN_ETHERNET.md`](config/RUN_ETHERNET.md).

## 4. Run

From your PC, connect to the board over SSH (username: **`root`**, password: **`root`**):

```bash
ssh root@192.168.200.2
```

On a **fresh boot**, run the bring-up-and-run script on the board:

```bash
sh /root/run_fecps.sh
```

The board's run script is tracked as [`config/run_fecps.sh`](config/run_fecps.sh).
After editing it, copy it to `/root/run_fecps.sh` with `scp -O`.

It configures the AD9361 + FPGA (rf_loopback, 15.36 MSPS), loads the FIR, does
image rejection ([`khuanh.sh`](config/khuanh.sh)), then runs `./fec_on_ps 40`.
Expected result:

```
== 4. decoder on ARM ==
  RS codewords processed         : 1200
  codewords UNRECOVERABLE        : 0
  codewords BYTE-EXACT vs golden : 1200/1200  (100.0%)
== 5. PHY statistics ==
  CRC errors                     : 0
  PER                            : 0.0000
```

**1200/1200 byte-exact** was achieved on the hardware: the message recovered on
the ARM matches the transmitted frame for all 30 codewords per frame. This is
FEC running on the PS while every other block runs in the PL.

Requirements and caveats:

- **The RF cable must be connected** (SMA, TX↔RX). TX is fixed at **−27 dBm** in
  the script — do **not** raise it; that is a hardware safety limit for a cabled
  loopback.
- **Always run the script on a fresh boot.** Running `./fec_on_ps` directly
  without the AD9361/FPGA bring-up yields garbage.
- The AD9361 lock is non-deterministic across boots. The bring-up prints a lock
  check; if it reports a poor lock, **power-cycle** and run the bring-up once
  more (re-running the autoconfig on the same boot makes it worse). See
  [`config/README.md`](config/README.md).

The full step-by-step procedures are in
[`config/RUN_FECPS.md`](config/RUN_FECPS.md) (over UART) and
[`config/RUN_ETHERNET.md`](config/RUN_ETHERNET.md) (over SSH).

### The `fec_errors` / BER numbers are wrong on the PS

In section 5 of the output, `fec_errors` and `BER` are **wrong** while the FEC
runs on the PS: the FPGA still bumps those counters from the block being
substituted. The real metric is the **byte-exact count in section 4**. This
caveat is printed by the program and is documented in
[`docs/conformance.md`](docs/conformance.md) (6A.10).

---

## The boundary contract

Full text in [`docs/boundaries.md`](docs/boundaries.md). The short version —
because getting any of it wrong yields a decoder that runs and returns
believable garbage:

**In, at RX B5** — 255 unsigned GF(2⁸) symbols per codeword, hard decisions,
codeword order, **no de-interleaving**. Optionally followed by an erasure
bitmap: one bit per symbol, LSB-first, padded to a 64-byte boundary; a set bit
marks the symbol unreliable.

**Out, at RX B6** — an 8-byte header `{u32 corrected_symbols, u32
decode_failed}` followed by 215 message bytes per codeword, transmission order.

That header is not decoration: the FPGA decoder is what feeds the `fec_errors`
counter, so a substituted decoder that omitted it would silently stop the very
measurement the experiment was run to obtain.

**Field conventions:** primitive polynomial `0x11D`, `alpha = 2`, first
consecutive root `FCR = 0`, so syndrome `S_i = r(alpha^i)`. These match the RTL
exactly and are stated in the source rather than assumed, because all three are
invisible when wrong — the decoder still runs and the data is nonsense.

**Hard decisions with erasures, not soft decisions.** RS decoding is algebraic;
Berlekamp–Massey operates on hard symbols. The erasure bitmap is the cheap
middle ground: the code corrects **2t erasures against t errors**.

---

## Repository layout

```
examples/fec_on_ps.c      THE example: configures the PHY, sets the span, runs
                          the decoder on the ARM, reports its own statistics
                          and the PHY's
examples/ps_rs_decode.c   the decoder: RS(255,215) over GF(2^8), erasure-aware
examples/ps_hello.c       minimal tap check -- run this first when in doubt
examples/integrive-cli.c  info | caps | boundaries | run | ps
include/integrive.h       the public interface
src/integrive.c           SDK implementation
src/integrive_hw.[ch]     the only place register offsets and sysfs paths live
docs/boundaries.md        the data contract at every tapped boundary
docs/conformance.md       clause-by-clause audit against chapter 6A
config/                   board bring-up scripts + run procedures
rtl/                      RTL to make other boundaries substitutable (patches + modules)
cosim/                    software decoder vs RTL decoder, same vectors
```

---

## Equivalence: the software decoder against the RTL one

Substitution is only legitimate if the PS module behaves like the fabric block
it replaces — so the two are compared byte for byte on the same vectors. The
RTL is **not** vendored here; point `RTL_DIR` at it.

```bash
./cosim/run_cosim.sh 44                       # quick pass, mixed error counts
./cosim/run_cosim.sh 32000 -1 424242 14 21    # a full run -- ~12 min
python3 cosim/analyze.py /tmp/rs_cosim        # per-level + history cross-table
./cosim/campaign.sh 20 50                     # many seeds, separate runs
./cosim/sweep.sh 20                           # sweep the error count around the bound
```

`run_cosim.sh` generates codewords, runs **both** decoders on the same bytes —
the C one in process, the RTL one under `xsim` — and diffs the message output.
Grouped by the **true** error count, the software and RTL decoders agree
**exactly, every byte, through 19 errors** on tens of thousands of codewords. At
20 errors (which is `t`), the RTL decoder fails on all of them while the
software decoder recovers all of them:

**The usable capability of the fabric decoder is `t − 1 = 19`, not `t = 20`.**
Substituting the block recovers the missing symbol of correction capability. For
a link operating near the bound, that is the difference between a frame that
decodes and one that does not.

The comparison deliberately does **not** require agreement beyond `t` (both
implementations may legally miscorrect or declare failure), and the RTL has no
erasure input, so the erasure path (`2f + e ≤ 40`) is exercised only in the
software self-test.

---

## Rules the SDK enforces

Not advisory — the API rejects violations.

| rule | meaning |
|---|---|
| Taps are at **block boundaries**, never inside a block | a chain of N blocks has N+1 boundaries |
| A span is **two adjacent boundaries** — exactly one block | a multi-block span is rejected |
| `B0` and the final boundary are **not** taps | the chain always begins and ends in fabric |
| **One PS module system-wide** | a TX and an RX substitution cannot both be active |
| The span is **Stopped-only** | latched at `integrive_start()`; changing it while running returns `ERR_INVALID_STATE` |

---

## Hardware and access

| | |
|---|---|
| Board | Zynq-7020 (xc7z020) + AD9361 |
| Board IP | `192.168.200.2`, static, no DHCP |
| Host | `192.168.200.1/24` on the directly-connected port |
| Login | `root` / `root` |
| Ethernet | links reliably at 100 Mbps only (see `config/RUN_ETHERNET.md`) |
| Rootfs | Buildroot / busybox, **soft-float** |

---

## Troubleshooting

- **`fec_on_ps` cannot open the device.** It must run **on the board, as root,
  with the bitstream loaded**.
- **The callback is never invoked / results are garbage.** You almost certainly
  ran `./fec_on_ps` without the bring-up. Run `sh /root/run_fecps.sh` on a fresh
  boot instead. If it still fails, run `ps_hello` first — it answers the one
  question worth asking before blaming the decoder: does the loaded bitstream
  provide a tap at `(B5, B6)` at all?
- **Binary will not load on the board** (`no such file or directory` for an
  existing file). It was built hard-float. Rebuild with
  `make CROSS=arm-linux-gnueabi-` and check `file build/fec_on_ps`.
- **`0/N` sync, or 0/1200 byte-exact.** The AD9361 lock is per-boot and
  non-deterministic. Power-cycle and run the bring-up once (not the autoconfig
  again). See [`config/README.md`](config/README.md).
- **`fec_errors` / `BER` look wrong.** They are, on the PS — see the caveat
  above. Trust the byte-exact count in section 4.
- **Ethernet "up" but 100% packet loss.** The link negotiated 1 Gbps. Force
  100 Mbps on the PC (`config/pc_eth_setup.sh`).

---

## Conformance and status

The model conforms to chapter 6A (adjacency, the single system-wide span,
Stopped-only latching, the SDK-owned callback thread, the enforced timeout).
Audited clause by clause in [`docs/conformance.md`](docs/conformance.md), which
also records the deviations that are fabric limitations (register-port transport
instead of DMA over ACP; the erasure bitmap not reaching the callback; the FIFO
flags never set; only the `(B5, B6)` tap existing so far).

The `(B5, B6)` span has run on real hardware: `sh /root/run_fecps.sh` reports
**1200/1200 codewords byte-exact (100.0%)**, CRC errors 0, unrecoverable
codewords 0, reproducibly.

## License

**Not set.** Add one before publishing — with no license file, default
copyright applies and nobody may legally use this.
