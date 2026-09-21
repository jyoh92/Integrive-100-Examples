/* =========================================================================
 * fec_on_ps.c -- FEC decoder on the ARM, every other PHY block in the FPGA.
 *
 * This is the example the platform exists to make possible: swap ONE block
 * of the receive chain for C code, leave the rest in fabric, and measure the
 * result with the same statistics the all-FPGA path reports.
 *
 *   digital RX -> sync -> FFT/demap -> chan.proc -> demod -> [FEC] -> CRC
 *        PL        PL        PL           PL         PL       PS      PL
 *                                                   B5 ^      ^ B6
 *
 * Coded GF(2^8) symbols leave the data plane at RX B5, rs_decode() runs on
 * the ARM, and message bytes re-enter at RX B6.  The chain STALLS at B5 for
 * the duration of the call, so nothing is dropped -- this is batch mode
 * (Guide 6A.3), not streaming with a penalty.
 *
 * Boundary contract, PHY spec 3.3 -- getting any of it wrong yields a decoder
 * that runs and returns plausible garbage:
 *
 *   in   at B5   255 unsigned GF(2^8) symbols per codeword, hard decisions,
 *                codeword order, NO de-interleaving.  Optionally followed by
 *                an erasure bitmap, one bit per symbol, LSB-first, padded to
 *                a 64-byte boundary; a set bit marks the symbol unreliable.
 *   out  at B6   an 8-byte header {u32 corrected_symbols, u32 decode_failed}
 *                followed by 215 message bytes per codeword, transmission
 *                order.
 *
 * The header is not decoration.  The FPGA decoder feeds the fec_errors
 * counter; a substituted decoder that omitted it would silently stop the very
 * measurement the experiment was run to obtain.
 *
 * Build:  make CROSS=arm-linux-gnueabi-
 * Run:    /tmp/fec_on_ps [packets]
 * ========================================================================= */
#include "integrive.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* --------------------------------------------------------------------------
 * 1) Configuration of the PS-side FEC replacement span
 * --------------------------------------------------------------------------
 * - CHAIN = RX: we are attaching the callback on the receive path.
 * - B_START = 5: FEC input at RX boundary B5 (RS symbols from the demodulator).
 * - B_END   = 6: FEC output at RX boundary B6 (decoded payload).
 * - BUFSZ   = 16384: buffer large enough for the largest frame/payload seen at
 *   any PHY boundary.
 * - TIMEOUT_MS: timeout for a batch or packet before reporting an error.
 * - DEFAULT_PACKETS: default number of packets if no command-line argument is
 *   provided.
 *
 * Meaning: the “FEC on PS” mode is enabled by setting the span (B5, B6) and
 * registering a callback that processes data crossing this boundary.
 * -------------------------------------------------------------------------- */
#define CHAIN            INTEGRIVE_CHAIN_RX
#define B_START          5        /* demodulator      -> FEC decoder */
#define B_END            6        /* FEC decoder      -> CRC / packet decoder */
#define BUFSZ            16384    /* >= 4200, the largest buffer at any boundary */
#define TIMEOUT_MS       200
#define DEFAULT_PACKETS  50

/* --------------------------------------------------------------------------
 * 2) Declaration of the RS decoder implemented in ps_rs_decode.c
 * --------------------------------------------------------------------------
 * This function receives data at RX B5, performs RS decoding on the ARM, and
 * returns the decoded output at RX B6 according to the boundary contract.
 *
 * - in_len: number of input bytes, organized as 255-symbol codewords.
 * - out: output buffer that contains metadata header + payload.
 * - out_capacity: size of the output buffer.
 * - out_len: actual number of bytes written to out.
 * -------------------------------------------------------------------------- */
extern int ps_rs_decode(const void *in, size_t in_len,
                        void *out, size_t out_capacity,
                        size_t *out_len, void *user);

