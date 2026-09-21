/* =========================================================================
 * integrive.c -- Integrive-100 SDK, reference implementation.
 *
 * Conforms to ITG-100-API-001 v1.0.  See sdk/README.md for the map from this
 * interface onto the openwifi/neptunesdr bitstream, and for the list of calls
 * that currently return INTEGRIVE_ERR_UNSUPPORTED because the bitstream has
 * no such control.  Reporting UNSUPPORTED is deliberate: the spec separates
 * "absent from this bitstream" from "value out of range" precisely so that an
 * application can tell the two apart, and silently accepting a setting the
 * hardware ignores would be the worse failure.
 * ========================================================================= */
#define _GNU_SOURCE
#include "integrive.h"
#include "integrive_hw.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define ITG_SDK_MAJOR 1
#define ITG_SDK_MINOR 0
#define ITG_SDK_PATCH 0

/* ---- 3.1 the handle ---------------------------------------------------- */
struct integrive_device {
    itg_hw_t          hw;
    integrive_state_t state;

    /* configuration mirror.  The PL has no read-back for most of these, so
       the getter returns what was last accepted, per rule N4. */
    double                 freq_hz;
    double                 sample_rate_sps;
    double                 bandwidth_hz;
    double                 tx_gain_db;
    double                 rx_gain_db;
    integrive_modulation_t modulation;
    integrive_modulation_t demodulation;
    integrive_fec_t        fec;
    integrive_ofdm_config_t   ofdm;
    integrive_packet_config_t pkt;

    /* PS substitution */
    integrive_chain_t ps_chain;
    uint32_t          ps_out, ps_in;
    integrive_ps_fn_t ps_fn;
    void             *ps_user;
    size_t            ps_bytes;
    uint32_t          ps_timeout_ms;

    /* reference payload supplied through INTEGRIVE_PATTERN_USER, so that the
       statistics are measured against the truth rather than inferred from the
       decoder's own opinion of itself. */
    uint8_t  ref_payload[8192];
    size_t   ref_len;

    /* Which modulations the loaded bitstream demodulates: 0 = adaptive (all
       five, per packet), otherwise the single n_bpsc it was built for. */
    unsigned bs_nbpsc;

    /* Set when the reference payload ends in a valid CRC-32, i.e. the frame
       format this bitstream transmits carries one. */
    int      ref_crc;

    /* PS-span batch buffers.  Section 3.1 requires 64-byte alignment, and 6A.5
       requires the callback to address them directly rather than through a
       copy, so they are allocated once at start() and handed over as-is. */
    uint8_t *ps_buf_in, *ps_buf_out;
    uint64_t ps_batches, ps_aborts;

    /* statistics accumulated by integrive_wait_packets */
    uint64_t tx_packets, rx_packets, crc_errors, fec_errors, sync_failures;
    uint64_t frames_scored, frames_exact;
    uint64_t bits_compared, bit_errors;
    uint32_t sticky_flags;
};

/* ---- 4. error strings -------------------------------------------------- */
const char *integrive_strerror(integrive_status_t s)
{
    switch (s) {
    case INTEGRIVE_OK:                return "ok";
    case INTEGRIVE_ERR_INVALID_ARG:   return "invalid argument";
    case INTEGRIVE_ERR_OUT_OF_RANGE:  return "value out of range for this device";
    case INTEGRIVE_ERR_INVALID_STATE: return "call not permitted in this state";
    case INTEGRIVE_ERR_UNSUPPORTED:   return "not provided by this bitstream";
    case INTEGRIVE_ERR_NO_DEVICE:     return "no device";
    case INTEGRIVE_ERR_BUSY:          return "device busy";
    case INTEGRIVE_ERR_TIMEOUT:       return "timed out";
    case INTEGRIVE_ERR_IO:            return "I/O error";
    case INTEGRIVE_ERR_FIRMWARE:      return "firmware error";
    case INTEGRIVE_ERR_ABORTED:       return "aborted by the application";
    }
    return "unknown status";
}

/* ---- helpers ----------------------------------------------------------- */
#define CHECK_DEV(d)   do { if (!(d)) return INTEGRIVE_ERR_INVALID_ARG; } while (0)
#define CHECK_OUT(p)   do { if (!(p)) return INTEGRIVE_ERR_INVALID_ARG; } while (0)

/* Structural parameters are latched at start (Guide §7), so they may only be
   written while stopped. */
static integrive_status_t require_stopped(const integrive_device_t *dev)
{
    return (dev->state == INTEGRIVE_STATE_RUNNING)
         ? INTEGRIVE_ERR_INVALID_STATE : INTEGRIVE_OK;
}

/* Guide 7: CONFIGURED is "entered by a complete and consistent parameter set has
   been written".  Every accepted configuration call moves the device there, so
   integrive_get_state reports the documented state machine instead of sitting in
   OPEN until the first stop(). */
static void mark_configured(integrive_device_t *dev)
{
    if (dev->state == INTEGRIVE_STATE_OPEN)
        dev->state = INTEGRIVE_STATE_CONFIGURED;
}

static unsigned bits_per_symbol(integrive_modulation_t m)
{
    switch (m) {
    case INTEGRIVE_MOD_BPSK:   return 1;
    case INTEGRIVE_MOD_QPSK:   return 2;
    case INTEGRIVE_MOD_16QAM:  return 4;
    case INTEGRIVE_MOD_64QAM:  return 6;
    case INTEGRIVE_MOD_256QAM: return 8;
    }
    return 0;
}

/* The receiver reads its modulation from the SIGNAL field of each packet, so
   the index below is what the transmitter must encode, not a register value. */
static unsigned signal_field_index(integrive_modulation_t m)
{
    switch (m) {
    case INTEGRIVE_MOD_BPSK:   return 1;
    case INTEGRIVE_MOD_QPSK:   return 2;
    case INTEGRIVE_MOD_16QAM:  return 3;
    case INTEGRIVE_MOD_64QAM:  return 4;
    case INTEGRIVE_MOD_256QAM: return 0;
    }
    return 0;
}

/* Runs one batch through the application's PS module; defined with the
   Chapter 6A block below. */
static integrive_status_t ps_run_batch(integrive_device_t *dev,
                                       size_t in_len, size_t *out_len);
static void ps_return_bytes(integrive_device_t *dev,
                            const uint8_t *buf, size_t n);
static void ps_span_release(integrive_device_t *dev);

/* Read the frame-capture buffer.  Declared here because the measurement loop
   below uses it and the definition sits with the boundary code. */
static integrive_status_t read_capture_bytes(integrive_device_t *dev,
                                             int b5_mode, uint8_t *buf,
                                             size_t capacity, size_t *len);
static integrive_status_t capture_frame_bytes(integrive_device_t *dev,
                                              int mode, uint8_t *buf,
                                              size_t capacity, size_t *len);

/* True while the FEC decoder is the substituted block: the RX span (B5,B6). */
static int fec_runs_on_ps(const integrive_device_t *dev)
{
    return dev->ps_fn != NULL &&
           dev->ps_chain == INTEGRIVE_CHAIN_RX &&
           dev->ps_out == 5u && dev->ps_in == 6u;
}

/* Coded-symbol bytes one frame writes at B5 = codewords * RS_N.
   read_capture_bytes() reads a FIXED window (ITG_CAP_BYTES == 8192), which is
   larger than a frame's B5 content (7650 for the shipped 256-QAM framing); the
   tail of the RAM is stale from an earlier frame.  The fabric exposes no valid-
   length register, so the true length is reconstructed from the framing and
   used to trim the batch before it reaches the callback -- otherwise the RS
   decoder is handed 8192 bytes, which is not a whole number of codewords, and
   rejects it.  Returns 0 if the modulation is unset (caller then does not
   trim). */
static size_t b5_coded_bytes(const integrive_device_t *dev)
{
    unsigned bits   = bits_per_symbol(dev->demodulation);
    unsigned groups = bits ? (ITG_FRAME_SYMBOLS * bits) / 8u : 0u;
    unsigned ncw    = groups * (ITG_MSG_PER_GROUP / ITG_RS_K);
    return (size_t)ncw * ITG_RS_N;
}

/* CRC-32, polynomial 0x04C11DB7, reflected in and out with a final inversion
   -- the Ethernet convention the specification names in Section 2.3.  The
   fabric has no CRC block (its `fcs_ok` is the Reed-Solomon verdict), so the
   check happens here, over the payload the decoder produced. */
static uint32_t itg_crc32(const uint8_t *d, size_t n)
{
    static uint32_t tab[256];
    static int      inited;
    uint32_t        c = 0xFFFFFFFFu;
    size_t          i;

    if (!inited) {
        uint32_t k, j, v;
        for (k = 0; k < 256u; k++) {
            v = k;
            for (j = 0; j < 8u; j++)
                v = (v & 1u) ? ((v >> 1) ^ 0xEDB88320u) : (v >> 1);
            tab[k] = v;
        }
        inited = 1;
    }
    for (i = 0; i < n; i++)
        c = tab[(c ^ d[i]) & 0xffu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* A frame carries a CRC if the last four payload bytes are the CRC-32 of
   everything before them.  Detecting it rather than configuring it keeps the
   library honest about a bitstream whose transmit ROM was built without one:
   there is no flag to set wrongly. */
static int frame_has_crc(const uint8_t *d, size_t n)
{
    uint32_t want, got;

    if (n < 5u)
        return 0;
    want = (uint32_t)d[n - 4] | ((uint32_t)d[n - 3] << 8)
         | ((uint32_t)d[n - 2] << 16) | ((uint32_t)d[n - 1] << 24);
    got  = itg_crc32(d, n - 4u);
    return want == got;
}

static void sleep_ms(unsigned ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000u;
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&ts, NULL);
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000L);
}

