# Making any block substitutable, not just the FEC decoder

Today exactly one span works: RX `(B5, B6)`. The reason is not a design
decision — it is that the mechanism was hand-wired into the output mux of
`openofdm_rx.v`, where it suppresses `dot11`'s byte stream and replays a buffer
in its place. Nothing about that generalises to another boundary.

These files are that mechanism taken apart into pieces that do.

```
ps_inject.v            holds the pipeline silent at a boundary, then emits
                       exactly len elements from the buffer the PS wrote
ps_capture.v           stores the PL stream at a boundary for the PS to read
patches/b5_span.patch  ps_inject wired into the live RX chain at B5
```

Two modules rather than one because **the ends of a span carry different
data**. Span `(B4, B5)` takes complex 32-bit samples in and returns bytes. A
single-width tap only covers spans whose ends happen to match, which is why the
first version of this was rebuilt.

Verified by `cosim/tb_ps_span.sv` at `W = 32` — the complex-sample case the
existing byte path never exercises:

| | |
|---|---|
| `en = 0` | straight wire: 64 elements through, unchanged, no added latency |
| `en = 1`, before `go` | the downstream block receives **nothing** — the pipeline genuinely stalls rather than running ahead and being ignored |
| after `go` | the downstream block receives exactly what the PS wrote, in order, the right count |
| back to `en = 0` | reconnects immediately |
| capture | all 128 elements read back and compared, not just a valid strobe counted |

## The B5 patch

`patches/b5_span.patch` inserts `ps_inject` between the bit deserializer and the
Reed–Solomon wrapper, which is where B5 actually lives in the live path:

```verilog
v39ct_rs_decode_3cw_db u_rs (
    .in_valid(rs_iv_m), .in_byte(rs_byte_m), ...   // was rs_iv / rs_byte
```

Six control ports are added to `v39ct_mm_decode` and tied to zero at both call
sites, so the span is off and the path is untouched.

**Bit-exact when disabled — measured, not assumed.** The full PHY cosim gives
the same result before and after:

```
before:  COSIM-SYM:0,...  COSIM-CW:0,...  tong=0  byte_phat=6450
after :  COSIM-SYM:0,...  COSIM-CW:0,...  tong=0  byte_phat=6450
```

Apply it with:

```bash
patch -p0 path/to/openwifi_rx_phy.v < rtl/patches/b5_span.patch
```

## Where the boundaries are in the live RX path

Found by reading `openwifi_rx_phy.v`; the build defines `V39_ACTIVE`,
`V39_STREAM_TOP` and `RX_BYTE_REORDER`, so the `v39ct_*` family is what runs —
not the plainly-named modules beside it.

| boundary | signals | block after it |
|---|---|---|
| B1 | IQ capture path | `rx_packet_detect` |
| B2 | — | `rx_fft1024_wrap` |
| B3 | — | `rx_bin_extract` |
| B4 | `demap_sym_valid`, `sym_i_demod`, `sym_q_demod` | `qam_demapper_multi` |
| **B5** | `rs_iv`, `rs_byte`, `rs_ir` in `v39ct_mm_decode` | `v39ct_rs_decode_3cw_db` |

B5 is a stream, not a register: `in_valid`/`in_byte`/`in_ready` feeding a
codeword buffer. That is why `ps_inject` drops straight in.

## Cosim, every case

Three kinds of test, run for each boundary.

### 1. Everything off — must be bit-exact

| case | result |
|---|---|
| all spans disabled | `tong=0  byte_phat=6450` — identical to the unpatched tree |

### 2. Span enabled, nothing injected — the pipeline must stall

This is the test that catches a tap wired onto a branch the build compiles out.
Such a tap builds, stays bit-exact, and does nothing — and only this test says so.

| boundary | result | meaning |
|---|---|---|
| B2 | `byte_phat=0` | injection point is on the live path |
| B3 | `byte_phat=0` | injection point is on the live path |
| B4 | `byte_phat=0` | injection point is on the live path |
| B5 | `byte_phat=0` | injection point is on the live path |

All four genuinely control the chain.

### 3. Real data injected through the PS — the message must come back