/* --------------------------------------------------------------------------
 * 3) Metadata header that the decoder must write at the start of out
 * --------------------------------------------------------------------------
 * According to the PHY contract, the output at RX B6 is not just payload; it
 * also includes:
 * - corrected_symbols: number of symbols repaired
 * - decode_failed: flag indicating that a codeword could not be decoded
 *
 * This header is critical because it allows the FPGA side to update the
 * fec_errors counter and related statistics correctly even when the decoder is
 * replaced by software.
 * -------------------------------------------------------------------------- */
typedef struct { uint32_t corrected_symbols; uint32_t decode_failed; } meta_t;

#define RS_K 215u                    /* message bytes per codeword */

/* --------------------------------------------------------------------------
 * 4) Statistics used to evaluate the ARM-side decoder
 * --------------------------------------------------------------------------
 * struct stats is not used to measure CPU speed; it tracks the real quality of
 * the decode:
 * - number of batches passed to the callback by the SDK,
 * - total codewords decoded,
 * - number of symbols corrected,
 * - number of codewords that could not be recovered,
 * - number of output bytes,
 * - number of codewords that match the golden frame exactly (byte-exact).
 *
 * This is the real test of the experiment: not only whether the decoder runs,
 * but whether it produces the correct payload sent by the transmitter.
 * -------------------------------------------------------------------------- */
struct stats {
    unsigned long batches;       /* batches the SDK handed us          */
    unsigned long codewords;     /* codewords seen                     */
    unsigned long corrected;     /* symbols the decoder repaired       */
    unsigned long failed;        /* codewords it could not repair      */
    unsigned long bytes_out;
    unsigned long matched;       /* codewords byte-exact vs the golden */
    unsigned long compared;      /* codewords compared against golden  */
    const unsigned char *golden; /* one frame of reference payload     */
    size_t         golden_len;
};

/* --------------------------------------------------------------------------
 * 5) Callback invoked by the SDK when batched data reaches B5/B6
 * --------------------------------------------------------------------------
 * When a batch passes through the span B5→B6, the SDK invokes this callback.
 * In this stage it:
 * - receives the input buffer from the FPGA,
 * - calls ps_rs_decode() on the ARM,
 * - reads the result metadata at the beginning of the output buffer,
 * - compares the decoded payload against the golden reference to compute the
 *   byte-exact pass rate.
 *
 * If the decoder returns an error, the run is aborted. Otherwise, processing
 * continues normally.
 * -------------------------------------------------------------------------- */