/* ---- 5. device lifecycle ----------------------------------------------- */
integrive_status_t integrive_get_device_count(unsigned *count)
{
    CHECK_OUT(count);
    *count = (access(ITG_IIO_DEV, F_OK) == 0) ? 1u : 0u;
    return INTEGRIVE_OK;
}

integrive_status_t integrive_open_by_index(unsigned index,
                                           integrive_device_t **out)
{
    integrive_device_t *dev;

    CHECK_OUT(out);
    if (index != 0)
        return INTEGRIVE_ERR_NO_DEVICE;

    dev = calloc(1, sizeof(*dev));
    if (!dev)
        return INTEGRIVE_ERR_IO;

    if (itg_hw_open(&dev->hw) != 0) {
        free(dev);
        return INTEGRIVE_ERR_NO_DEVICE;
    }

    /* Defaults are the reference configuration of the PHY specification,
       except the transmit gain, which starts at the minimum of the range so
       that a board cabled into its own receiver cannot be damaged by a
       program that forgets to set it (spec §2.4). */
    dev->state           = INTEGRIVE_STATE_OPEN;
    dev->freq_hz         = 2484e6;
    dev->sample_rate_sps = 15.36e6;
    dev->bandwidth_hz    = 12.4e6;
    dev->tx_gain_db      = ITG_TX_GAIN_DB_MIN;
    dev->rx_gain_db      = 31.0;
    /* 256-QAM, not the spec's QPSK reference: this is the only modulation the
       shipped bitstream carries (ITG_BITSTREAM_NBPSC == 8), and it is the one
       the default payload below is sized for.  Defaulting to QPSK left open()
       self-inconsistent -- (ITG_FRAME_SYMBOLS * 2) is not RS-group aligned --
       so config_is_consistent() rejected start() for any program that did not
       set the modulation itself, defeating the "still starts" intent. */
    dev->modulation      = INTEGRIVE_MOD_256QAM;
    dev->demodulation    = INTEGRIVE_MOD_256QAM;
    dev->fec             = INTEGRIVE_FEC_RS_255_215;

    dev->ofdm.fft_size          = ITG_FFT_SIZE;
    dev->ofdm.cp_length         = ITG_CP_LENGTH;
    dev->ofdm.data_subcarriers  = ITG_DATA_SC;
    dev->ofdm.pilot_subcarriers = ITG_PILOT_SC;
    dev->ofdm.pilot_index       = NULL;

    /* Default to the payload this bitstream's framing actually produces, so a
       program that never calls set_packet_config still starts.  (The
       specification's reference default of 215 bytes belongs to a one-codeword
       frame, which this bitstream does not build.) */
    dev->pkt.payload_bytes = (ITG_FRAME_SYMBOLS * 8u / 8u) * ITG_MSG_PER_GROUP;
    dev->pkt.pattern       = INTEGRIVE_PATTERN_PRBS23;
    dev->pkt.packet_count  = 0;

    dev->ps_chain      = INTEGRIVE_CHAIN_RX;
    dev->ps_out        = INTEGRIVE_PS_NONE;
    dev->ps_in         = INTEGRIVE_PS_NONE;
    dev->ps_bytes      = 65536;
    dev->ps_timeout_ms = 100;

    {   /* The bitstream's modulation support, overridable without a rebuild. */
        const char *e = getenv("INTEGRIVE_BITSTREAM_NBPSC");
        long        v = e ? strtol(e, NULL, 0) : -1;
        dev->bs_nbpsc = (v == 0 || v == 1 || v == 2 || v == 4 || v == 6 || v == 8)
                      ? (unsigned)v : (unsigned)ITG_BITSTREAM_NBPSC;
    }

    *out = dev;
    return INTEGRIVE_OK;
}

integrive_status_t integrive_open(integrive_device_t **dev)
{
    return integrive_open_by_index(0, dev);
}

integrive_status_t integrive_close(integrive_device_t *dev)
{
    CHECK_DEV(dev);
    if (dev->state == INTEGRIVE_STATE_RUNNING)
        integrive_stop(dev);
    ps_span_release(dev);          /* also covers a handle closed mid-abort */
    itg_hw_close(&dev->hw);
    free(dev->ps_buf_in);
    free(dev->ps_buf_out);
    free(dev);
    return INTEGRIVE_OK;
}

integrive_status_t integrive_reset(integrive_device_t *dev)
{
    CHECK_DEV(dev);
    /* Pulse the PHY reset, then clear the per-frame counters.  This does not
       touch the AD9361: a chip reset from software has repeatedly been shown
       to leave the transmit path dead until the board is power-cycled. */
    itg_wr(dev->hw.xpu, ITG_XPU_MULTI_RST, 1);
    itg_wr(dev->hw.xpu, ITG_XPU_MULTI_RST, 0);
    itg_wr(dev->hw.rx,  ITG_RX_REG3, ITG_RX3_CLR);
    itg_wr(dev->hw.rx,  ITG_RX_REG3, 0);
    dev->tx_packets = dev->rx_packets = 0;
    dev->crc_errors = dev->fec_errors = dev->sync_failures = 0;
    dev->frames_scored = 0;
    dev->state = INTEGRIVE_STATE_OPEN;
    return INTEGRIVE_OK;
}

integrive_status_t integrive_get_state(integrive_device_t *dev,
                                       integrive_state_t  *state)
{
    CHECK_DEV(dev); CHECK_OUT(state);
    *state = dev->state;
    return INTEGRIVE_OK;
}

/* ---- 6. capability discovery -------------------------------------------
 * Ranges describe the loaded bitstream and the AD9361 in front of it, not the
 * specification.  An application that sweeps a parameter reads its bounds
 * from here instead of hard-coding them, so a new bitstream needs no rebuild.
 * ----------------------------------------------------------------------- */
static void range_cont(integrive_range_t *r, double lo, double hi,
                       double step, double def)
{
    memset(r, 0, sizeof(*r));
    r->min = lo; r->max = hi; r->step = step; r->def = def;
}

static void range_list(integrive_range_t *r, const double *v, unsigned n,
                       double def)
{
    unsigned i;
    memset(r, 0, sizeof(*r));
    r->is_list  = 1;
    r->list_len = (n > 32u) ? 32u : n;
    for (i = 0; i < r->list_len; i++)
        r->list[i] = v[i];
    r->min = r->list[0];
    r->max = r->list[r->list_len - 1];
    r->def = def;
}

integrive_status_t integrive_get_capability(integrive_device_t     *dev,
                                            integrive_capability_t  cap,
                                            integrive_range_t      *r)
{
    static const double fft_sizes[] = { 1024.0 };
    static const double rates[]     = { 15.36e6 };

    CHECK_DEV(dev); CHECK_OUT(r);

    switch (cap) {
    case INTEGRIVE_CAP_FREQUENCY_HZ:
        range_cont(r, 70e6, 6000e6, 1.0, 2484e6);
        return INTEGRIVE_OK;

    case INTEGRIVE_CAP_SAMPLE_RATE_SPS:
        /* One value only: the receive chain's window timing, the transmit
           interpolation and the loaded FIR are all cut for 15.36 MSPS.  It is
           a list of one rather than a range so that a sweep cannot silently
           walk off the supported point. */
        range_list(r, rates, 1, 15.36e6);
        return INTEGRIVE_OK;

    case INTEGRIVE_CAP_BANDWIDTH_HZ:
        range_cont(r, 1e6, 20e6, 0.0, 12.4e6);
        return INTEGRIVE_OK;

    case INTEGRIVE_CAP_TX_GAIN_DB:
        /* Ceiling is the library's safety limit, not the chip's. */
        range_cont(r, ITG_TX_GAIN_DB_MIN, ITG_TX_GAIN_DB_MAX, 0.25,
                   ITG_TX_GAIN_DB_MIN);
        return INTEGRIVE_OK;

    case INTEGRIVE_CAP_RX_GAIN_DB:
        range_cont(r, ITG_RX_GAIN_DB_MIN, ITG_RX_GAIN_DB_MAX, 1.0, 31.0);
        return INTEGRIVE_OK;

    case INTEGRIVE_CAP_FFT_SIZE:
        range_list(r, fft_sizes, 1, 1024.0);
        return INTEGRIVE_OK;

    case INTEGRIVE_CAP_CP_LENGTH:
        range_cont(r, 26.0, 26.0, 0.0, 26.0);
        return INTEGRIVE_OK;

    case INTEGRIVE_CAP_PAYLOAD_BYTES:
        /* The frame carries a whole number of RS groups; one group is 645
           message bytes.  The payload is therefore quantised, not free. */
        range_cont(r, (double)ITG_MSG_PER_GROUP,
                      (double)(ITG_MSG_PER_GROUP * 10u),
                      (double)ITG_MSG_PER_GROUP,
                      (double)ITG_MSG_PER_GROUP);
        return INTEGRIVE_OK;

    case INTEGRIVE_CAP_TX_CHANNELS:
    case INTEGRIVE_CAP_RX_CHANNELS:
        /* MODE_1R1T is fixed in this bitstream. */
        range_cont(r, 1.0, 1.0, 0.0, 1.0);
        return INTEGRIVE_OK;

    case INTEGRIVE_CAP_PS_BUFFER_BYTES:
        range_cont(r, 4096.0, 1048576.0, 4096.0, 65536.0);
        return INTEGRIVE_OK;
    }
    return INTEGRIVE_ERR_INVALID_ARG;
}

