# Conformance against chapter 6A

Audited 2026-09-15 by reading the specification and the implementation side by
side. Where they differ, the deviation is recorded here rather than left for
someone to discover at the bench.

## Conforms

| clause | requirement | evidence |
|---|---|---|
| 6A.4 | span must be two **adjacent** boundaries | `integrive.c`: `if (in_b != out_b + 1) return ERR_UNSUPPORTED` |
| 6A.4 | `B0` and the final boundary are not taps | rejected with `ERR_UNSUPPORTED` |
| 6A.4 | **one** PS module system-wide | the handle holds a single span slot; `set_ps_span` overwrites rather than accumulates, so an RX span replaces a TX span. A property of the data type, not a check that can be forgotten |
| 6A.4 | Stopped-only, latched at `integrive_start()` | state checked; returns `ERR_INVALID_STATE` while running |
| 6A.5 | callback invoked **once per batch** | `ps_run_batch()` per captured frame |
| 6A.5 | callback runs on an **SDK-owned thread** | `pthread_create` |
| 6A.5 | per-batch **timeout aborts the run** | `pthread_timedjoin_np` → `ERR_TIMEOUT` |
| 6A.5 | non-zero return aborts | → `ERR_ABORTED`, and `ps_span_release()` so the chain is never left stalled |
| 6A.5 | 64-byte buffer alignment | `posix_memalign(..., 64, ...)` |
| 6A.6 | the data plane **stalls** at the exit boundary | `slv_reg6[0]` holds the downstream output silent |
| 6A.6 | data **returns** at the entry boundary | 8 KB fabric buffer (2 BRAM) written through `slv_reg7`, released by a rising edge on `slv_reg6[2]` |
| 6A.8 | the decoder contracts by `k/n` | 255 symbols in, 215 out |
| 6A.12 | the stated limitations hold | by construction, per the rows above |

## Deviates

### 1. Transport is the register port, not DMA over ACP — 6A.5, 6A.11

The specification requires DMA through the ACP port into `mmap`'d buffers, with
**no copy in either direction** and no cache maintenance by the application.

The implementation allocates ordinary heap memory with `posix_memalign` and
copies bytes in through register reads and out through register writes. A few
milliseconds per frame.

This is legitimate batch-mode behaviour under 6A.9 and the callback interface is
unaffected, but the performance model of 6A.11 does not apply and no throughput
figure from this path should be compared against the document.

### 2. The erasure bitmap cannot reach the callback — PHY spec 3.3

PHY spec 3.3 places the bitmap **inside the B5 buffer**, immediately after the
symbol array, padded to 64 bytes. A substituted decoder is meant to read it
there.

It is not there, and **this cannot be fixed in the SDK**. The only way to obtain
the bitmap is `capture_frame_bytes(mode 2)`, which calls `fire_one_frame()` —
it transmits a *new* frame and captures that. The bitmap it returns belongs to a
different frame than the symbols already in hand.

Erasure marks from the wrong frame are worse than none: they tell the decoder to
discard symbols that were correct. The fabric carries one stream at a time, so
this is a **hardware limitation**, not an SDK omission. Fixing it needs a second
read path in fabric that carries the bitmap alongside the symbols of the same
frame.

**Consequence: the erasure path is unreachable.** `ps_rs_decode()` implements it
and the self-test exercises it, but at runtime the decoder always takes the
errors-only branch. Correction capability in practice is `t = 20`, not
`2f + e ≤ 40`.

> An earlier revision of this document attributed this to the SDK passing only
> the symbol array, and a patch was written to append the bitmap at the padded
> offset. That patch was wrong and was reverted: it would have paired each
> frame's symbols with a different frame's erasure marks.

### 3. `FIFO_OVERFLOW` and `FIFO_UNDERFLOW` are never set — 6A.9

6A.9 calls these the mechanism that marks a batch-mode run invalid: *"a run in
which either is set is not a valid measurement."*

