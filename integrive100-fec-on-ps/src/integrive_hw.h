/* =========================================================================
 * integrive_hw.h -- private hardware layer.  NOT part of the public API.
 *
 * Rule N8 of the API spec forbids register names and offsets in the public
 * interface, so every one of them lives here and nowhere else.
 * ========================================================================= */
#ifndef INTEGRIVE_HW_H
#define INTEGRIVE_HW_H

#include <stdint.h>
#include <stddef.h>

/* ---- PL register windows (AXI-lite, mapped through /dev/mem) ------------ */
#define ITG_TX_INTF_BASE   0x83c00000u   /* tx_intf                          */
#define ITG_RX_INTF_BASE   0x83c20000u   /* rx_intf                          */
#define ITG_XPU_BASE       0x83c30000u   /* xpu / phy control + diagnostics  */
#define ITG_WINDOW_LEN     0x10000u

/* tx_intf ---------------------------------------------------------------- */
#define ITG_TX_REG7        0x1c   /* [3] = fire one golden frame            */
#define ITG_TX_REG10       0x28   /* [2]=no zero-stuff [3]=invert dac_phase */
#define ITG_TX_BB_GAIN     0x34   /* transmit digital backoff               */
#define ITG_TX_FIRE_BIT    (1u << 3)

/* rx_intf ---------------------------------------------------------------- */
#define ITG_RX_REG3        0x0c   /* counters + captured-byte read port      */
#define ITG_RX_REG4        0x10   /* [3] = bb_20M_en                         */
#define ITG_RX_REG11       0x2c   /* bb_gain + decim_bypass                  */
#define ITG_RX3_CLR        0x00000800u   /* clear the per-frame counters     */
#define ITG_RX3_REPLAY     0x00000200u   /* rx_replay trigger: push one stored
                                            frame through the RX chain so the
                                            capture RAM fills with THIS frame.
                                            Firing the DAC (TX) instead does not
                                            fill the RAM in step, and the B5
                                            capture then reads stale bytes.   */
#define ITG_RX3_TOTAL      0x00020000u   /* select: frames detected          */
#define ITG_RX3_PASS       0x00010000u   /* select: frames RS-clean          */
#define ITG_RX3_BYTE       0x00030000u   /* select: captured byte, idx << 20 */
#define ITG_RX3_BYTE_SHIFT 20
/* The selected value is read back from slv_reg31, NOT by re-reading reg3.
   Reading reg3 returns what was written to it and looks plausible, which is
   the trap: the counters come back as the selector constant. */
#define ITG_RX_READBACK    0x7c

/* Firing a frame writes ABSOLUTE values, mirroring the proven board recipe:
   0 -> bit3 -> 0.  A read-modify-write would carry whatever else is set in
   the register, and this register also holds the transmit control bits. */
#define ITG_TX_FIRE_SEQ0   0x00000000u
#define ITG_TX_FIRE_SEQ1   0x00000008u

/* xpu -------------------------------------------------------------------- */
#define ITG_XPU_MULTI_RST  0x00   /* pulse 1 then 0 before each frame        */
#define ITG_XPU_REG5       0x14   /* [3:0] window shift, [8] fft_sat_en      */
#define ITG_XPU_PHY_DIAG   0x78   /* [21] chest fired, [31:22] group fails   */
/* slv_reg5[16]: route the coded-byte stream (RX B5) onto the byte capture in
   place of the decoded message bytes.  0 = normal, and it must be left at 0
   for any measurement of the decoded output. */
#define ITG_REG5_B5_MODE   (1u << 16)
/* slv_reg5[17]: emit the RX B5 erasure bitmap instead of the coded bytes --
   one bit per coded symbol, packed LSB-first, a set bit marking the symbol as
   unreliable (PHY spec 3.3).  Read as a second capture; byte_out carries one
   stream at a time. */
#define ITG_REG5_ER_MODE   (1u << 17)
/* [18] = emit a copy of the B4 stream (equalized samples): 2 bytes per
   subcarrier, I then Q, each an 8-bit signed component.  This is the CAPTURE
   half of span (B4,B5). */
#define ITG_REG5_B4_MODE   (1u << 18)
/* [19] = the B3 stream (bin-extracted subcarriers, 4 bytes per subcarrier:
          I high, I low, Q high, Q low -- each a 16-bit signed component).
   [20] = the B2 stream (CFO-corrected samples, cyclic prefix still present;
          also 4 bytes each). */
#define ITG_REG5_B3_MODE   (1u << 19)
#define ITG_REG5_B2_MODE   (1u << 20)

