/* =========================================================================
 * gen_vectors.c -- generate test vectors and run the SOFTWARE decoder on them.
 *
 * Purpose: put the C decoder (which will run on the PS) alongside the RTL
 * decoder (running in the PL) over the SAME data.  Substituting the block is
 * only legitimate if the two give IDENTICAL results on the errors-only path.
 *
 * ★ The RTL has NO erasure support (no erasure port).  So the vectors here mark
 *   no erasures -- the comparison runs on the shared capability: t = 20 errors.
 *   The PS module is stronger (2f + e <= 40) but that part cannot be compared.
 *
 * Produces:
 *   coded.hex     255 bytes per line, one line per codeword  -> for the RTL testbench
 *   expect.hex    215 bytes per line, the correct message
 *   sw_out.hex    215 bytes per line, the C DECODER'S RESULT
 *   sw_flags.txt  one line: "<corrected symbols> <failed 0/1>"
 * ========================================================================= */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define RS_N 255
#define RS_K 215

extern int    ps_rs_decode(const void *in, size_t in_len, void *out,
                           size_t out_capacity, size_t *out_len, void *user);
typedef struct { uint32_t corrected_symbols; uint32_t decode_failed; } meta_t;

/* RS(255,215) encoder -- same conventions as the RTL: polynomial 0x11D, alpha = 2, FCR = 0 */
static uint8_t ex[512], lg[256];
static void gf_init(void)
{
    unsigned x = 1, i;
    for (i = 0; i < 255; i++) { ex[i] = (uint8_t)x; lg[x] = (uint8_t)i;
        x <<= 1; if (x & 0x100) x ^= 0x11D; }
    for (i = 255; i < 512; i++) ex[i] = ex[i - 255];
}
static uint8_t mul(uint8_t a, uint8_t b)
{ return (!a || !b) ? 0 : ex[lg[a] + lg[b]]; }

static void encode(const uint8_t *msg, uint8_t *cw)
{
    uint8_t gen[RS_N - RS_K + 1];
    unsigned i, j, nsym = RS_N - RS_K;
    memset(gen, 0, sizeof gen); gen[0] = 1;
    for (i = 0; i < nsym; i++) {
        for (j = i + 1; j > 0; j--) gen[j] ^= mul(gen[j - 1], ex[i]);
    }
    memcpy(cw, msg, RS_K);
    memset(cw + RS_K, 0, nsym);
    for (i = 0; i < RS_K; i++) {
        uint8_t f = cw[i] ^ 0;
        uint8_t s = (uint8_t)(cw[i] ^ (i ? 0 : 0));
        (void)f; (void)s;
    }
    /* long division: discard the quotient, the remainder is the parity */
    {
        uint8_t tmp[RS_N]; memcpy(tmp, cw, RS_N);
        for (i = 0; i < RS_K; i++) {
            uint8_t c = tmp[i];
            if (c) for (j = 1; j <= nsym; j++) tmp[i + j] ^= mul(gen[j], c);
        }
        memcpy(cw + RS_K, tmp + RS_K, nsym);
    }
}

static void ghi_dong(FILE *f, const uint8_t *p, size_t n)
{ size_t i; for (i = 0; i < n; i++) fprintf(f, "%02X", p[i]); fputc('\n', f); }

int main(int argc, char **argv)
{
    unsigned ncw = (argc > 1) ? (unsigned)atoi(argv[1]) : 24u;
    unsigned seed;
    /* argv[2]: fixed error count per codeword.  Empty = ramp 0..21.
       Used to SWEEP around the bound t = 20, where the two implementations
       diverge most. */
    int      nerr_co_dinh = (argc > 2) ? atoi(argv[2]) : -1;
    /* argv[4],argv[5]: sweep the error level from LO to HI, cycling per codeword.
       ★ Codewords within ONE generation are ALREADY independent: the generator
       runs its LCG continuously so each codeword carries fresh data.  So many
       seeds are not needed -- only MANY CODEWORDS in a single simulation run. */
    int      lo = (argc > 4) ? atoi(argv[4]) : -1;
    int      hi = (argc > 5) ? atoi(argv[5]) : -1;
    /* argv[3]: seed.  ★ Fixing the seed means a re-run just replays the same
       vectors -- it adds no evidence.  A new seed is a genuinely independent
       trial. */
    seed = (argc > 3) ? (unsigned)strtoul(argv[3], NULL, 10) : 20260914u;
    unsigned c;
    FILE *fc, *fe, *fo, *ff, *fn;
    uint8_t *coded = malloc((size_t)ncw * RS_N);
    uint8_t *out   = malloc(sizeof(meta_t) + (size_t)ncw * RS_K);
    uint8_t (*want)[RS_K] = malloc((size_t)ncw * RS_K);
    size_t out_len = 0;
    int rc;

    gf_init();
    fc = fopen("coded.hex", "w"); fe = fopen("expect.hex", "w");
    fo = fopen("sw_out.hex", "w"); ff = fopen("sw_flags.txt", "w");
    fn = fopen("nerr.txt", "w");
    if (!fc || !fe || !fo || !ff || !fn) { perror("open file"); return 1; }

    /* Error count ramps 0..t: covers the clean, correctable, and over-capacity cases. */
    for (c = 0; c < ncw; c++) {
        uint8_t msg[RS_K], cw[RS_N], sach[RS_N];
        unsigned i, nerr;
        if (lo >= 0 && hi >= lo)      nerr = (unsigned)(lo + (int)(c % (unsigned)(hi - lo + 1)));
        else if (nerr_co_dinh >= 0)   nerr = (unsigned)nerr_co_dinh;
        else                          nerr = c % 22u;
        for (i = 0; i < RS_K; i++) {
            seed = seed * 1103515245u + 12345u; msg[i] = (uint8_t)(seed >> 16);
        }
        encode(msg, cw);
        memcpy(want[c], msg, RS_K);
        { unsigned q; for (q = 0; q < RS_N; q++) sach[q] = cw[q]; }
        ghi_dong(fe, msg, RS_K);
        for (i = 0; i < nerr; i++) {
            seed = seed * 1103515245u + 12345u;
            cw[(seed >> 8) % RS_N] ^= (uint8_t)(1u + ((seed >> 20) % 255u));
        }
        /* ★ Injection positions are random and CAN COLLIDE: two hits on the
           same symbol can cancel.  The number of injections is NOT the error
           count.  Count the real one by comparing against the clean codeword. */
        { unsigned q, thuc = 0;
          for (q = 0; q < RS_N; q++) if (cw[q] != sach[q]) thuc++;
          fprintf(fn, "%u\n", thuc); }
        ghi_dong(fc, cw, RS_N);
        memcpy(coded + (size_t)c * RS_N, cw, RS_N);
    }

    /* Run the SOFTWARE decoder over that exact array -- no bitmap. */
    rc = ps_rs_decode(coded, (size_t)ncw * RS_N, out, sizeof(meta_t) +
                      (size_t)ncw * RS_K, &out_len, NULL);
    if (rc != 0) { fprintf(stderr, "C decoder returned %d\n", rc); return 1; }

    for (c = 0; c < ncw; c++)
        ghi_dong(fo, out + sizeof(meta_t) + (size_t)c * RS_K, RS_K);

    {   /* ps_rs_decode aggregates the whole batch's stats; record them for an overall check */
        meta_t *m = (meta_t *)out;
        fprintf(ff, "%u %u\n", m->corrected_symbols, m->decode_failed);
    }
    fclose(fc); fclose(fe); fclose(fo); fclose(ff); fclose(fn);
    printf("generated %u codewords (error counts 0..21, t = 20)\n", ncw);
    return 0;
}
