/* =========================================================================
 * ps_rs_decode.c -- a PS module for the receive span (B5, B6).
 *
 * Substitutes the FEC decoder: coded bytes leave the data plane at RX B5,
 * this code decodes them on the ARM, and the message bytes re-enter at B6.
 *
 * Boundary contract (PHY spec 3.3):
 *   in   unsigned 8-bit symbols over GF(2^8), hard decisions, codeword
 *        order, a multiple of 255 symbols, no de-interleaving, optionally
 *        followed by an erasure bitmap of one bit per symbol.
 *   out  unsigned 8-bit message bytes, transmission order, 215 per codeword,
 *        preceded by the metadata header below.
 *
 * Field conventions match the RTL exactly: primitive polynomial 0x11D,
 * alpha = 2, first consecutive root FCR = 0, so syndrome S_i = r(alpha^i).
 * Getting any of the three wrong yields a decoder that runs and returns
 * plausible garbage, which is why they are stated here rather than assumed.
 * ========================================================================= */
#include "integrive.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RS_N   255
#define RS_K   215
#define RS_2T  (RS_N - RS_K)      /* 40 parity symbols, t = 20 */

/* Metadata the substituted decoder must report so that the statistics the
   FPGA decoder produces do not silently stop (spec 3.3, RX B6). */
typedef struct {
    uint32_t corrected_symbols;
    uint32_t decode_failed;
} rs_meta_t;

/* ---- GF(256) -------------------------------------------------------- */
static uint8_t gf_exp[512], gf_log[256];

static void gf_init(void)
{
    unsigned x = 1, i;
    for (i = 0; i < 255; i++) {
        gf_exp[i] = (uint8_t)x;
        gf_log[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100)
            x ^= 0x11D;
    }
    for (i = 255; i < 512; i++)
        gf_exp[i] = gf_exp[i - 255];
}

static uint8_t gf_mul(uint8_t a, uint8_t b)
{
    if (!a || !b)
        return 0;
    return gf_exp[gf_log[a] + gf_log[b]];
}

static uint8_t gf_inv(uint8_t a)
{
    return gf_exp[255 - gf_log[a]];
}

static uint8_t gf_pow(uint8_t a, int n)
{
    int e;
    if (!a)
        return 0;
    e = (gf_log[a] * n) % 255;
    if (e < 0)
        e += 255;
    return gf_exp[e];
}

/* ---- decode one codeword in place ------------------------------------
 * Returns the number of corrected symbols, or -1 if uncorrectable.
 * cw[0] is the highest-degree coefficient, matching the RTL.
 * -------------------------------------------------------------------- */