integrive_status_t integrive_is_modulation_supported(integrive_device_t *dev,
                                                     integrive_modulation_t m,
                                                     int *supported)
{
    CHECK_DEV(dev); CHECK_OUT(supported);
    if (bits_per_symbol(m) == 0)
        return INTEGRIVE_ERR_INVALID_ARG;

    /* The specification permits modulation to be fixed at synthesis, and an
       application is expected to discover which case it has.  Answering "all
       five" on a bitstream built for one would be the worst kind of wrong:
       every call would succeed and the results would be silently meaningless.
       No capability register exists yet, so the answer comes from
       INTEGRIVE_BITSTREAM_NBPSC in the environment, falling back to the
       compile-time default.  One binary then serves every bitstream. */
    if (dev->bs_nbpsc == 0)
        *supported = 1;             /* adaptive: chosen per packet */
    else
        *supported = (bits_per_symbol(m) == dev->bs_nbpsc);
    return INTEGRIVE_OK;
}

integrive_status_t integrive_is_fec_supported(integrive_device_t *dev,
                                              integrive_fec_t f, int *supported)
{
    CHECK_DEV(dev); CHECK_OUT(supported);
    switch (f) {
    case INTEGRIVE_FEC_RS_255_215:
    case INTEGRIVE_FEC_RS_765_645:
        *supported = 1;   /* the same three-codeword construction */
        break;
    case INTEGRIVE_FEC_NONE:
        *supported = 0;   /* the decoder cannot be bypassed in this bitstream */
        break;
    default:
        return INTEGRIVE_ERR_INVALID_ARG;
    }
    return INTEGRIVE_OK;
}

integrive_status_t integrive_get_version(integrive_device_t  *dev,
                                         integrive_version_t *v)
{
    char buf[64];

    CHECK_DEV(dev); CHECK_OUT(v);
    memset(v, 0, sizeof(*v));
    v->sdk_major = ITG_SDK_MAJOR;
    v->sdk_minor = ITG_SDK_MINOR;
    v->sdk_patch = ITG_SDK_PATCH;

    if (itg_sysfs_read_str("/sys/class/fpga_manager/fpga0", "name",
                           buf, sizeof(buf)) == 0)
        snprintf(v->bitstream_id, sizeof(v->bitstream_id), "%s", buf);
    else
        snprintf(v->bitstream_id, sizeof(v->bitstream_id), "unknown");

    if (itg_sysfs_read_str(ITG_IIO_DEV, "name",
                           v->driver_version, sizeof(v->driver_version)) != 0)
        snprintf(v->driver_version, sizeof(v->driver_version), "unknown");

    return INTEGRIVE_OK;
}

/* ---- 7. RF configuration ----------------------------------------------- */
integrive_status_t integrive_set_frequency_hz(integrive_device_t *dev,
                                              unsigned channel, double hz)
{
    integrive_range_t r;

    CHECK_DEV(dev);
    if (channel != 0)
        return INTEGRIVE_ERR_INVALID_ARG;
    integrive_get_capability(dev, INTEGRIVE_CAP_FREQUENCY_HZ, &r);
    if (hz < r.min || hz > r.max)
        return INTEGRIVE_ERR_OUT_OF_RANGE;

    if (itg_sysfs_write_ll(ITG_IIO_DEV, "out_altvoltage0_RX_LO_frequency",
                           (long long)hz) != 0)
        return INTEGRIVE_ERR_IO;
    if (itg_sysfs_write_ll(ITG_IIO_DEV, "out_altvoltage1_TX_LO_frequency",
                           (long long)hz) != 0)
        return INTEGRIVE_ERR_IO;

    dev->freq_hz = hz;
    mark_configured(dev);
    return INTEGRIVE_OK;
}

integrive_status_t integrive_get_frequency_hz(integrive_device_t *dev,
                                              unsigned channel, double *hz)
{
    CHECK_DEV(dev); CHECK_OUT(hz);
    if (channel != 0)
        return INTEGRIVE_ERR_INVALID_ARG;
    if (itg_sysfs_read_dbl(ITG_IIO_DEV, "out_altvoltage0_RX_LO_frequency", hz) != 0)
        *hz = dev->freq_hz;
    return INTEGRIVE_OK;
}

integrive_status_t integrive_set_sample_rate_sps(integrive_device_t *dev,
                                                 double sps)
{
    integrive_status_t st;

    CHECK_DEV(dev);
    st = require_stopped(dev);
    if (st != INTEGRIVE_OK)
        return st;
    if (fabs(sps - 15.36e6) > 1.0)
        return INTEGRIVE_ERR_OUT_OF_RANGE;

    if (itg_sysfs_write_ll(ITG_IIO_DEV, "in_voltage_sampling_frequency",
                           (long long)sps) != 0 ||
        itg_sysfs_write_ll(ITG_IIO_DEV, "out_voltage_sampling_frequency",
                           (long long)sps) != 0)
        return INTEGRIVE_ERR_IO;

    dev->sample_rate_sps = sps;
    mark_configured(dev);
    return INTEGRIVE_OK;
}

integrive_status_t integrive_get_sample_rate_sps(integrive_device_t *dev,
                                                 double *sps)
{
    CHECK_DEV(dev); CHECK_OUT(sps);
    if (itg_sysfs_read_dbl(ITG_IIO_DEV, "in_voltage_sampling_frequency", sps) != 0)
        *sps = dev->sample_rate_sps;
    return INTEGRIVE_OK;
}

integrive_status_t integrive_set_bandwidth_hz(integrive_device_t *dev, double hz)
{
    CHECK_DEV(dev);
    (void)hz;
    /* Writing in_voltage_rf_bandwidth by hand has been measured to trigger a
       progressive degradation of the AD9361 that does not recover without a
       power cycle.  The analog bandwidth is set once, by the FIR header
       loaded at configuration time.  Refusing here is the honest answer. */
    return INTEGRIVE_ERR_UNSUPPORTED;
}

integrive_status_t integrive_get_bandwidth_hz(integrive_device_t *dev, double *hz)
{
    CHECK_DEV(dev); CHECK_OUT(hz);
    if (itg_sysfs_read_dbl(ITG_IIO_DEV, "in_voltage_rf_bandwidth", hz) != 0)
        *hz = dev->bandwidth_hz;
    return INTEGRIVE_OK;
}

integrive_status_t integrive_set_tx_gain_db(integrive_device_t *dev,
                                            unsigned channel, double db)
{
    char buf[32];

    CHECK_DEV(dev);
    if (channel != 0)
        return INTEGRIVE_ERR_INVALID_ARG;
    /* Hard ceiling: see ITG_TX_GAIN_DB_MAX. */
    if (db < ITG_TX_GAIN_DB_MIN || db > ITG_TX_GAIN_DB_MAX)
        return INTEGRIVE_ERR_OUT_OF_RANGE;

    snprintf(buf, sizeof(buf), "%.2f", db);
    if (itg_sysfs_write_str(ITG_IIO_DEV, "out_voltage0_hardwaregain", buf) != 0)
        return INTEGRIVE_ERR_IO;

    dev->tx_gain_db = db;
    mark_configured(dev);
    return INTEGRIVE_OK;
}

integrive_status_t integrive_get_tx_gain_db(integrive_device_t *dev,
                                            unsigned channel, double *db)
{
    CHECK_DEV(dev); CHECK_OUT(db);
    if (channel != 0)
        return INTEGRIVE_ERR_INVALID_ARG;
    if (itg_sysfs_read_dbl(ITG_IIO_DEV, "out_voltage0_hardwaregain", db) != 0)
        *db = dev->tx_gain_db;
    return INTEGRIVE_OK;
}

integrive_status_t integrive_set_rx_gain_db(integrive_device_t *dev,
                                            unsigned channel, double db)
{
    char buf[32];

    CHECK_DEV(dev);
    if (channel != 0)
        return INTEGRIVE_ERR_INVALID_ARG;
    if (db < ITG_RX_GAIN_DB_MIN || db > ITG_RX_GAIN_DB_MAX)
        return INTEGRIVE_ERR_OUT_OF_RANGE;

    /* Manual gain: the automatic loop chases the idle noise floor between
       packets and arrives at the wrong point for a burst measurement. */
    if (itg_sysfs_write_str(ITG_IIO_DEV, "in_voltage0_gain_control_mode",
                            "manual") != 0)
        return INTEGRIVE_ERR_IO;
    snprintf(buf, sizeof(buf), "%d", (int)(db + 0.5));
    if (itg_sysfs_write_str(ITG_IIO_DEV, "in_voltage0_hardwaregain", buf) != 0)
        return INTEGRIVE_ERR_IO;

    dev->rx_gain_db = db;
    mark_configured(dev);
    return INTEGRIVE_OK;
}

integrive_status_t integrive_get_rx_gain_db(integrive_device_t *dev,
                                            unsigned channel, double *db)
{
    CHECK_DEV(dev); CHECK_OUT(db);
    if (channel != 0)
        return INTEGRIVE_ERR_INVALID_ARG;
    if (itg_sysfs_read_dbl(ITG_IIO_DEV, "in_voltage0_hardwaregain", db) != 0)
        *db = dev->rx_gain_db;
    return INTEGRIVE_OK;
}