static int fec_decode_cb(const void *in, size_t in_len,
                        void *out, size_t out_capacity,
                        size_t *out_len, void *user)
{
    struct stats *s = user;
    int rc = ps_rs_decode(in, in_len, out, out_capacity, out_len, NULL);

    if (rc != 0)
        return rc;                        /* non-zero aborts the run */

    s->batches++;
    s->codewords += in_len / 255u;
    s->bytes_out += *out_len;
    if (*out_len >= sizeof(meta_t)) {
        const meta_t *m = out;
        s->corrected += m->corrected_symbols;
        s->failed    += m->decode_failed;
    }
    /* If a golden frame was supplied, score the decode where it matters: does
       the recovered message equal the transmitted one, codeword by codeword?
       This is the byte-exact verdict the decode_failed flag alone does not give. */
    if (s->golden && *out_len > sizeof(meta_t)) {
        const unsigned char *msg = (const unsigned char *)out + sizeof(meta_t);
        size_t mlen = *out_len - sizeof(meta_t);
        size_t off;
        for (off = 0; off + RS_K <= mlen && off + RS_K <= s->golden_len; off += RS_K) {
            s->compared++;
            if (memcmp(msg + off, s->golden + off, RS_K) == 0)
                s->matched++;
        }
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * 6) Helper to print the status of each setup/startup step
 * --------------------------------------------------------------------------
 * This function prints a clear label and either OK or the specific error string.
 * -------------------------------------------------------------------------- */
static int step(const char *label, integrive_status_t st)
{
    printf("  %-32s %s\n", label,
           st == INTEGRIVE_OK ? "OK" : integrive_strerror(st));
    return st == INTEGRIVE_OK;
}

/* --------------------------------------------------------------------------
 * 7) Main function: the center of the FEC-on-PS demo
 * --------------------------------------------------------------------------
 * Main performs five major tasks:
 * 1. Open the device and confirm the bitstream is loaded.
 * 2. Configure the RX span (B5, B6) to replace the FEC block on the PS.
 * 3. Set the buffer size, timeout, and decoder callback.
 * 4. Start the chain and wait for packets to be processed.
 * 5. Print the decode and PHY statistics for evaluation.
 *
 * Each step matters:
 * - No device → cannot run on the board.
 * - Wrong span configuration → the callback is never invoked.
 * - No golden frame → there is no true byte-exact verification.
 * - Incorrect stats interpretation → PL and PS metrics can be confused.
 * -------------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    integrive_device_t *dev = NULL;
    integrive_status_info_t info;
    integrive_version_t ver;
    integrive_status_t st;
    struct stats s;
    static unsigned char golden[8192];
    uint64_t packets = (argc > 1) ? strtoull(argv[1], NULL, 10) : DEFAULT_PACKETS;

    memset(&s, 0, sizeof s);

    /* ----------------------------------------------------------------------
     * 7.1 Load the golden frame for real decode verification
     * ----------------------------------------------------------------------
     * The golden file contains the original payload transmitted by the sender.
     * If present, the program compares each decoded 215-byte codeword against
     * the original payload and reports the byte-exact pass rate.
     *
     * Priority order:
     * 1) second argument in argv
     * 2) GOLDEN environment variable
     * 3) default /root/golden6450.bin on the board
     * ---------------------------------------------------------------------- */
    {
        const char *gp = (argc > 2) ? argv[2]
                       : getenv("GOLDEN") ? getenv("GOLDEN")
                       : "/root/golden6450.bin";
        FILE *gf = fopen(gp, "rb");
        if (gf) {
            size_t n = fread(golden, 1, sizeof golden, gf);
            fclose(gf);
            if (n > 0) { s.golden = golden; s.golden_len = n; }
        }
    }

    /* ----------------------------------------------------------------------
     * 7.2 Open the device and confirm the bitstream was loaded
     * ---------------------------------------------------------------------- */
    printf("== 1. device ==\n");
    st = integrive_open(&dev);
    if (st != INTEGRIVE_OK) {
        printf("  cannot open: %s\n", integrive_strerror(st));
        printf("  Must run ON THE BOARD, as root, with the bitstream loaded.\n");
        return 1;
    }
    if (integrive_get_version(dev, &ver) == INTEGRIVE_OK)
        printf("  bitstream: %s\n", ver.bitstream_id);

    /* ----------------------------------------------------------------------
     * 7.3 Configure the RX span (B5,B6): FEC runs on PS, the rest stays in PL
     * ----------------------------------------------------------------------
     * This is the actual block substitution step. By setting the span from B5 to
     * B6, the SDK forces the stream to pass through the ARM callback instead of
     * the FPGA FEC block. If the bitstream has no tap at (B5,B6), the run stops
     * immediately.
     * ---------------------------------------------------------------------- */
    printf("\n== 2. set span: FEC on PS, the rest on PL ==\n");
    st = integrive_set_ps_span(dev, CHAIN, B_START, B_END);
    if (!step("span RX (B5,B6)", st)) {
        printf("\n  This bitstream has NO tap point at (B5,B6).\n");
        printf("  Run examples/ps_hello first to see which boundaries it supports.\n");
        integrive_close(dev);
        return 2;
    }
    step("buffer",   integrive_set_ps_buffer_bytes(dev, BUFSZ));
    step("timeout",  integrive_set_ps_timeout_ms(dev, TIMEOUT_MS));
    step("callback = RS decoder", integrive_set_ps_callback(dev, fec_decode_cb, &s));

    /* ----------------------------------------------------------------------
     * 7.4 Start RX and wait for the requested number of packets
     * ----------------------------------------------------------------------
     * integrive_start() actually arms the span, allocates the batch buffers, and
     * puts the chain into the running state. The program then waits until the
     * requested number of packets has passed through the decoder.
     * ---------------------------------------------------------------------- */
    printf("\n== 3. try %llu packets ==\n", (unsigned long long)packets);
    integrive_clear_statistics(dev);
    /* integrive_start() is what actually arms the span: it allocates the batch
       buffers, resets the chain and enables the stall (state -> RUNNING).
       start_rx() only sets a bookkeeping flag and REQUIRES the chain already be
       RUNNING, so calling it alone left the span disarmed and start refused. */
    if (!step("start", integrive_start(dev))) { integrive_close(dev); return 3; }

    st = integrive_wait_packets(dev, packets, 60000);
    step("wait for packets", st);
    step("stop", integrive_stop(dev));

    /* ----------------------------------------------------------------------
     * 7.5 Print the statistics from the decoder running on the ARM
     * ----------------------------------------------------------------------
     * This is the most important section: it reports the values produced by the
     * PS-side decoder, including processed codewords, repaired symbols, failed
     * codewords, and the byte-exact match rate against the golden frame.
     * ---------------------------------------------------------------------- */
    printf("\n== 4. decoder on ARM ==\n");
    printf("  batches delivered        : %lu\n", s.batches);
    printf("  RS codewords processed   : %lu\n", s.codewords);
    printf("  symbols corrected        : %lu\n", s.corrected);
    printf("  codewords UNRECOVERABLE  : %lu\n", s.failed);
    printf("  bytes returned to PL     : %lu\n", s.bytes_out);
    if (s.compared) {
        printf("  codewords BYTE-EXACT vs golden: %lu/%lu  (%.1f%%)\n",
               s.matched, s.compared, 100.0 * (double)s.matched / (double)s.compared);
        printf("    * this is the real metric: the decoded message MATCHES what was transmitted.\n");
    }

    if (s.batches == 0) {
        printf("\n  * Callback was never invoked -- no packets arrived, or\n");
        printf("    the pipeline did not stall at B5.  Check RF before suspecting 6A.\n");
        integrive_close(dev);
        return 4;
    }

    /* ----------------------------------------------------------------------
     * 7.6 Print the PHY / FPGA statistics
     * ----------------------------------------------------------------------
     * These values come from the receive chain as reported by the FPGA. However,
     * once the FEC block is replaced by the PS, fec_errors and BER are no longer
     * the correct quality metric because they are still derived from the FPGA
     * logic that was substituted. For this reason, the byte-exact result in
     * section 4 is the authoritative metric.
     * ---------------------------------------------------------------------- */
    printf("\n== 5. PHY statistics ==\n");
    if (integrive_get_status(dev, &info) == INTEGRIVE_OK) {
        printf("  packets received         : %llu\n", (unsigned long long)info.rx_packets);
        printf("  CRC errors               : %llu\n", (unsigned long long)info.crc_errors);
        printf("  PER                      : %.4f\n", info.per);
        printf("  SNR                      : %.1f dB\n", info.snr_db);
        /* * fec_errors is WRONG while the FEC runs on the PS.  Section 6A.10 says
           this counter must FREEZE and the application must track its own, yet the
           SDK still bumps it from the `pass` of the decoder IN FABRIC -- i.e. the
           very block being substituted.  Printing it without a caveat is worse
           than not printing it: the number looks plausible but describes the wrong
           thing.  The correct number is in section 4 above. */
        printf("  fec_errors               : %llu  <- N/A: FPGA FEC is bypassed when decoding runs on PS\n",
               (unsigned long long)info.fec_errors);
        /* BER also depends on the decoder, so it is likewise invalid in this mode. */
        if (info.ber_valid)
            printf("  BER                      : %.3e  <- also follows the PL path\n", info.ber);
    }

    /* ----------------------------------------------------------------------
     * 7.7 Final takeaway for the user
     * ----------------------------------------------------------------------
     * When the program ends, the user should understand that:
     * - the decoder executed on the ARM,
     * - the rest of the receive chain still runs in FPGA fabric,
     * - the key metric to trust is the byte-exact match against the golden.
     * ---------------------------------------------------------------------- */
    printf("\n  * %lu codewords were just decoded on the ARM, not in the FPGA.\n", s.codewords);
    printf("    Every other block of the receive chain still runs in fabric as usual.\n");

    integrive_close(dev);
    return 0;
}