static int rs_decode_cw(uint8_t *cw)
{
    uint8_t synd[RS_2T];
    uint8_t lam[RS_2T + 1], prev[RS_2T + 1], tmp[RS_2T + 1];
    uint8_t omega[RS_2T + 1];
    int     pos[RS_2T];
    int     i, j, L = 0, m = 1, nerr = 0;
    uint8_t b = 1;
    int     any = 0;

    /* syndromes S_i = r(alpha^i), FCR = 0 */
    for (i = 0; i < RS_2T; i++) {
        uint8_t s = 0, a = gf_exp[i % 255];
        for (j = 0; j < RS_N; j++)
            s = (uint8_t)(gf_mul(s, a) ^ cw[j]);
        synd[i] = s;
        if (s)
            any = 1;
    }
    if (!any)
        return 0;                       /* clean codeword */

    /* Berlekamp-Massey */
    memset(lam,  0, sizeof(lam));
    memset(prev, 0, sizeof(prev));
    lam[0] = prev[0] = 1;

    for (i = 0; i < RS_2T; i++) {
        uint8_t d = synd[i];
        for (j = 1; j <= L; j++)
            d ^= gf_mul(lam[j], synd[i - j]);

        if (d == 0) {
            m++;
        } else if (2 * L <= i) {
            uint8_t coef = gf_mul(d, gf_inv(b));
            memcpy(tmp, lam, sizeof(lam));
            for (j = 0; j + m <= RS_2T; j++)
                lam[j + m] ^= gf_mul(coef, prev[j]);
            L = i + 1 - L;
            memcpy(prev, tmp, sizeof(lam));
            b = d;
            m = 1;
        } else {
            uint8_t coef = gf_mul(d, gf_inv(b));
            for (j = 0; j + m <= RS_2T; j++)
                lam[j + m] ^= gf_mul(coef, prev[j]);
            m++;
        }
    }

    if (L > RS_2T / 2)
        return -1;                      /* more errors than the code can carry */

    /* Chien search: a root at alpha^-i means an error at degree i, which is
       byte position N-1-i. */
    for (i = 0; i < RS_N; i++) {
        uint8_t v = 0;
        for (j = 0; j <= L; j++)
            v ^= gf_mul(lam[j], gf_pow(gf_exp[(255 - (i % 255)) % 255], j));
        if (v == 0) {
            if (nerr >= RS_2T)
                return -1;
            pos[nerr++] = RS_N - 1 - i;
        }
    }
    if (nerr != L)
        return -1;                      /* locator degree and roots disagree */

    /* Forney: omega = S(x) * Lambda(x) mod x^2t */
    memset(omega, 0, sizeof(omega));
    for (i = 0; i < RS_2T; i++)
        for (j = 0; j <= L && i + j < RS_2T; j++)
            omega[i + j] ^= gf_mul(synd[i], lam[j]);

    for (i = 0; i < nerr; i++) {
        int     deg = RS_N - 1 - pos[i];
        uint8_t Xi  = gf_exp[deg % 255];          /* error locator          */
        uint8_t Xinv = gf_inv(Xi);
        uint8_t num = 0, den = 0;

        for (j = 0; j < RS_2T; j++)
            num ^= gf_mul(omega[j], gf_pow(Xinv, j));
        /* formal derivative of Lambda: odd-degree terms only */
        for (j = 1; j <= L; j += 2)
            den ^= gf_mul(lam[j], gf_pow(Xinv, j - 1));
        if (den == 0)
            return -1;

        /* FCR = 0 so the magnitude carries one extra factor of Xi. */
        cw[pos[i]] ^= gf_mul(Xi, gf_mul(num, gf_inv(den)));
    }

    /* Verify: a decoder that reports success on a wrong answer is worse than
       one that reports failure, and Berlekamp-Massey can converge on a valid
       locator for an out-of-range error pattern. */
    for (i = 0; i < RS_2T; i++) {
        uint8_t s = 0, a = gf_exp[i % 255];
        for (j = 0; j < RS_N; j++)
            s = (uint8_t)(gf_mul(s, a) ^ cw[j]);
        if (s)
            return -1;
    }
    return nerr;
}

/* ---- decode one codeword with erasures -------------------------------
 * `er` marks known-unreliable symbols.  An RS code corrects 2t erasures
 * against t errors, so telling the decoder which symbols are suspect is worth
 * twice as much as making it find them -- which is the whole reason Section
 * 3.3 asks for the bitmap.
 *
 * Standard construction: build the erasure locator, convert the syndromes to
 * Forney syndromes, run Berlekamp-Massey on those for the remaining errors,
 * multiply the two locators, then Chien + Forney as usual.
 * Returns the number of corrected symbols, or -1.
 * -------------------------------------------------------------------- */