/* ---- 8. PHY configuration ---------------------------------------------- */
integrive_status_t integrive_set_modulation(integrive_device_t *dev,
                                            integrive_modulation_t m)
{
    integrive_status_t st;
    int supported = 0;

    CHECK_DEV(dev);
    st = require_stopped(dev);
    if (st != INTEGRIVE_OK)
        return st;
    integrive_is_modulation_supported(dev, m, &supported);
    if (!supported)
        return INTEGRIVE_ERR_UNSUPPORTED;

    /* The transmitter replays a frame built for one modulation, and the frame
       is held in a read-only memory inside the bitstream.  Changing it at run
       time needs the sample-replay path of Chapter 6A, which is a separate
       feature; report honestly rather than accept and ignore. */
    if (signal_field_index(m) != signal_field_index(dev->modulation))
        return INTEGRIVE_ERR_UNSUPPORTED;

    dev->modulation = m;
    mark_configured(dev);
    return INTEGRIVE_OK;
}

integrive_status_t integrive_get_modulation(integrive_device_t *dev,
                                            integrive_modulation_t *m)
{
    CHECK_DEV(dev); CHECK_OUT(m);
    *m = dev->modulation;
    return INTEGRIVE_OK;
}

integrive_status_t integrive_set_demodulation(integrive_device_t *dev,
                                              integrive_modulation_t m)
{
    integrive_status_t st;
    int supported = 0;

    CHECK_DEV(dev);
    /* Section 6.10 classes demodulation as Stopped, alongside modulation. */
    st = require_stopped(dev);
    if (st != INTEGRIVE_OK)
        return st;
    integrive_is_modulation_supported(dev, m, &supported);
    if (!supported)
        return INTEGRIVE_ERR_UNSUPPORTED;

    /* With an adaptive bitstream there is nothing to write: the receiver reads
       the modulation from each packet's SIGNAL field.  With a fixed one, the
       only legal value is the one it was built for, and is_modulation_supported
       above has already rejected anything else. */
    dev->demodulation = m;
    mark_configured(dev);
    return INTEGRIVE_OK;
}

integrive_status_t integrive_get_demodulation(integrive_device_t *dev,
                                              integrive_modulation_t *m)
{
    CHECK_DEV(dev); CHECK_OUT(m);
    *m = dev->demodulation;
    return INTEGRIVE_OK;
}

integrive_status_t integrive_set_fec(integrive_device_t *dev,
                                     integrive_fec_t f)
{
    integrive_status_t st;
    int supported = 0;

    CHECK_DEV(dev);
    st = require_stopped(dev);
    if (st != INTEGRIVE_OK)
        return st;
    if (integrive_is_fec_supported(dev, f, &supported) != INTEGRIVE_OK)
        return INTEGRIVE_ERR_INVALID_ARG;
    if (!supported)
        return INTEGRIVE_ERR_UNSUPPORTED;

    /* RS_255_215 and RS_765_645 name the same construction: three
       RS(255,215) codewords in sequence, no interleaving.  They differ only
       in the granularity the application sees, so both are accepted and
       neither changes the hardware. */
    dev->fec = f;
    mark_configured(dev);
    return INTEGRIVE_OK;
}

integrive_status_t integrive_get_fec(integrive_device_t *dev,
                                     integrive_fec_t *f)
{
    CHECK_DEV(dev); CHECK_OUT(f);
    *f = dev->fec;
    return INTEGRIVE_OK;
}

integrive_status_t integrive_set_ofdm_config(integrive_device_t *dev,
                                             const integrive_ofdm_config_t *cfg)
{
    integrive_status_t st;

    CHECK_DEV(dev); CHECK_OUT(cfg);
    st = require_stopped(dev);
    if (st != INTEGRIVE_OK)
        return st;

    /* The allocation is fixed by the bitstream: transform length, prefix,
       and the data/pilot split are all structural.  Accept the values that
       match and reject the rest, so that a caller writing the reference
       configuration of a different bitstream finds out immediately. */
    if (cfg->fft_size != ITG_FFT_SIZE || cfg->cp_length != ITG_CP_LENGTH)
        return INTEGRIVE_ERR_OUT_OF_RANGE;
    if (cfg->data_subcarriers  != ITG_DATA_SC ||
        cfg->pilot_subcarriers != ITG_PILOT_SC)
        return INTEGRIVE_ERR_UNSUPPORTED;

    dev->ofdm = *cfg;
    mark_configured(dev);
    return INTEGRIVE_OK;
}

integrive_status_t integrive_get_ofdm_config(integrive_device_t *dev,
                                             integrive_ofdm_config_t *cfg)
{
    CHECK_DEV(dev); CHECK_OUT(cfg);
    *cfg = dev->ofdm;
    return INTEGRIVE_OK;
}

integrive_status_t integrive_set_fft_size(integrive_device_t *dev, uint32_t n)
{
    integrive_ofdm_config_t cfg;

    CHECK_DEV(dev);
    cfg = dev->ofdm;
    cfg.fft_size = n;
    return integrive_set_ofdm_config(dev, &cfg);
}

integrive_status_t integrive_set_cp_length(integrive_device_t *dev, uint32_t n)
{
    integrive_ofdm_config_t cfg;

    CHECK_DEV(dev);
    cfg = dev->ofdm;
    cfg.cp_length = n;
    return integrive_set_ofdm_config(dev, &cfg);
}

integrive_status_t integrive_get_subcarrier_spacing_hz(integrive_device_t *dev,
                                                       double *hz)
{
    CHECK_DEV(dev); CHECK_OUT(hz);
    *hz = dev->sample_rate_sps / (double)dev->ofdm.fft_size;
    return INTEGRIVE_OK;
}

integrive_status_t integrive_get_symbol_rate_hz(integrive_device_t *dev,
                                                double *hz)
{
    CHECK_DEV(dev); CHECK_OUT(hz);
    *hz = dev->sample_rate_sps
        / (double)(dev->ofdm.fft_size + dev->ofdm.cp_length);
    return INTEGRIVE_OK;
}

integrive_status_t integrive_get_payload_rate_bps(integrive_device_t *dev,
                                                  double *bps)
{
    double  r_sym;
    unsigned b;

    CHECK_DEV(dev); CHECK_OUT(bps);
    integrive_get_symbol_rate_hz(dev, &r_sym);
    b = bits_per_symbol(dev->modulation);

    /* Gross PHY payload rate: data subcarriers x bits x code rate.
       Framing overhead is not included -- see the specification. */
    *bps = r_sym * (double)dev->ofdm.data_subcarriers * (double)b
         * ((double)ITG_RS_K / (double)ITG_RS_N);
    return INTEGRIVE_OK;
}

integrive_status_t integrive_set_packet_config(
        integrive_device_t *dev, const integrive_packet_config_t *cfg)
{
    integrive_status_t st;

    CHECK_DEV(dev); CHECK_OUT(cfg);
    st = require_stopped(dev);
    if (st != INTEGRIVE_OK)
        return st;
    if (cfg->pattern == INTEGRIVE_PATTERN_USER &&
        (!cfg->user_pattern || cfg->user_pattern_len == 0))
        return INTEGRIVE_ERR_INVALID_ARG;

    /* The frame carries a whole number of RS groups. */
    if (cfg->payload_bytes == 0 || cfg->payload_bytes % ITG_MSG_PER_GROUP)
        return INTEGRIVE_ERR_OUT_OF_RANGE;

    /* The transmitted frame lives in a read-only memory, so a generator
       cannot be selected -- that would need the transmit-side sample replay
       of Chapter 6A.  PATTERN_USER is different: it does not ask the
       transmitter to change, it TELLS the library what the transmitter
       already sends.  Supplying it turns the statistics of Section 10 from
       an inference into a measurement, because every received frame can then
       be compared byte for byte against the truth. */
    if (cfg->pattern != dev->pkt.pattern &&
        cfg->pattern != INTEGRIVE_PATTERN_USER)
        return INTEGRIVE_ERR_UNSUPPORTED;

    if (cfg->pattern == INTEGRIVE_PATTERN_USER &&
        cfg->user_pattern_len > sizeof(dev->ref_payload))
        return INTEGRIVE_ERR_OUT_OF_RANGE;

    dev->pkt = *cfg;
    dev->ref_len = 0;
    if (cfg->pattern == INTEGRIVE_PATTERN_USER) {
        memcpy(dev->ref_payload, cfg->user_pattern, cfg->user_pattern_len);
        dev->ref_len = cfg->user_pattern_len;
        dev->ref_crc = frame_has_crc(dev->ref_payload, dev->ref_len);
        /* Keep our own copy: the caller's buffer is not required to outlive
           the call, and the pointer in the struct is const. */
        dev->pkt.user_pattern = dev->ref_payload;
    }
    mark_configured(dev);
    return INTEGRIVE_OK;
}

integrive_status_t integrive_get_packet_config(integrive_device_t *dev,
                                               integrive_packet_config_t *cfg)
{
    CHECK_DEV(dev); CHECK_OUT(cfg);
    *cfg = dev->pkt;
    return INTEGRIVE_OK;
}

/* ---- 9. runtime control -------------------------------------------------
 * One transmitted frame is one measurement.  The sequence below is the one
 * the hardware requires and is easy to get wrong:
 *
 *   1. pulse the PHY reset  BEFORE clearing the counters, not after;
 *   2. fire the frame;
 *   3. wait ~60 ms before reading, or the counters are read mid-flight.
 *
 * Because the counters are cleared per frame, every frame is scored on its
 * own and the totals below are accumulated in software.
 * ----------------------------------------------------------------------- */