Both flags are declared in `integrive.h`. Neither is ever assigned in
`integrive.c` — the only sticky flags raised are `CONFIG_ERROR`, `TX_ACTIVE`,
`RX_ACTIVE` and `SYNC_LOCK`.

**Consequence: an invalid run is indistinguishable from a valid one.** If the
converters overrun while the chain is stalled, nothing says so.

Not fixable in software either: `integrive_hw.h` shows no overflow or underflow
bit anywhere in the register map — `ITG_XPU_PHY_DIAG` carries only "chest fired"
and a group-failure count. `integrive.h` now marks both flags as unimplemented
so callers do not rely on them.

### 4. `fec_errors` reports the block that was bypassed — 6A.10

6A.10: when the FEC decoder is the substituted block, the counter *"no longer
advances and the equivalent figure must be computed by the application."*

The implementation increments `fec_errors` from the PL decoder's `pass` flag in
the same loop that runs the PS callback, regardless of whether the span is
active. The figure reported is the verdict of the decoder being bypassed.

**FIXED.** `fec_runs_on_ps()` detects the RX `(B5,B6)` span and the counter
stops advancing while it is active, which is what 6A.10 requires. The
application computes the equivalent — `examples/fec_on_ps.c` reports the
corrected-symbol count and failure count its own module produced.

### 5. One tap pair exists, not the full set — 6A.4

PHY spec 3.3 specifies `B1`–`B6` on the receive chain and the corresponding
transmit boundaries. The bitstream provides `(B5, B6)` on RX only. Any other
pair returns `ERR_UNSUPPORTED`.

## Summary

| # | deviation | fixable in software? |
|---|---|---|
| 1 | register-port transport instead of DMA over ACP | **no** — needs an AXI DMA path and the ACP port in the bitstream |
| 2 | erasure bitmap not in the B5 buffer | **no** — the fabric carries one stream at a time |
| 3 | FIFO flags never set | **no** — no such bit in the register map |
| 4 | `fec_errors` reported the bypassed decoder | **yes — fixed** |
| 5 | one tap pair instead of the full set | **no** — needs fabric work |

Four of the five are fabric limitations. Only one was an SDK defect, and it is
corrected.

## What this means for the example

`fec_on_ps.c` runs the model correctly: the chain stalls, the ARM decodes, the
chain resumes, and nothing is dropped. What it cannot do is use erasures, detect
an invalid run through the FIFO flags, or trust `fec_errors`. Those are
properties of the platform below it, not of the example.

## Hardware bring-up — 2026-09-16 (65 MHz build, cable loopback)

First time the `(B5,B6)` span ran on silicon, not simulation. Three things were
established, and one gap was found.

**The mechanism works (`ps_hello`).** The bitstream provides the `(B5,B6)` tap;
a batch of bytes leaves the chain at B5, a callback runs on the ARM, and the
result returns to the chain at B6 — proven by a latch callback that turned
`0xB2` into `0x4D` and saw it come back. Data crosses PL→PS→PL on hardware.

**The radio works (`fec_on_ps`, PHY counters).** With the bring-up in
[`../config/`](../config/README.md), every fired frame is received and the
**PL** Reed-Solomon decoder passes it: `frames received = 40/40`, `PER = 0.0000`,
reproduced across every run. 256-QAM at Fs = 15.36 MSPS over an SMA cable.

**The decoder works (`rs_selftest`).** The PS RS decoder recovers up to the full
`t = 20` on hardware — one better than the PL decoder, which cosim showed tops
out at 19 (see §... the RTL `o_fail` at 20-error codewords).

**The gap that was found, and fixed: the frame source, not the tap.** At first
the PS decoder rejected every codeword the PL decoder had just passed. The
captured 7650-byte buffer was pulled to a host: **0 of 30** codewords were
syndrome-clean under any sweep (primitive polynomial, FCR 0/1, 32-bit byte-swap,
bit-reversal, every byte offset), and it was not the decoded B6 message either.
The capture was reading *something else*.