static int rs_decode_cw_erasures(uint8_t *cw, const uint8_t *er)
{
    uint8_t synd[RS_2T], lam_e[RS_2T + 1], forney[RS_2T];
    uint8_t lam_f[RS_2T + 1], prev[RS_2T + 1], tmp[RS_2T + 1];
    uint8_t lam[RS_2T + 1], omega[RS_2T + 1];
    int     epos[RS_2T], pos[RS_2T];
    int     i, j, ne = 0, L = 0, m = 1, nerr = 0, any = 0;
    uint8_t b = 1;

    for (i = 0; i < RS_N && ne < RS_2T; i++)
        if (er[i])
            epos[ne++] = i;
    if (ne == 0)
        return rs_decode_cw(cw);            /* nothing marked: ordinary path */

    for (i = 0; i < RS_2T; i++) {
        uint8_t s = 0, a = gf_exp[i % 255];
        for (j = 0; j < RS_N; j++)
            s = (uint8_t)(gf_mul(s, a) ^ cw[j]);
        synd[i] = s;
        if (s) any = 1;
    }
    if (!any)
        return 0;

    /* erasure locator: product of (1 - X_j x), X_j = alpha^(N-1-pos) */
    memset(lam_e, 0, sizeof(lam_e));
    lam_e[0] = 1;
    for (i = 0; i < ne; i++) {
        uint8_t X = gf_exp[(RS_N - 1 - epos[i]) % 255];
        for (j = ne; j > 0; j--)
            lam_e[j] ^= gf_mul(lam_e[j - 1], X);
    }

    /* Forney syndromes: T = S * lam_e mod x^2t, keeping terms from degree ne */
    for (i = 0; i < RS_2T; i++) {
        uint8_t v = 0;
        for (j = 0; j <= ne && j <= i; j++)
            v ^= gf_mul(synd[i - j], lam_e[j]);
        forney[i] = v;
    }

    /* Berlekamp-Massey on the Forney syndromes, for the errors that remain */
    memset(lam_f, 0, sizeof(lam_f));
    memset(prev,  0, sizeof(prev));
    lam_f[0] = prev[0] = 1;
    for (i = 0; i < RS_2T - ne; i++) {
        uint8_t d = forney[i + ne];
        for (j = 1; j <= L; j++)
            d ^= gf_mul(lam_f[j], forney[i + ne - j]);
        if (d == 0) {
            m++;
        } else if (2 * L <= i) {
            uint8_t coef = gf_mul(d, gf_inv(b));
            memcpy(tmp, lam_f, sizeof(lam_f));
            for (j = 0; j + m <= RS_2T; j++)
                lam_f[j + m] ^= gf_mul(coef, prev[j]);
            L = i + 1 - L;
            memcpy(prev, tmp, sizeof(lam_f));
            b = d; m = 1;
        } else {
            uint8_t coef = gf_mul(d, gf_inv(b));
            for (j = 0; j + m <= RS_2T; j++)
                lam_f[j + m] ^= gf_mul(coef, prev[j]);
            m++;
        }
    }
    if (2 * L + ne > RS_2T)
        return -1;                          /* beyond 2f + e <= 2t */

    /* full locator = erasure locator * error locator */
    memset(lam, 0, sizeof(lam));
    for (i = 0; i <= ne; i++)
        for (j = 0; j <= L && i + j <= RS_2T; j++)
            lam[i + j] ^= gf_mul(lam_e[i], lam_f[j]);

    for (i = 0; i < RS_N; i++) {
        uint8_t v = 0;
        for (j = 0; j <= ne + L; j++)
            v ^= gf_mul(lam[j], gf_pow(gf_exp[(255 - (i % 255)) % 255], j));
        if (v == 0) {
            if (nerr >= RS_2T) return -1;
            pos[nerr++] = RS_N - 1 - i;
        }
    }
    if (nerr != ne + L)
        return -1;

    memset(omega, 0, sizeof(omega));
    for (i = 0; i < RS_2T; i++)
        for (j = 0; j <= ne + L && i + j < RS_2T; j++)
            omega[i + j] ^= gf_mul(synd[i], lam[j]);

    for (i = 0; i < nerr; i++) {
        int     deg  = RS_N - 1 - pos[i];
        uint8_t Xi   = gf_exp[deg % 255];
        uint8_t Xinv = gf_inv(Xi);
        uint8_t num = 0, den = 0;

        for (j = 0; j < RS_2T; j++)
            num ^= gf_mul(omega[j], gf_pow(Xinv, j));
        for (j = 1; j <= ne + L; j += 2)
            den ^= gf_mul(lam[j], gf_pow(Xinv, j - 1));
        if (den == 0) return -1;
        cw[pos[i]] ^= gf_mul(Xi, gf_mul(num, gf_inv(den)));
    }

    for (i = 0; i < RS_2T; i++) {
        uint8_t s = 0, a = gf_exp[i % 255];
        for (j = 0; j < RS_N; j++)
            s = (uint8_t)(gf_mul(s, a) ^ cw[j]);
        if (s) return -1;
    }
    return nerr;
}