/* Push one stored frame through the RX chain via the rx_replay trigger, so the
   byte-capture RAM fills with exactly this frame's stream.  This is the path
   every working decode tool uses.  fire_one_frame() drives the DAC instead,
   which transmits but does not fill the capture RAM in step -- good enough to
   move the frame counters, but the B5 capture then reads a stale, mid-frame
   window that is not a whole codeword and never decodes.  reg5's stream select
   (B5_MODE) must already be set when this runs, so the frame flows out the
   coded-symbol tap. */
static void trigger_rx_replay(integrive_device_t *dev)
{
    itg_wr(dev->hw.rx,  ITG_RX_REG3, ITG_RX3_CLR);
    itg_wr(dev->hw.rx,  ITG_RX_REG3, 0);
    itg_wr(dev->hw.xpu, ITG_XPU_MULTI_RST, 1);
    itg_wr(dev->hw.xpu, ITG_XPU_MULTI_RST, 0);
    itg_wr(dev->hw.rx,  ITG_RX_REG3, ITG_RX3_REPLAY);
    sleep_ms(200);                     /* let the whole frame stream out */
    itg_wr(dev->hw.rx,  ITG_RX_REG3, 0);
    sleep_ms(50);
}

static void fire_one_frame(integrive_device_t *dev)
{
    /* Reset BEFORE clearing the counters, not after. */
    itg_wr(dev->hw.xpu, ITG_XPU_MULTI_RST, 1);
    itg_wr(dev->hw.xpu, ITG_XPU_MULTI_RST, 0);
    itg_wr(dev->hw.rx,  ITG_RX_REG3, ITG_RX3_CLR);
    itg_wr(dev->hw.rx,  ITG_RX_REG3, 0);

    /* Absolute writes, 0 -> bit3 -> 0.  Not read-modify-write: this register
       also carries transmit control bits and the proven recipe drives it
       whole. */
    itg_wr(dev->hw.tx, ITG_TX_REG7, ITG_TX_FIRE_SEQ0);
    itg_wr(dev->hw.tx, ITG_TX_REG7, ITG_TX_FIRE_SEQ1);
    itg_wr(dev->hw.tx, ITG_TX_REG7, ITG_TX_FIRE_SEQ0);
}

static void read_frame_counters(integrive_device_t *dev,
                                uint32_t *total, uint32_t *pass)
{
    /* Write the selector to reg3, read the value from the readback register.
       Re-reading reg3 returns the selector itself, which looks like a
       plausible counter and is silently wrong. */
    itg_wr(dev->hw.rx, ITG_RX_REG3, ITG_RX3_TOTAL);
    *total = itg_rd(dev->hw.rx, ITG_RX_READBACK);
    itg_wr(dev->hw.rx, ITG_RX_REG3, ITG_RX3_PASS);
    *pass  = itg_rd(dev->hw.rx, ITG_RX_READBACK);
    itg_wr(dev->hw.rx, ITG_RX_REG3, 0);
}

/* Section 6.11: "Combinations that are individually valid but jointly
   inconsistent are checked when the configuration is latched at
   integrive_start(), not at the individual set call.  A failure there is
   reported through the CONFIG_ERROR flag in the status word, and the PHY does
   not enter RUNNING."  Returns 0 if the set is consistent. */
static int config_is_consistent(const integrive_device_t *dev)
{
    unsigned bits, groups, msg_bytes;

    /* The frame carries a whole number of RS groups, and how many depends on
       BOTH the symbol count and the modulation: groups = nsym * bits / 8. */
    bits = bits_per_symbol(dev->modulation);
    if (bits == 0)
        return 0;
    if ((ITG_FRAME_SYMBOLS * bits) % 8u != 0u)
        return 0;                       /* not RS-group aligned */
    groups    = (ITG_FRAME_SYMBOLS * bits) / 8u;
    msg_bytes = groups * ITG_MSG_PER_GROUP;

    /* The payload the caller asked for must be the payload this framing
       produces; otherwise every statistic would be scored against the wrong
       length and the mismatch would not surface until results were compared. */
    if (dev->pkt.payload_bytes != msg_bytes)
        return 0;

    /* A reference payload must cover exactly one frame. */
    if (dev->ref_len != 0 && dev->ref_len != msg_bytes)
        return 0;

    /* Transmit and receive must agree unless the receiver reads the
       modulation from each packet. */
    if (dev->bs_nbpsc != 0 && dev->demodulation != dev->modulation)
        return 0;

    return 1;
}

integrive_status_t integrive_start(integrive_device_t *dev)
{
    CHECK_DEV(dev);
    if (dev->state == INTEGRIVE_STATE_RUNNING)
        return INTEGRIVE_ERR_INVALID_STATE;

    if (!config_is_consistent(dev)) {
        dev->sticky_flags |= INTEGRIVE_FLAG_CONFIG_ERROR;
        return INTEGRIVE_ERR_INVALID_STATE;   /* PHY does not enter RUNNING */
    }
    dev->sticky_flags &= ~(uint32_t)INTEGRIVE_FLAG_CONFIG_ERROR;

    /* Allocate the batch buffers once, 64-byte aligned per Section 3.1, so the
       callback addresses them directly (6A.5: no copy is made). */
    if (dev->ps_fn && dev->ps_out != INTEGRIVE_PS_NONE) {
        if (!dev->ps_buf_in &&
            posix_memalign((void **)&dev->ps_buf_in, 64, dev->ps_bytes) != 0)
            return INTEGRIVE_ERR_IO;
        if (!dev->ps_buf_out &&
            posix_memalign((void **)&dev->ps_buf_out, 64, dev->ps_bytes) != 0)
            return INTEGRIVE_ERR_IO;
    }

    /* Structural parameters latch here (Guide §7). */
    itg_wr(dev->hw.xpu, ITG_XPU_MULTI_RST, 1);
    itg_wr(dev->hw.xpu, ITG_XPU_MULTI_RST, 0);
    itg_wr(dev->hw.rx,  ITG_RX_REG3, ITG_RX3_CLR);
    itg_wr(dev->hw.rx,  ITG_RX_REG3, 0);

    /* Arm the stall only when a span is actually configured.  With no span the
       bit stays clear and the chain behaves exactly as before. */
    itg_wr(dev->hw.xpu, ITG_XPU_REG6,
           (dev->ps_fn && dev->ps_out != INTEGRIVE_PS_NONE)
               ? (ITG_REG6_SPAN_EN |
                  ((dev->ps_in & 7u) << ITG_REG6_BND_SHIFT))
               : 0u);

    dev->state = INTEGRIVE_STATE_RUNNING;
    return INTEGRIVE_OK;
}

integrive_status_t integrive_stop(integrive_device_t *dev)
{
    CHECK_DEV(dev);
    if (dev->state != INTEGRIVE_STATE_RUNNING)
        return INTEGRIVE_ERR_INVALID_STATE;
    /* Never leave the chain held: a stalled receiver looks like a dead one. */
    ps_span_release(dev);
    dev->state = INTEGRIVE_STATE_CONFIGURED;
    return INTEGRIVE_OK;
}

/* start_tx / start_rx exist so one direction can be halted without leaving
   the RUNNING state.  In this bitstream the receiver is always listening and
   the transmitter only emits when a frame is fired, so the pair is a
   bookkeeping distinction rather than two independent engines. */
integrive_status_t integrive_start_tx(integrive_device_t *dev)
{
    CHECK_DEV(dev);
    if (dev->state != INTEGRIVE_STATE_RUNNING)
        return INTEGRIVE_ERR_INVALID_STATE;
    dev->sticky_flags |= INTEGRIVE_FLAG_TX_ACTIVE;
    return INTEGRIVE_OK;
}

integrive_status_t integrive_stop_tx(integrive_device_t *dev)
{
    CHECK_DEV(dev);
    dev->sticky_flags &= ~(uint32_t)INTEGRIVE_FLAG_TX_ACTIVE;
    return INTEGRIVE_OK;
}

integrive_status_t integrive_start_rx(integrive_device_t *dev)
{
    CHECK_DEV(dev);
    if (dev->state != INTEGRIVE_STATE_RUNNING)
        return INTEGRIVE_ERR_INVALID_STATE;
    dev->sticky_flags |= INTEGRIVE_FLAG_RX_ACTIVE;
    return INTEGRIVE_OK;
}

integrive_status_t integrive_stop_rx(integrive_device_t *dev)
{
    CHECK_DEV(dev);
    dev->sticky_flags &= ~(uint32_t)INTEGRIVE_FLAG_RX_ACTIVE;
    return INTEGRIVE_OK;
}