/* Chapter 6A span (B5, B6).  reg6 controls the stall and the return, reg7 is
   the byte port into the return buffer.
     reg6[0]      span enable: the chain holds at the exit boundary
     reg6[1]      reset the return-buffer write pointer
     reg6[2]      go: emit ps_len bytes from the return buffer downstream
     reg6[31:16]  ps_len
     reg7[7:0]    data, reg7[8] toggle -- flip it to write one byte
   The toggle exists because writing the same value twice must still count as
   two bytes; comparing against the previous value would swallow the second. */
#define ITG_XPU_REG6       0x18
#define ITG_XPU_REG7       0x1c
#define ITG_REG6_SPAN_EN   (1u << 0)
#define ITG_REG6_PTR_RST   (1u << 1)
#define ITG_REG6_GO        (1u << 2)
#define ITG_REG6_LEN_SHIFT 16
/* [6:4] = the ENTRY boundary index (the insertion point).  Only one span is
   active at a time (6A.4), so a single register set suffices; this index picks
   where injection happens.
   ★ 0 is treated as 6, so legacy software -- which wrote reg6 before this field
     existed -- still behaves exactly as before. */
#define ITG_REG6_BND_SHIFT 4
#define ITG_REG6_BND_MASK  (7u << ITG_REG6_BND_SHIFT)

/* The captured bytes are read four to a 32-bit word through the same port:
   write ITG_RX3_BYTE | (word_index << ITG_RX3_BYTE_SHIFT) to reg3, then read
   the readback register. */
#define ITG_CAP_WORDS      2048u
#define ITG_CAP_BYTES      (ITG_CAP_WORDS * 4u)

/* ---- AD9361 through the industrial-I/O sysfs interface ----------------- */
#define ITG_IIO_DEV        "/sys/bus/iio/devices/iio:device0"
#define ITG_IIO_DEBUG      "/sys/kernel/debug/iio/iio:device0"

/* ---- Safety ceiling ----------------------------------------------------
 * The board carries an RF amplifier and is normally cabled straight into
 * its own receiver, where an over-driven transmitter damages the front end.
 * The ceiling is enforced in the library, not left to the caller.
 * ----------------------------------------------------------------------- */
#define ITG_TX_GAIN_DB_MAX   (-27.0)
#define ITG_TX_GAIN_DB_MIN   (-89.75)
#define ITG_RX_GAIN_DB_MIN   (0.0)
#define ITG_RX_GAIN_DB_MAX   (73.0)

/* ---- Which modulations the loaded bitstream demodulates ----------------
 * The PHY specification allows modulation to be a synthesis-time choice: an
 * application asks integrive_is_modulation_supported() and gets UNSUPPORTED
 * for a scheme the bitstream was not built with.  This constant is how the
 * library knows which case it is, and it MUST match the bitstream:
 *
 *   0            built with RX_ADAPTIVE_MOD -- all five, chosen per packet
 *                from the SIGNAL field
 *   1,2,4,6,8    built for one modulation only (bits per subcarrier)
 *
 * Reading it from a capability register would be better than trusting a
 * compile-time constant, but no such register exists yet; it is listed as an
 * outstanding item.  Override at build time:  make CFLAGS='... -DITG_BITSTREAM_NBPSC=2'
 */
#ifndef ITG_BITSTREAM_NBPSC
#define ITG_BITSTREAM_NBPSC 8       /* the shipped bitstream is 256-QAM */
#endif

/* ---- Frame geometry of the loaded bitstream ---------------------------- */
#define ITG_FFT_SIZE         1024u
#define ITG_CP_LENGTH        26u
#define ITG_DATA_SC          765u
#define ITG_PILOT_SC         11u
#define ITG_RS_N             255u
#define ITG_RS_K             215u
#define ITG_CODED_PER_GROUP  765u    /* 3 codewords */
#define ITG_MSG_PER_GROUP    645u    /* 3 * 215     */
#define ITG_FRAME_SYMBOLS    10u     /* payload OFDM symbols per frame */

typedef struct {
    int      mem_fd;
    volatile uint32_t *tx;
    volatile uint32_t *rx;
    volatile uint32_t *xpu;
} itg_hw_t;

int      itg_hw_open (itg_hw_t *hw);
void     itg_hw_close(itg_hw_t *hw);

static inline void     itg_wr(volatile uint32_t *base, unsigned off, uint32_t v)
{ base[off >> 2] = v; }
static inline uint32_t itg_rd(volatile uint32_t *base, unsigned off)
{ return base[off >> 2]; }

/* sysfs helpers: return 0 on success, -1 on failure (errno set). */
int itg_sysfs_write_str(const char *dir, const char *attr, const char *val);
int itg_sysfs_write_ll (const char *dir, const char *attr, long long val);
int itg_sysfs_read_str (const char *dir, const char *attr, char *buf, size_t len);
int itg_sysfs_read_dbl (const char *dir, const char *attr, double *out);

#endif /* INTEGRIVE_HW_H */
