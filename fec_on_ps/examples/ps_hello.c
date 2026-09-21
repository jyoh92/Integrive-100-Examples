/* =========================================================================
 * ps_hello.c -- the SMALLEST example of running a substituted block on the PS
 *               (Guide 6A).
 *
 * Purpose: PROVE the mechanism, not implement an algorithm.  The callback here
 * only copies in -> out and FLIPS the BITS of the first byte.  If that byte
 * comes back changed downstream, the data really did travel through the ARM
 * and return to the PL -- there is no other way to explain it.
 *
 * Unlike ps_rs_decode.c: that file has a main() but only SELF-TESTS the decoder
 * in software, never touching the board.  This one runs on real hardware.
 *
 * Build for the board (soft-float rootfs, MUST use gueabi- without "hf"):
 *   cd sdk && make CROSS=arm-linux-gnueabi- examples/ps_hello
 *
 * Run:
 *   ./ps_hello            # flip the bit -> prove the data went through the PS
 *   ./ps_hello --y-nguyen # copy verbatim -> check byte-for-byte fidelity
 * ========================================================================= */
#include "integrive.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define SPAN_RA   5      /* RX B5: coded bytes leaving the pipeline    */
#define SPAN_VAO  6      /* RX B6: message bytes re-entering the pipeline */
#define SO_LO     20     /* number of batches to run */
#define BODEM     16384  /* bytes, enough for a 6450-byte frame */

struct dem {
    unsigned long lan;         /* times the callback was invoked      */
    unsigned long byte_vao;    /* total bytes received from the PL    */
    unsigned long byte_ra;     /* total bytes returned to the PL      */
    int           lat_bit;     /* 1 = flip the first byte to prove it */
    uint8_t       dau_vao;     /* first byte of the last batch, before the change */
    uint8_t       dau_ra;      /* first byte of the last batch, after the change  */
};

/* ★ The callback runs on the SDK'S THREAD, not the thread that called start().
 *   The in/out pointers are valid only during this call -- do not keep them. */
static int module_ps(const void *in, size_t in_len,
                     void *out, size_t out_capacity,
                     size_t *out_len, void *user)
{
    struct dem *d = user;
    const uint8_t *v = in;
    uint8_t *r = out;

    if (in_len > out_capacity)
        return -1;                     /* non-zero => abort the run */

    memcpy(r, v, in_len);
    if (d->lat_bit && in_len > 0)
        r[0] = (uint8_t)(v[0] ^ 0xFF); /* the PS's fingerprint */

    d->lan++;
    d->byte_vao += in_len;
    d->byte_ra  += in_len;
    if (in_len > 0) { d->dau_vao = v[0]; d->dau_ra = r[0]; }

    *out_len = in_len;
    return 0;
}

static void bao(const char *viec, integrive_status_t st)
{
    printf("  %-34s %s\n", viec,
           st == INTEGRIVE_OK ? "OK" : integrive_strerror(st));
}

int main(int argc, char **argv)
{
    integrive_device_t *dev = NULL;
    integrive_status_t  st;
    struct dem d;
    integrive_range_t cap;
    uint32_t n_bien;
    integrive_state_t trangthai;

    memset(&d, 0, sizeof d);
    d.lat_bit = 1;
    if (argc > 1 && strcmp(argv[1], "--y-nguyen") == 0) d.lat_bit = 0;

    printf("== 1. open device ==\n");
    st = integrive_open(&dev);
    if (st != INTEGRIVE_OK) {
        printf("  CANNOT open: %s\n", integrive_strerror(st));
        printf("  Must run ON THE BOARD, as root, with the bitstream loaded.\n");
        return 1;
    }
    integrive_get_state(dev, &trangthai);
    printf("  state = %d\n", (int)trangthai);

    printf("\n== 2. does this bitstream support a span ==\n");
    if (integrive_get_capability(dev, INTEGRIVE_CAP_PS_BUFFER_BYTES, &cap) == INTEGRIVE_OK)
        printf("  max PS buffer               : %.0f bytes\n", cap.max);
    if (integrive_get_boundary_count(dev, INTEGRIVE_CHAIN_RX, &n_bien) == INTEGRIVE_OK)
        printf("  RX chain boundary count     : %u  (B0..B%u)\n", n_bien, n_bien ? n_bien-1 : 0);

    st = integrive_set_ps_span(dev, INTEGRIVE_CHAIN_RX, SPAN_RA, SPAN_VAO);
    bao("set span RX (B5,B6)", st);
    if (st != INTEGRIVE_OK) {
        printf("\n  The loaded bitstream has NO tap point at this boundary.\n");
        printf("  Load a build with Guide 6A support (slv_reg6/7 in openofdm_rx).\n");
        integrive_close(dev);
        return 2;
    }

    printf("\n== 3. configure (all Stopped-only parameters) ==\n");
    bao("buffer size",       integrive_set_ps_buffer_bytes(dev, BODEM));
    bao("per-batch timeout", integrive_set_ps_timeout_ms(dev, 200));
    bao("register callback", integrive_set_ps_callback(dev, module_ps, &d));

    printf("\n== 4. run %d batches ==\n", SO_LO);
    printf("  mode: %s\n", d.lat_bit ? "FLIP the first byte (prove it went through the PS)"
                                       : "copy VERBATIM (check byte-exact)");
    st = integrive_start(dev);
    bao("start", st);
    if (st != INTEGRIVE_OK) { integrive_close(dev); return 3; }

    st = integrive_wait_packets(dev, SO_LO, 30000);
    bao("wait for batches", st);
    bao("stop", integrive_stop(dev));

    printf("\n== 5. results ==\n");
    printf("  callback invoked            : %lu times\n", d.lan);
    printf("  bytes received from PL      : %lu\n", d.byte_vao);
    printf("  bytes returned to PL        : %lu\n", d.byte_ra);
    if (d.lan) {
        printf("  first byte of last batch    : PL sent 0x%02X -> PS returned 0x%02X%s\n",
               d.dau_vao, d.dau_ra,
               d.lat_bit ? "   (flipped)" : "   (unchanged)");
    }

    if (d.lan == 0) {
        printf("\n  ★ The callback was NEVER invoked.  The pipeline did not stall at the exit\n");
        printf("    boundary, or no frame arrived.  Check RF and the PHY configuration first.\n");
        integrive_close(dev); return 4;
    }

    printf("\n  ★ The callback ran %lu times on the ARM, and the PL STALLED for each one.\n", d.lan);
    if (d.lat_bit)
        printf("    The first byte changing 0x%02X -> 0x%02X proves the data went through the PS and back to the PL.\n",
               d.dau_vao, d.dau_ra);

    integrive_close(dev);
    return 0;
}