/* ---- the callback the SDK invokes once per batch --------------------- */
/* Layout of one B5 buffer holding n codewords, with the erasure bitmap:
      symbols    n * 255 bytes
      padding    up to the next 64-byte boundary
      bitmap     ceil(n * 255 / 8) bytes
   Without the bitmap it is simply n * 255.  Both forms are recognised; the
   bitmap form is tested first because it is the one the PHY actually sends,
   and a length can satisfy both by coincidence. */
size_t rs_codeword_count(size_t in_len, int *has_bitmap)
{
    size_t n;

    for (n = in_len / RS_N; n > 0; n--) {
        size_t sym = n * RS_N;
        size_t tot = ((sym + 63u) & ~(size_t)63u) + (sym + 7u) / 8u;
        if (tot == in_len) { *has_bitmap = 1; return n; }
        if (tot < in_len)  break;          /* monotonic: no larger n can match */
    }
    if (in_len % RS_N == 0) { *has_bitmap = 0; return in_len / RS_N; }
    *has_bitmap = 0;
    return 0;
}

int ps_rs_decode(const void *in, size_t in_len,
                 void *out, size_t out_capacity,
                 size_t *out_len, void *user)
{
    const uint8_t *src = (const uint8_t *)in;
    const uint8_t *bitmap = NULL;
    int            co_bitmap = 0;
    uint8_t       *dst = (uint8_t *)out;
    rs_meta_t     *meta;
    size_t         ncw, c, produced;
    static int     inited;

    (void)user;

    if (!inited) {
        gf_init();
        inited = 1;
    }

    /* The buffer holds a whole number of codewords, optionally followed by the
       erasure bitmap (one bit per symbol, LSB-first, padded to 64 bytes).
       Section 3.3 explains why it is worth having: an RS code corrects 2t
       erasures against t errors, so a symbol the demodulator already knows is
       unreliable costs half as much to repair as one it has to find.

       ★ in_len INCLUDES the bitmap, so `in_len / RS_N` overestimates the
       codeword count.  For 30 codewords the symbol array is 7650 bytes, the
       bitmap starts at 7680 and runs 957 bytes, giving in_len = 8637 --
       and 8637/255 = 33.  Decoding 33 codewords reads three of them out of
       the bitmap and returns 33*215 bytes of plausible garbage, which is
       exactly the failure this file's header warns about.  Recover the count
       from the layout instead of dividing. */
    ncw = rs_codeword_count(in_len, &co_bitmap);
    if (ncw == 0)
        return -1;

    produced = sizeof(rs_meta_t) + ncw * RS_K;
    if (produced > out_capacity)
        return -1;

    meta = (rs_meta_t *)dst;
    meta->corrected_symbols = 0;
    meta->decode_failed     = 0;

    if (co_bitmap)
        bitmap = src + ((ncw * RS_N + 63u) & ~(size_t)63u);

    for (c = 0; c < ncw; c++) {
        uint8_t cw[RS_N], er[RS_N];
        int     n;

        memcpy(cw, src + c * RS_N, RS_N);
        if (bitmap) {
            size_t k;
            for (k = 0; k < RS_N; k++) {
                size_t bit = c * RS_N + k;
                er[k] = (uint8_t)((bitmap[bit >> 3] >> (bit & 7u)) & 1u);
            }
            n = rs_decode_cw_erasures(cw, er);
        } else {
            n = rs_decode_cw(cw);
        }
        if (n < 0)
            meta->decode_failed++;
        else
            meta->corrected_symbols += (uint32_t)n;

        /* Systematic code: the message occupies the leading K symbols. */
        memcpy(dst + sizeof(rs_meta_t) + c * RS_K, cw, RS_K);
    }

    *out_len = produced;
    return 0;
}