| span | block substituted | result |
|---|---|---|
| **(B4, B5)** | demodulator | **645/645 bytes match** — `tb_span_b5.sv` |
| **(B3, B4)** | channel processing | **6450/6450 bytes match** — `tb_span_b4.sv` |
| (B2, B3) | FFT and demapping | **content wrong** — see below |
| (B1, B2) | synchronization | not attempted |

`tb_span_b4.sv` is the strong one: it replays the **real B4 stream**, 7650
subcarrier decisions dumped from a full PHY run, with `dec_valid` held low.
Synchronisation, FFT, bin extraction, channel estimation and equalisation were
all supplied by software, and the message came out intact.

### Why (B2, B3) does not work yet

Replaying the captured B3 stream gives `byte_phat=6450` — a full frame's worth
of bytes — with `tong=6450`, every byte wrong. The pipeline ran; the content did
not survive.

The cause is not the injector. **B3 carries more than `{I, Q}`:**

| signal | what it is | replaced? |
|---|---|---|
| `data_i`, `data_q` | the subcarrier sample | yes |
| `data_no` | which subcarrier it is (10 bits) | **no** |
| `pilot_valid`, `pilot_no`, `pilot_i/q` | the pilot subcarriers | **no** |

Those still follow the fabric path, so they no longer line up with the injected
stream and the samples are attributed to the wrong subcarriers. Supplying them
is the remaining work — the PHY specification says as much in §3.3: at B3
"pilots included".

B4 and B5 work because their downstream is purely count-driven: a stream of
elements in order, nothing on the side.

**This is the general shape of the problem.** A boundary is substitutable when
everything crossing it is in the injected stream. The further upstream you go,
the more side-channel state there is — which is why the deepest boundaries were
also the easiest.

### Two traps this work walked into

**Patching a dormant branch.** The first B4 tap went on `qam_demapper_multi`,
which `V39_ACTIVE` compiles out. It built, stayed bit-exact, and captured
exactly zero subcarriers. The live B4 is `dec_*` entering `v39ct_mm_decode`.
Test 2 above exists because of this.

**A buffer shorter than a frame.** `ps_inject` at B4 was given `DEPTH = 4096`
against a 7650-element frame. The write pointer wrapped over the start: the last
five RS groups decoded perfectly, the first five were garbage, and nothing
warned. The symptom looked like a framing bug. `ps_inject` now reports
`len > DEPTH` in simulation.

### What was added

| level | change |
|---|---|
| `openofdm_rx.v` | `ps_ctl` from `slv_reg6/7`; `slv_reg6[6:4]` picks the entry boundary; `slv_reg5[18..20]` pick the B4/B3/B2 capture streams |
| `dot11.v` | passes `ps_ctl` down, brings three capture streams up |
| `openwifi_rx_phy.v` | `ps_inject` at B2 and B3; serialisers at B2 and B3; `ps_ctl` through both chest tops |
| `v39ct_mm_decode` | `ps_inject` at B4 and B5; serialiser at B4 |
| SDK | `rx_boundary_tapped()` reports B2–B6; the capture stream follows the span's exit boundary |

One control bus rather than six ports per level, because **one span is active at
a time** — the rule 6A.4 already enforces. A three-bit boundary index selects
where injection happens, so each boundary costs an instance, not a register set.

**Backward compatible.** The legacy mechanism is gated on `bnd == 6 || bnd == 0`,
so software written before this field behaves exactly as before.

## What is still missing

The patch proves the mechanism reaches the live path without disturbing it. It
does not yet make another block substitutable. Remaining:

1. **Side-channel signals at B2 and B3.** `data_no` and the pilot stream at B3;
   symbol timing at B2. Measured, not guessed — see the table above.
2. **The transmit chain.** Only TX `B5` is tapped, and a span needs two adjacent
   taps, so no TX substitution is possible regardless of the SDK.
3. **Rebuild and verify on hardware.** Several hours of synthesis, then
   `(B5,B6)` must be re-confirmed unchanged before trusting the new spans.
   Nothing here has run on the board — the evidence is simulation only.