integrive_status_t integrive_wait_packets(integrive_device_t *dev,
                                          uint64_t packets, uint32_t timeout_ms)
{
    uint64_t deadline;
    uint64_t sent = 0;

    CHECK_DEV(dev);
    if (dev->state != INTEGRIVE_STATE_RUNNING)
        return INTEGRIVE_ERR_INVALID_STATE;
    if (packets == 0)
        return INTEGRIVE_ERR_INVALID_ARG;

    deadline = now_ms() + (uint64_t)timeout_ms;

    while (sent < packets) {
        uint32_t total = 0, pass = 0, diag;
        /* True whenever a block is substituted onto the PS (any span, B5 or
           B4), which is exactly when the capture branch below runs.  The frame
           source changes with it: a span is fed by rx_replay so the capture
           RAM fills in step; with no span, the DAC is fired as before. */
        int      fec   = (dev->ps_fn != NULL &&
                          dev->ps_out != INTEGRIVE_PS_NONE);
        uint32_t r5_saved = 0;

        if (timeout_ms && now_ms() > deadline)
            return INTEGRIVE_ERR_TIMEOUT;

        if (fec) {
            /* While a block is substituted, the frame source is rx_replay, not
               the DAC: it is the trigger that fills the byte-capture RAM in
               step (see trigger_rx_replay).  Select the tap's stream in reg5
               FIRST so this one replay serves both the counters and the B5
               capture below; the branch restores reg5 afterward. */
            r5_saved = itg_rd(dev->hw.xpu, ITG_XPU_REG5);
            {
                uint32_t r5 = r5_saved & ~(uint32_t)(ITG_REG5_B5_MODE |
                                                     ITG_REG5_ER_MODE |
                                                     ITG_REG5_B4_MODE |
                                                     ITG_REG5_B3_MODE |
                                                     ITG_REG5_B2_MODE);
                if (dev->ps_out == 5u) r5 |= ITG_REG5_B5_MODE;
                if (dev->ps_out == 4u) r5 |= ITG_REG5_B4_MODE;
                itg_wr(dev->hw.xpu, ITG_XPU_REG5, r5);
            }
            trigger_rx_replay(dev);
        } else {
            fire_one_frame(dev);
            sleep_ms(60);                 /* settle before reading */
        }
        read_frame_counters(dev, &total, &pass);

        dev->tx_packets++;
        sent++;
        if (total)
            dev->rx_packets++;
        else
            dev->sync_failures++;

        /* `pass` means the decoder reported no uncorrectable codeword.  It is
           NOT a byte-exact verdict -- frames have been observed to pass while
           most of the payload was wrong -- so it is counted as a FEC result
           and nothing stronger is claimed from it. */
        /* ★ 6A.10: any counter generated inside the substituted block stops
           advancing, and the application computes the equivalent.  `pass` is
           the verdict of the PL Reed-Solomon decoder, so while the FEC decoder
           is the substituted block this counter would describe the very block
           being bypassed -- present, plausible, and about the wrong decoder,
           which is worse than a counter that visibly stops. */
        if (total && !pass && !fec_runs_on_ps(dev))
            dev->fec_errors++;

        /* With a reference payload the frame can be scored properly: read the
           decoded bytes back and count the bytes and bits that actually
           differ.  This is the difference between "the decoder did not
           complain" and "the payload arrived intact". */
        if (dev->ref_len && total) {
            static uint8_t rx[8192];
            size_t got = 0, i, n;

            if (read_capture_bytes(dev, 0, rx, dev->ref_len, &got)
                    == INTEGRIVE_OK) {
                uint64_t berr = 0;
                int      exact = 1;

                n = (got < dev->ref_len) ? got : dev->ref_len;
                for (i = 0; i < n; i++) {
                    uint8_t d = (uint8_t)(rx[i] ^ dev->ref_payload[i]);
                    if (d) {
                        exact = 0;
                        while (d) { berr += (d & 1u); d >>= 1; }
                    }
                }
                dev->bits_compared += (uint64_t)n * 8u;
                dev->bit_errors    += berr;
                if (exact && n == dev->ref_len)
                    dev->frames_exact++;

                /* If the frame format carries a CRC-32, score it as a CRC
                   result; otherwise report a payload mismatch, which is the
                   stronger check and is named as such in the README. */
                if (dev->ref_crc) {
                    if (!frame_has_crc(rx, n))
                        dev->crc_errors++;
                } else if (!exact || n != dev->ref_len) {
                    dev->crc_errors++;
                }
            }
        }

        diag = itg_rd(dev->hw.xpu, ITG_XPU_PHY_DIAG);
        if (diag & (1u << 21))
            dev->sticky_flags |= INTEGRIVE_FLAG_SYNC_LOCK;

        /* PS span active: the chain is holding at the exit boundary.  Read the
           batch, hand it to the application's module (one invocation per
           batch, 6A.5, on an SDK-owned thread, under the configured timeout),
           then return the result at the entry boundary and let the chain
           resume.  That is the execution model of Section 6A.6. */
        if (dev->ps_fn && dev->ps_out != INTEGRIVE_PS_NONE) {
            size_t got = 0, produced = 0;
            integrive_status_t pst;

            /* The capture stream was selected in reg5 at the top of the loop
               (B5_MODE for ps_out==5, B4_MODE for ==4; no bit = the decoded
               message at B6), and one frame was already replayed under it, so
               the byte-capture RAM now holds THIS boundary's stream.  Getting
               that select wrong hands the module the decoded message at B6
               instead of the coded bytes at B5 -- the decoder then re-decodes
               already-decoded data.  reg5 is restored to r5_saved below.

                 B4 -> equalized samples (REG5_B4_MODE)
                 B5 -> coded bytes       (REG5_B5_MODE)
                 B6 -> message bytes     (no bit set) */
            if (read_capture_bytes(dev, 0, dev->ps_buf_in,
                                   dev->ps_bytes, &got) == INTEGRIVE_OK) {
                /* ★ The erasure bitmap CANNOT be obtained here.  PHY spec 3.3
                   places it in the same B5 buffer, but capture_frame_bytes(mode 2)
                   calls fire_one_frame() -- it TRANSMITS A NEW FRAME and captures
                   that.  The bitmap it returns belongs to a DIFFERENT FRAME than
                   the symbol array in hand.  Erasure marks from the wrong frame
                   are worse than none: they tell the decoder to discard symbols
                   that were correct.

                   The fabric carries ONE stream at a time, so this is a HARDWARE
                   limitation, not an SDK omission.  Fixing it needs a second read
                   path in fabric.  See docs/conformance.md section 2. */

                  /* Trim the fixed 8192-byte capture window to the frame's real coded
                     length; the RAM past the frame is stale.  Without this the callback
                     is handed 8192 bytes -- not a whole number of codewords -- and the
                     RS decoder rejects it. */
                  if (fec_runs_on_ps(dev)) {
                      size_t coded = b5_coded_bytes(dev);
                      if (coded && got > coded) got = coded;
                  }

                pst = ps_run_batch(dev, got, &produced);
                if (pst != INTEGRIVE_OK) {
                    ps_span_release(dev);   /* never leave the chain stalled */
                    itg_wr(dev->hw.xpu, ITG_XPU_REG5, r5_saved);
                    return pst;
                }
                ps_return_bytes(dev, dev->ps_buf_out, produced);
            }
            itg_wr(dev->hw.xpu, ITG_XPU_REG5, r5_saved);   /* always restore */
        }

        dev->frames_scored++;
    }
    return INTEGRIVE_OK;
}

/* ---- 10. status and statistics ----------------------------------------- */
integrive_status_t integrive_get_status(integrive_device_t *dev,
                                        integrive_status_info_t *info)
{
    CHECK_DEV(dev); CHECK_OUT(info);
    memset(info, 0, sizeof(*info));

    info->tx_packets    = dev->tx_packets;
    info->rx_packets    = dev->rx_packets;
    info->crc_errors    = dev->crc_errors;
    info->fec_errors    = dev->fec_errors;
    info->sync_failures = dev->sync_failures;

    if (dev->ref_len && dev->tx_packets) {
        /* Measured: a frame counts as received only if every payload byte
           matches. */
        info->per = 1.0 - (double)dev->frames_exact / (double)dev->tx_packets;
    } else {
        /* Inferred from the decoder's own verdict, which is weaker. */
        info->per = (dev->tx_packets == 0) ? 0.0
                  : 1.0 - (double)(dev->rx_packets - dev->fec_errors)
                          / (double)dev->tx_packets;
    }

    /* Post-FEC BER is real only when a reference payload was supplied through
       INTEGRIVE_PATTERN_USER; without one the bitstream reports per-codeword
       success, not per-bit errors.  A separate validity flag is used rather
       than a zero sentinel, because zero is a legitimate result on a good
       link. */
    if (dev->bits_compared) {
        info->ber       = (double)dev->bit_errors / (double)dev->bits_compared;
        info->ber_valid = 1;
    } else {
        info->ber       = 0.0;
        info->ber_valid = 0;
    }

    info->snr_db = 0.0;
    info->flags  = dev->sticky_flags | INTEGRIVE_FLAG_PHY_READY;
    return INTEGRIVE_OK;
}

integrive_status_t integrive_clear_statistics(integrive_device_t *dev)
{
    CHECK_DEV(dev);
    dev->tx_packets = dev->rx_packets = 0;
    dev->crc_errors = dev->fec_errors = dev->sync_failures = 0;
    dev->frames_scored = dev->frames_exact = 0;
    dev->bits_compared = dev->bit_errors = 0;
    itg_wr(dev->hw.rx, ITG_RX_REG3, ITG_RX3_CLR);
    itg_wr(dev->hw.rx, ITG_RX_REG3, 0);
    return INTEGRIVE_OK;
}

integrive_status_t integrive_get_error_flags(integrive_device_t *dev,
                                             uint32_t *flags)
{
    CHECK_DEV(dev); CHECK_OUT(flags);
    *flags = dev->sticky_flags;
    return INTEGRIVE_OK;
}

integrive_status_t integrive_clear_error_flags(integrive_device_t *dev)
{
    CHECK_DEV(dev);
    /* Only the sticky fault bits are cleared; the activity bits reflect the
       present state and are not the caller's to reset. */
    dev->sticky_flags &= (uint32_t)(INTEGRIVE_FLAG_TX_ACTIVE |
                                    INTEGRIVE_FLAG_RX_ACTIVE);
    return INTEGRIVE_OK;
}

