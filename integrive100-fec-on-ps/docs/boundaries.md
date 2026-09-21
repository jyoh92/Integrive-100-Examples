# Data contract at each tapped boundary

Condensed from the Integrive-100 PHY Design Specification §3.3 and §3.4. A PS
module is only usable if the format at each boundary is stated exactly — this
is that statement.

The receive chain has boundaries **B0–B7**, of which **B1–B6 are tapped**.
`B0` and `B7` are never taps, so the chain always begins and ends in fabric.

```
 digital RX -> sync -> FFT/demap -> chan.proc -> demod -> FEC -> CRC/packet
           B1      B2           B3          B4       B5     B6
```

---

## RX B1 — digital RX → synchronization

| | |
|---|---|
| Element | complex, 16-bit signed per component, Q1.15 at converter full scale after RX gain |
| Ordering | time order at the sample rate; **no frame alignment established** |
| Granularity | none — an unaligned continuous stream, a buffer holds an arbitrary window |
| Metadata | 64-bit absolute sample index of the first element, at the head |
| Length | any multiple of 64 bytes |
| Length relation | a substituted synchronizer returns whole aligned symbols, so its output is generally shorter |

**Why this boundary alone carries a timestamp:** it is the only tap upstream of
frame alignment. Without an absolute index a substituted synchronizer can say
where a frame begins *within* the buffer but not where the buffer sits in the
stream — which makes timing measurements across batches impossible and prevents
two captures from being related to each other.

## RX B2 — synchronization → FFT and demapping

| | |
|---|---|
| Element | complex, 16-bit signed, Q1.15 |
| Ordering | time order, aligned to symbol boundaries, **cyclic prefix still present** |
| Granularity | one aligned OFDM symbol including its prefix |
| Metadata | estimated CFO in Hz, 32-bit signed Q16.16, at the head |
| Length | 1050 elements, 4200 bytes |
| Length relation | the FFT stage discards the prefix and returns the used subcarriers |

## RX B3 — FFT and demapping → channel processing

| | |
|---|---|
| Element | complex, 16-bit signed, Q1.15 |
| Scaling | the FFT applies 1/N scaling, so values sit well below full scale; no renormalization |
| Ordering | subcarrier order ascending; used bins only, **pilots included**, DC and guard removed |
| Granularity | the used subcarriers of one OFDM symbol |
| Metadata | none |
| Length | 288 elements / 1152 bytes at the reference configuration |
| Length relation | channel processing consumes the 24 pilots and returns 264 equalized data subcarriers |

Pilots are present here and absent at B4. A substituted channel estimator
therefore gets what it needs, and a substituted demodulator is not burdened
with knowing the pilot pattern.

## RX B4 — channel processing → demodulator

| | |
|---|---|
| Element | complex, 16-bit signed, Q1.15 |
| Scaling | equalized to a constellation RMS of 0.25 of full scale, matching TX B3 |
| Ordering | subcarrier order ascending, **data subcarriers only** |
| Granularity | one OFDM symbol |
| Metadata | none |
| Length | 264 elements / 1056 bytes at the reference configuration |
| Length relation | the demodulator produces b bits per element, packed into GF(2⁸) symbols |

## RX B5 — demodulator → FEC decoder  ← *input of this example*

| | |
|---|---|
| Element | **unsigned 8-bit symbols over GF(2⁸), hard decisions** |
| Ordering | codeword order, matching TX B2. **No de-interleaving** — codeword boundaries fall every 255 symbols |
| Granularity | a whole number of codewords |
| Metadata | an **erasure bitmap**, one bit per symbol, immediately after the symbol array, padded to a 64-byte boundary. A set bit marks the symbol unreliable |
| Length | a multiple of 255 symbols, plus the bitmap (255 + 32 bytes per codeword) |
| Length relation | the decoder contracts by 215/255 |

**Hard decisions with erasures, not soft decisions.** RS decoding is algebraic —
Berlekamp–Massey operates on hard symbols. Soft-decision RS decoding exists but
is disproportionately expensive and almost never implemented. Passing soft
values would give the decoder information it cannot use while multiplying the
buffer by eight. The bitmap is the cheap middle ground: the code corrects **2t
erasures against t errors**, so marking a symbol unreliable rather than guessing
it doubles the effective correction capability wherever the demodulator can tell
that a subcarrier faded or that a point fell far from any reference.

## RX B6 — FEC decoder → CRC and packet decoder  ← *output of this example*

| | |
|---|---|
| Element | unsigned 8-bit bytes |
| Ordering | transmission order, matching TX B1 |
| Granularity | one decoded packet |
| Metadata | **corrected symbol count, u32, and a decode-failure flag** — at the *head* of the buffer |
| Length | header + payload_bytes + 4 bytes |
| Length relation | the CRC stage removes the header and the trailing CRC |

**Why the corrected-symbol count is carried:** so a substituted decoder reports
the same statistic the fabric decoder does. Without it, replacing the decoder
would silently stop the `fec_errors` counter, and an experiment would lose the
measurement it was probably run to obtain.

---

## Buffer sizes at the reference configuration

| Boundary | Element | Elements per unit | Bytes per unit |
|---|---|---|---|
| TX B1 | byte | header + 219 | same |
| TX B2 | byte | 255 per codeword | same |
| TX B3 | complex 32-bit | 264 | 1056 |
| TX B4 | complex 32-bit | 1050 | 4200 |
| TX B5 | complex 32-bit | multiple of 1050 | multiple of 4200 |
| RX B1 | complex 32-bit | arbitrary | arbitrary, plus 8-byte index |
| RX B2 | complex 32-bit | 1050 | 4200, plus 4-byte offset |
| RX B3 | complex 32-bit | 288 | 1152 |
| RX B4 | complex 32-bit | 264 | 1056 |
| RX B5 | byte | 255 per codeword | 255, plus 32-byte bitmap |
| RX B6 | byte | header + 219 | same, plus 8-byte header |

The largest buffer is **4200 bytes**, at TX B4 and RX B2. An 8 KB buffer
therefore holds one unit at any boundary; this example asks for 16 KB.