The tap was fine; the trigger was wrong. `integrive_wait_packets` fired the DAC
(`fire_one_frame`) and then read the byte-capture RAM. The DAC transmits and
moves the frame counters, but it does not fill that RAM in step — the read
returned a stale, mid-frame window. Every working decode tool on the board fills
the RAM with the **rx_replay trigger** (`rx_intf` reg3 bit 9) instead. Selecting
the B5 stream in `reg5` and then replaying one frame makes the capture hold real
RS(255,215) codewords: **29 of 30 syndrome-clean, decoding byte-exact to the
golden payload.** On hardware, `./fec_on_ps 40` now reports **1160/1200
codewords byte-exact (96.7 %)**. The fix is in `trigger_rx_replay()` and the
`ITG_RX3_REPLAY` selector.

**What remains: the tap emits 29 codewords, not 30 (the sym10 boundary).** One
codeword in thirty — always the last, `cw#29`, in the frame's tenth OFDM
symbol — is missing. This was pinned down: the full 8192-byte capture RAM was
searched for the first bytes of every codeword's message, and `cw#0..28` each
sit at their expected `c·255` offset while `cw#29`'s bytes appear **nowhere** in
the buffer. So the coded-symbol tap (`coded_byte_o` → `b5_byte`) streams out
only 29 codewords (7395 bytes) for a 30-codeword frame; the last codeword never
reaches B5. The PL's own RS decoder still gets all 30 through an internal path
(the "sym10" fix), but that fix does not extend to the B5 tap.

The exact spot is known. `coded_byte_o` (which becomes `b5_byte`) is driven from
`sipddm_raw_byte` in `dot11.v`, and that file carries the note *"SYM10
in_last_group / demod_wd removed 2026-08-22 — BISECT … to isolate got_lock=0"*.
The tenth symbol's last-group handling was deliberately removed to fix a
`got_lock=0` regression, and the raw coded tap has streamed 29 codewords ever
since. Closing it means restoring that SYM10 last-group logic on the raw path
without reviving `got_lock=0`, then rebuilding — real RTL work with its own
validation, not a decoder or SDK change. It costs 1/30 of the payload; the 29
byte-exact codewords before it prove the path end to end.

## Reproducing the decode: a per-boot sync lottery (65 MHz build)

The 96.7 % result reproduces only when the receiver locks the replayed frame at
the **correct** position — `bytecmp_rxr3` reports `got_lock=1`, `first3=b2 3a c9`.
On most cold boots it instead locks at a **wrong, fixed** offset: `got_lock=0`,
`first3=00 0c 51`, and the coded symbols captured at B5 are then garbage (0/30
syndrome-clean, fec_on_ps 0/1200). This bad state is **deterministic within a
boot** and **insensitive to every datapath knob tried** — FFT window shift
(0–8), `bb_gain`, analog vs BIST loopback, and the openofdm hw_init are all
identical between a good and a bad boot. It changes only across a power cycle.

The signature — config-independent, fixed per boot, varying only boot-to-boot —
points at the **clock-domain crossing** the 65 MHz build carries (the 371
critical CDC findings noted in `CHAY.md`). The rx_replay→openofdm hand-off
appears to settle at a boot-phase-dependent alignment; a good phase decodes, a
bad one locks one frame-position off. This is the CDC risk that build was
flagged with, now visible.

Consequences:
- `bringup.sh` runs the lock check and prints `LOCK OK` / `LOCK POOR`, so a bad
  boot is caught before fec_on_ps is pointed at garbage. Power-cycle until
  `LOCK OK`, then the decode reproduces.
- The durable fix is to resolve the span65 CDC (RTL + rebuild) so the sync is
  boot-independent, or to build the (B5,B6) tap onto a CDC-clean base. The
  85 MHz `nochest.bin` locks deterministically but has no tap.