/* ---- 11. PS substitution ------------------------------------------------
 * Boundary numbering for this bitstream.  The specification leaves the
 * numbering TBC and requires it to be fixed against the released bitstream;
 * this table is that fixing, and it is exposed through the three boundary
 * queries so no application has to hard-code it.
 *
 * The receive chain of the specification lists channel processing and the
 * demodulator as separate blocks.  Here equalisation and slicing happen in
 * one decision-feedback block, so B4 is the boundary inside it where the
 * equalised subcarriers exist; it is named accordingly and marked untapped
 * until the tap is built.
 * ----------------------------------------------------------------------- */
static const char *const rx_boundary_name[] = {
    "B0 antenna to digital RX",
    "B1 digital RX to synchronization",
    "B2 synchronization to FFT and demapping",
    "B3 FFT and demapping to channel processing",
    "B4 channel processing to demodulator",
    "B5 demodulator to FEC decoder",
    "B6 FEC decoder to CRC and packet decoder",
    "B7 CRC and packet decoder to host"
};

static const char *const tx_boundary_name[] = {
    "B0 host to packet formatter",
    "B1 packet formatter to FEC encoder",
    "B2 FEC encoder to modulator",
    "B3 modulator to OFDM mapping",
    "B4 OFDM mapping and IFFT to digital TX",
    "B5 digital TX to RF",
    "B6 RF to antenna"
};

/* Which boundaries have a DMA tap in this bitstream.  B0 and the last
   boundary of a chain are never tap points (spec 6A.4). */
static int rx_boundary_tapped(uint32_t b)
{
    switch (b) {
    case 1: return 1;   /* raw IQ capture, proven                       */
    case 2: return 1;   /* CFO-corrected samples into the FFT           */
    case 3: return 1;   /* bin-extracted subcarriers into channel proc  */
    case 4: return 1;   /* equalised subcarriers into the demodulator   */
    case 5: return 1;   /* coded bytes to the RS decoder                */
    case 6: return 1;   /* decoded message bytes                        */
    default: return 0;  /* B0 and B7 are never taps (6A.4)              */
    }
}

static int tx_boundary_tapped(uint32_t b)
{
    return (b == 5);    /* sample replay into the transmit path         */
}

integrive_status_t integrive_get_boundary_count(integrive_device_t *dev,
                                                integrive_chain_t chain,
                                                uint32_t *count)
{
    CHECK_DEV(dev); CHECK_OUT(count);
    switch (chain) {
    case INTEGRIVE_CHAIN_RX:
        *count = (uint32_t)(sizeof(rx_boundary_name) / sizeof(rx_boundary_name[0]));
        return INTEGRIVE_OK;
    case INTEGRIVE_CHAIN_TX:
        *count = (uint32_t)(sizeof(tx_boundary_name) / sizeof(tx_boundary_name[0]));
        return INTEGRIVE_OK;
    }
    return INTEGRIVE_ERR_INVALID_ARG;
}

integrive_status_t integrive_get_boundary_name(integrive_device_t *dev,
                                               integrive_chain_t chain,
                                               uint32_t b, char *name,
                                               size_t name_len)
{
    const char *const *tbl;
    uint32_t n = 0;

    CHECK_DEV(dev); CHECK_OUT(name);
    if (name_len == 0)
        return INTEGRIVE_ERR_INVALID_ARG;
    if (integrive_get_boundary_count(dev, chain, &n) != INTEGRIVE_OK)
        return INTEGRIVE_ERR_INVALID_ARG;
    if (b >= n)
        return INTEGRIVE_ERR_OUT_OF_RANGE;

    tbl = (chain == INTEGRIVE_CHAIN_RX) ? rx_boundary_name : tx_boundary_name;
    snprintf(name, name_len, "%s", tbl[b]);
    return INTEGRIVE_OK;
}

integrive_status_t integrive_is_boundary_tapped(integrive_device_t *dev,
                                                integrive_chain_t chain,
                                                uint32_t b, int *tapped)
{
    uint32_t n = 0;

    CHECK_DEV(dev); CHECK_OUT(tapped);
    if (integrive_get_boundary_count(dev, chain, &n) != INTEGRIVE_OK)
        return INTEGRIVE_ERR_INVALID_ARG;
    if (b >= n)
        return INTEGRIVE_ERR_OUT_OF_RANGE;

    *tapped = (chain == INTEGRIVE_CHAIN_RX) ? rx_boundary_tapped(b)
                                            : tx_boundary_tapped(b);
    return INTEGRIVE_OK;
}

integrive_status_t integrive_set_ps_span(integrive_device_t *dev,
                                         integrive_chain_t chain,
                                         uint32_t out_b, uint32_t in_b)
{
    integrive_status_t st;
    uint32_t n = 0;
    int      t_out = 0, t_in = 0;

    CHECK_DEV(dev);
    st = require_stopped(dev);
    if (st != INTEGRIVE_OK)
        return st;

    /* Returning to the pure PL path. */
    if (out_b == INTEGRIVE_PS_NONE && in_b == INTEGRIVE_PS_NONE) {
        dev->ps_chain = chain;
        dev->ps_out = dev->ps_in = INTEGRIVE_PS_NONE;
        return INTEGRIVE_OK;
    }

    if (integrive_get_boundary_count(dev, chain, &n) != INTEGRIVE_OK)
        return INTEGRIVE_ERR_INVALID_ARG;
    if (out_b >= n || in_b >= n)
        return INTEGRIVE_ERR_OUT_OF_RANGE;

    /* Adjacency: exactly one block is substituted (spec 6A.4). */
    if (in_b != out_b + 1)
        return INTEGRIVE_ERR_UNSUPPORTED;
    /* B0 and the final boundary are not tap points. */
    if (out_b == 0 || in_b == n - 1)
        return INTEGRIVE_ERR_UNSUPPORTED;

    integrive_is_boundary_tapped(dev, chain, out_b, &t_out);
    integrive_is_boundary_tapped(dev, chain, in_b,  &t_in);
    if (!t_out || !t_in)
        return INTEGRIVE_ERR_UNSUPPORTED;

    dev->ps_chain = chain;
    dev->ps_out   = out_b;
    dev->ps_in    = in_b;
    mark_configured(dev);
    return INTEGRIVE_OK;
}

integrive_status_t integrive_get_ps_span(integrive_device_t *dev,
                                         integrive_chain_t *chain,
                                         uint32_t *out_b, uint32_t *in_b)
{
    CHECK_DEV(dev); CHECK_OUT(chain); CHECK_OUT(out_b); CHECK_OUT(in_b);
    *chain = dev->ps_chain;
    *out_b = dev->ps_out;
    *in_b  = dev->ps_in;
    return INTEGRIVE_OK;
}

integrive_status_t integrive_set_ps_callback(integrive_device_t *dev,
                                             integrive_ps_fn_t fn, void *user)
{
    integrive_status_t st;

    CHECK_DEV(dev);
    st = require_stopped(dev);
    if (st != INTEGRIVE_OK)
        return st;
    dev->ps_fn   = fn;
    dev->ps_user = user;
    mark_configured(dev);
    return INTEGRIVE_OK;
}

integrive_status_t integrive_set_ps_buffer_bytes(integrive_device_t *dev,
                                                 size_t bytes)
{
    integrive_status_t st;
    integrive_range_t  r;

    CHECK_DEV(dev);
    st = require_stopped(dev);
    if (st != INTEGRIVE_OK)
        return st;
    integrive_get_capability(dev, INTEGRIVE_CAP_PS_BUFFER_BYTES, &r);
    if ((double)bytes < r.min || (double)bytes > r.max)
        return INTEGRIVE_ERR_OUT_OF_RANGE;
    dev->ps_bytes = bytes;
    mark_configured(dev);
    return INTEGRIVE_OK;
}

integrive_status_t integrive_get_ps_buffer_bytes(integrive_device_t *dev,
                                                 size_t *bytes)
{
    CHECK_DEV(dev); CHECK_OUT(bytes);
    *bytes = dev->ps_bytes;
    return INTEGRIVE_OK;
}

integrive_status_t integrive_set_ps_timeout_ms(integrive_device_t *dev,
                                               uint32_t ms)
{
    integrive_status_t st;

    CHECK_DEV(dev);
    st = require_stopped(dev);
    if (st != INTEGRIVE_OK)
        return st;
    if (ms == 0)
        return INTEGRIVE_ERR_INVALID_ARG;
    dev->ps_timeout_ms = ms;
    mark_configured(dev);
    return INTEGRIVE_OK;
}

integrive_status_t integrive_get_ps_timeout_ms(integrive_device_t *dev,
                                               uint32_t *ms)
{
    CHECK_DEV(dev); CHECK_OUT(ms);
    *ms = dev->ps_timeout_ms;
    return INTEGRIVE_OK;
}

/* ---- reading a tapped boundary ------------------------------------------
 * The receive chain captures a whole frame into a buffer in the fabric; the
 * bytes are read back four to a 32-bit word through one register port.  Which
 * stream fills that buffer is selected by a single bit:
 *
 *   B6 (decoded message bytes) -- the normal path, bit clear
 *   B5 (coded bytes, before the FEC decoder) -- bit set
 *
 * The bit is restored before returning, so a failed call leaves the device
 * exactly as it found it.
 * ----------------------------------------------------------------------- */
/* Read what is already in the capture buffer.  Does not fire a frame, so the
   measurement loop can score the frame it just sent. */