/* ---- standalone self-test -------------------------------------------
 * Runs without a board:  make examples/ps_rs_decode && ./examples/ps_rs_decode
 * -------------------------------------------------------------------- */
#ifndef PS_RS_NO_MAIN
size_t rs_codeword_count(size_t in_len, int *has_bitmap);
static void rs_encode_cw(const uint8_t *msg, uint8_t *cw)
{
    uint8_t gen[RS_2T + 1];
    int i, j;

    memset(gen, 0, sizeof(gen));
    gen[0] = 1;
    for (i = 0; i < RS_2T; i++) {
        uint8_t r = gf_exp[i % 255];
        for (j = i + 1; j > 0; j--)
            gen[j] = (uint8_t)(gen[j - 1] ^ gf_mul(gen[j], r));
        gen[0] = gf_mul(gen[0], r);
    }

    memcpy(cw, msg, RS_K);
    memset(cw + RS_K, 0, RS_2T);
    for (i = 0; i < RS_K; i++) {
        uint8_t coef = cw[i];
        if (!coef)
            continue;
        for (j = 1; j <= RS_2T; j++)
            cw[i + j] ^= gf_mul(gen[RS_2T - j], coef);
    }
    memcpy(cw, msg, RS_K);
}

int main(void)
{
    uint8_t msg[RS_K], cw[RS_N];
    unsigned seed = 12345u, trial;
    int fails = 0;

    gf_init();

    for (trial = 0; trial <= 21u; trial++) {
        unsigned i;
        int      n;

        for (i = 0; i < RS_K; i++) {
            seed = seed * 1103515245u + 12345u;
            msg[i] = (uint8_t)(seed >> 16);
        }
        rs_encode_cw(msg, cw);

        for (i = 0; i < trial; i++) {
            seed = seed * 1103515245u + 12345u;
            cw[(seed >> 8) % RS_N] ^= (uint8_t)(1u + ((seed >> 20) % 255u));
        }

        n = rs_decode_cw(cw);
        if (trial <= 20u) {
            if (n < 0 || memcmp(cw, msg, RS_K) != 0) {
                printf("  %2u errors: FAILED (returned %d)\n", trial, n);
                fails++;
            } else {
                printf("  %2u errors: recovered\n", trial);
            }
        } else {
            printf("  %2u errors: %s (beyond t=20, either outcome is legal)\n",
                   trial, (n < 0) ? "reported uncorrectable" : "reported success");
        }
    }

    /* Erasures: the code corrects 2t = 40 marked symbols, twice what it can
       find unaided.  Sweep the boundary 2f + e <= 40. */
    printf("\nwith the erasure bitmap (2f + e <= 40):\n");
    for (trial = 0; trial <= 3u; trial++) {
        unsigned f = trial * 5u;              /* errors  */
        unsigned e = 40u - 2u * f;            /* erasures the budget allows */
        uint8_t  er[RS_N];
        unsigned i;
        int      n;

        for (i = 0; i < RS_K; i++) {
            seed = seed * 1103515245u + 12345u;
            msg[i] = (uint8_t)(seed >> 16);
        }
        rs_encode_cw(msg, cw);
        memset(er, 0, sizeof(er));

        for (i = 0; i < e; i++) {            /* marked, contiguous */
            cw[i] ^= (uint8_t)(1u + (i % 255u));
            er[i]  = 1;
        }
        for (i = 0; i < f; i++) {            /* unmarked, elsewhere */
            size_t k = RS_N - 1u - i;
            cw[k] ^= (uint8_t)(1u + (i % 255u));
        }

        n = rs_decode_cw_erasures(cw, er);
        if (n < 0 || memcmp(cw, msg, RS_K) != 0) {
            printf("  %2u errors + %2u erasures: FAILED (returned %d)\n", f, e, n);
            fails++;
        } else {
            printf("  %2u errors + %2u erasures: recovered\n", f, e);
        }
    }

    /* ---- the whole B5 buffer, laid out as the PHY actually sends it ------
       symbols | pad to 64 | erasure bitmap.  This is the path that broke:
       in_len includes the bitmap, so dividing it by 255 counted 33 codewords
       where there were 30 and decoded three of them out of the bitmap. */
    printf("\nwhole B5 buffer (symbols + pad + bitmap):\n");
    {
        enum { NCW = 30 };
        static uint8_t buf[NCW * RS_N + 64 + (NCW * RS_N + 7) / 8];
        static uint8_t out[sizeof(rs_meta_t) + NCW * RS_K];
        static uint8_t want[NCW * RS_K];
        size_t sym = (size_t)NCW * RS_N;
        size_t map_off = (sym + 63u) & ~(size_t)63u;
        size_t in_len  = map_off + (sym + 7u) / 8u;
        size_t out_len = 0, c;
        int    n_bm = -1, rc;
        size_t got_cw;
        rs_meta_t *m;

        memset(buf, 0, sizeof buf);
        for (c = 0; c < NCW; c++) {
            unsigned i;
            for (i = 0; i < RS_K; i++) {
                seed = seed * 1103515245u + 12345u;
                msg[i] = (uint8_t)(seed >> 16);
            }
            memcpy(want + c * RS_K, msg, RS_K);
            rs_encode_cw(msg, cw);
            /* 6 marked + 5 unmarked: 2*5 + 6 = 16 <= 40, inside the budget. */
            for (i = 0; i < 6; i++) {
                size_t k = c * RS_N + i;
                cw[i] ^= (uint8_t)(0x5Au + i);
                buf[map_off + (k >> 3)] |= (uint8_t)(1u << (k & 7u));
            }
            for (i = 0; i < 5; i++)
                cw[RS_N - 1 - i] ^= (uint8_t)(0xA5u + i);
            memcpy(buf + c * RS_N, cw, RS_N);
        }

        got_cw = rs_codeword_count(in_len, &n_bm);
        printf("  in_len = %zu  ->  %zu codewords, bitmap %s\n",
               in_len, got_cw, n_bm ? "present" : "absent");
        if (got_cw != NCW || n_bm != 1) {
            printf("  FAILED: expected %d codewords with a bitmap\n", NCW);
            fails++;
        }

        rc = ps_rs_decode(buf, in_len, out, sizeof out, &out_len, NULL);
        m  = (rs_meta_t *)out;
        if (rc != 0 || out_len != sizeof(rs_meta_t) + (size_t)NCW * RS_K) {
            printf("  FAILED: rc=%d out_len=%zu\n", rc, out_len);
            fails++;
        } else if (m->decode_failed != 0) {
            printf("  FAILED: %u codewords reported uncorrectable\n",
                   m->decode_failed);
            fails++;
        } else if (memcmp(out + sizeof(rs_meta_t), want, sizeof want) != 0) {
            printf("  FAILED: message bytes do not match\n");
            fails++;
        } else {
            printf("  %d codewords recovered, %u symbols corrected, 0 failures\n",
                   NCW, m->corrected_symbols);
        }
    }

    printf("%s\n", fails ? "SELF-TEST FAILED" : "SELF-TEST PASSED");
    return fails ? 1 : 0;
}
#endif