static integrive_status_t read_capture_bytes(integrive_device_t *dev,
                                             int b5_mode, uint8_t *buf,
                                             size_t capacity, size_t *len)
{
    uint32_t w;
    size_t   nbytes, i;

    (void)b5_mode;   /* the stream is selected by the caller through reg5 */
    nbytes = (capacity < ITG_CAP_BYTES) ? capacity : ITG_CAP_BYTES;
    if (nbytes < 4)
        return INTEGRIVE_ERR_INVALID_ARG;

    for (i = 0; i + 4 <= nbytes; i += 4) {
        itg_wr(dev->hw.rx, ITG_RX_REG3,
               ITG_RX3_BYTE | ((uint32_t)(i >> 2) << ITG_RX3_BYTE_SHIFT));
        w = itg_rd(dev->hw.rx, ITG_RX_READBACK);
        buf[i + 0] = (uint8_t)(w & 0xffu);          /* little endian in the word */
        buf[i + 1] = (uint8_t)((w >> 8)  & 0xffu);
        buf[i + 2] = (uint8_t)((w >> 16) & 0xffu);
        buf[i + 3] = (uint8_t)((w >> 24) & 0xffu);
    }
    itg_wr(dev->hw.rx, ITG_RX_REG3, 0);
    *len = i;
    return INTEGRIVE_OK;
}

/* mode: 0 = decoded message bytes (B6), 1 = coded bytes (B5),
         2 = the B5 erasure bitmap, 3 = equalised subcarriers (B4),
         4 = bin-extracted subcarriers (B3), 5 = CFO-corrected samples (B2). */
static integrive_status_t capture_frame_bytes(integrive_device_t *dev,
                                              int mode,
                                              uint8_t *buf, size_t capacity,
                                              size_t *len)
{
    uint32_t reg5_saved, reg5;
    integrive_status_t st;

    reg5_saved = itg_rd(dev->hw.xpu, ITG_XPU_REG5);
    reg5 = reg5_saved & ~(uint32_t)(ITG_REG5_B5_MODE | ITG_REG5_ER_MODE |
                                    ITG_REG5_B4_MODE | ITG_REG5_B3_MODE |
                                    ITG_REG5_B2_MODE);
    if (mode == 1) reg5 |= ITG_REG5_B5_MODE;
    if (mode == 2) reg5 |= ITG_REG5_ER_MODE;
    if (mode == 3) reg5 |= ITG_REG5_B4_MODE;   /* equalized samples at B4     */
    if (mode == 4) reg5 |= ITG_REG5_B3_MODE;   /* bin-extracted subcarriers   */
    if (mode == 5) reg5 |= ITG_REG5_B2_MODE;   /* CFO-corrected samples       */
    itg_wr(dev->hw.xpu, ITG_XPU_REG5, reg5);

    fire_one_frame(dev);
    sleep_ms(60);

    st = read_capture_bytes(dev, mode, buf, capacity, len);

    itg_wr(dev->hw.xpu, ITG_XPU_REG5, reg5_saved);   /* always restore */
    return st;
}

integrive_status_t integrive_read_boundary(integrive_device_t *dev,
                                           integrive_chain_t chain,
                                           uint32_t boundary,
                                           void *buf, size_t capacity,
                                           size_t *len)
{
    int tapped = 0;

    CHECK_DEV(dev); CHECK_OUT(buf); CHECK_OUT(len);
    if (dev->state == INTEGRIVE_STATE_RUNNING)
        return INTEGRIVE_ERR_INVALID_STATE;
    if (chain != INTEGRIVE_CHAIN_RX)
        return INTEGRIVE_ERR_UNSUPPORTED;

    if (integrive_is_boundary_tapped(dev, chain, boundary, &tapped)
            != INTEGRIVE_OK)
        return INTEGRIVE_ERR_OUT_OF_RANGE;
    if (!tapped)
        return INTEGRIVE_ERR_UNSUPPORTED;

    switch (boundary) {
    case 2:  return capture_frame_bytes(dev, 5, (uint8_t *)buf, capacity, len);
    case 3:  return capture_frame_bytes(dev, 4, (uint8_t *)buf, capacity, len);
    case 4:  return capture_frame_bytes(dev, 3, (uint8_t *)buf, capacity, len);
    case 5:  return capture_frame_bytes(dev, 1, (uint8_t *)buf, capacity, len);
    case 6:  return capture_frame_bytes(dev, 0, (uint8_t *)buf, capacity, len);
    default: return INTEGRIVE_ERR_UNSUPPORTED;   /* B1 needs the IQ capture arm */
    }
}


/* ---- Chapter 6A execution model ----------------------------------------
 * The specification's programming model is a callback the SDK invokes once
 * per batch, with a per-batch timeout that aborts the run.  This implements
 * that model in the batch form Section 6A.9 permits for the receive chain:
 * a finite window is captured at full rate, then processed.
 *
 * The data plane stalls at the exit boundary while the PS callback processes
 * the captured batch.  The processed result is then written to the fabric
 * return buffer and re-enters the chain at the entry boundary, allowing the
 * downstream blocks to continue processing the PS-generated data.
 * ----------------------------------------------------------------------- */
struct ps_call {
    integrive_device_t *dev;
    size_t              in_len;
    size_t              out_len;
    int                 rc;
};

static void *ps_thread(void *arg)
{
    struct ps_call *c = (struct ps_call *)arg;
    c->out_len = 0;
    c->rc = c->dev->ps_fn(c->dev->ps_buf_in, c->in_len,
                          c->dev->ps_buf_out, c->dev->ps_bytes,
                          &c->out_len, c->dev->ps_user);
    return NULL;
}

/* Runs one batch through the registered callback.  Returns OK, ABORTED (the
   callback returned non-zero) or TIMEOUT. */
static integrive_status_t ps_run_batch(integrive_device_t *dev,
                                       size_t in_len, size_t *out_len)
{
    struct ps_call  c;
    pthread_t       th;
    struct timespec deadline;
    int             rc;

    c.dev = dev; c.in_len = in_len; c.out_len = 0; c.rc = 0;

    /* The callback runs on a thread owned by the SDK, not the caller's thread
       (6A.5), which is also what makes the timeout enforceable. */
    if (pthread_create(&th, NULL, ps_thread, &c) != 0)
        return INTEGRIVE_ERR_IO;

    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec  += (time_t)(dev->ps_timeout_ms / 1000u);
    deadline.tv_nsec += (long)(dev->ps_timeout_ms % 1000u) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_nsec -= 1000000000L;
        deadline.tv_sec  += 1;
    }

    rc = pthread_timedjoin_np(th, NULL, &deadline);
    if (rc != 0) {
        /* The run is aborted; the thread is left detached rather than killed,
           because cancelling it could leave the application's own state torn. */
        pthread_detach(th);
        dev->ps_aborts++;
        return INTEGRIVE_ERR_TIMEOUT;
    }

    dev->ps_batches++;
    if (c.rc != 0) {
        dev->ps_aborts++;
        return INTEGRIVE_ERR_ABORTED;
    }
    *out_len = c.out_len;
    return INTEGRIVE_OK;
}

integrive_status_t integrive_read_erasures(integrive_device_t *dev,
                                           integrive_chain_t chain,
                                           uint32_t boundary,
                                           void *buf, size_t capacity,
                                           size_t *len)
{
    CHECK_DEV(dev); CHECK_OUT(buf); CHECK_OUT(len);
    if (dev->state == INTEGRIVE_STATE_RUNNING)
        return INTEGRIVE_ERR_INVALID_STATE;
    if (chain != INTEGRIVE_CHAIN_RX || boundary != 5u)
        return INTEGRIVE_ERR_UNSUPPORTED;
    return capture_frame_bytes(dev, 2, (uint8_t *)buf, capacity, len);
}

/* ---- the return half of the span ---------------------------------------
 * Writes the module's output into the fabric's return buffer and releases the
 * stall, so the blocks after the entry boundary run on the PS's data rather
 * than the decoder's.  One byte per register write with a toggling flag; at a
 * few thousand bytes per frame this is milliseconds, which is what batch mode
 * (6A.9) is for.
 * ----------------------------------------------------------------------- */
static void ps_return_bytes(integrive_device_t *dev,
                            const uint8_t *buf, size_t n)
{
    uint32_t reg6;
    size_t   i;
    unsigned tog = 0;

    if (n > 8192u)
        n = 8192u;

    /* Rewind the write pointer, then stream the bytes in. */
    itg_wr(dev->hw.xpu, ITG_XPU_REG6, ITG_REG6_SPAN_EN | ITG_REG6_PTR_RST);
    itg_wr(dev->hw.xpu, ITG_XPU_REG6, ITG_REG6_SPAN_EN);
    for (i = 0; i < n; i++) {
        tog ^= 1u;
        itg_wr(dev->hw.xpu, ITG_XPU_REG7,
               ((uint32_t)tog << 8) | (uint32_t)buf[i]);
    }

    /* Release: a rising edge on GO emits n bytes downstream. */
    reg6 = ITG_REG6_SPAN_EN | ((uint32_t)n << ITG_REG6_LEN_SHIFT);
    itg_wr(dev->hw.xpu, ITG_XPU_REG6, reg6);
    itg_wr(dev->hw.xpu, ITG_XPU_REG6, reg6 | ITG_REG6_GO);
    itg_wr(dev->hw.xpu, ITG_XPU_REG6, reg6);
}

/* Drop the stall unconditionally.  Leaving the chain held after an aborted run
   would wedge the next one, and the fault would look like a dead receiver. */
static void ps_span_release(integrive_device_t *dev)
{
    itg_wr(dev->hw.xpu, ITG_XPU_REG6, 0);
}
