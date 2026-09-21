/* =========================================================================
 * integrive.h -- Integrive-100 SDK public C interface
 *
 * Conforms to: ITG-100-API-001 v1.0 (normative).
 * Naming rules N1..N10, argument order 2.3, return convention 2.4.
 *
 * Every function returns integrive_status_t.  INTEGRIVE_OK is 0, all errors
 * are negative, no positive value is ever returned.  Results come back
 * through output pointers.  A failed call leaves all state unchanged.
 * ========================================================================= */
#ifndef INTEGRIVE_H
#define INTEGRIVE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 12. Versioning ---------------------------------------------------- */
#define INTEGRIVE_API_VERSION_MAJOR 1
#define INTEGRIVE_API_VERSION_MINOR 0

#define INTEGRIVE_API_AT_LEAST(maj, min)                    \
    (INTEGRIVE_API_VERSION_MAJOR > (maj) ||                 \
     (INTEGRIVE_API_VERSION_MAJOR == (maj) &&               \
      INTEGRIVE_API_VERSION_MINOR >= (min)))

/* ---- 3.1 Handle -------------------------------------------------------- */
/* Opaque.  The definition is not part of the public interface.
   A handle is not thread-safe; concurrent use needs external serialization. */
typedef struct integrive_device integrive_device_t;

/* ---- 4. Error codes ---------------------------------------------------- */
typedef enum {
    INTEGRIVE_OK                = 0,
    INTEGRIVE_ERR_INVALID_ARG   = -1,
    INTEGRIVE_ERR_OUT_OF_RANGE  = -2,   /* value unsupported: query capability */
    INTEGRIVE_ERR_INVALID_STATE = -3,   /* right call, wrong time: stop first  */
    INTEGRIVE_ERR_UNSUPPORTED   = -4,   /* absent from this bitstream          */
    INTEGRIVE_ERR_NO_DEVICE     = -5,
    INTEGRIVE_ERR_BUSY          = -6,
    INTEGRIVE_ERR_TIMEOUT       = -7,
    INTEGRIVE_ERR_IO            = -8,
    INTEGRIVE_ERR_FIRMWARE      = -9,
    INTEGRIVE_ERR_ABORTED       = -10
} integrive_status_t;

/* Never returns NULL.  Valid for the lifetime of the process. */
const char *integrive_strerror(integrive_status_t status);

/* ---- 3.2 Enumerations -------------------------------------------------- */
/* Explicit values: these appear in saved configuration files and logged
   results, so they are part of the data format.  Append, never insert. */
typedef enum {
    INTEGRIVE_MOD_BPSK   = 0,
    INTEGRIVE_MOD_QPSK   = 1,
    INTEGRIVE_MOD_16QAM  = 2,
    INTEGRIVE_MOD_64QAM  = 3,
    INTEGRIVE_MOD_256QAM = 4
} integrive_modulation_t;

typedef enum {
    INTEGRIVE_FEC_NONE       = 0,
    INTEGRIVE_FEC_RS_255_215 = 1,
    INTEGRIVE_FEC_RS_765_645 = 2
} integrive_fec_t;

typedef enum {
    INTEGRIVE_PATTERN_PRBS9  = 0,
    INTEGRIVE_PATTERN_PRBS23 = 1,
    INTEGRIVE_PATTERN_USER   = 2
} integrive_pattern_t;

typedef enum {
    INTEGRIVE_CHAIN_TX = 0,
    INTEGRIVE_CHAIN_RX = 1
} integrive_chain_t;

typedef enum {
    INTEGRIVE_STATE_CLOSED     = 0,
    INTEGRIVE_STATE_OPEN       = 1,
    INTEGRIVE_STATE_CONFIGURED = 2,
    INTEGRIVE_STATE_RUNNING    = 3,
    INTEGRIVE_STATE_ERROR      = 4
} integrive_state_t;

typedef enum {
    INTEGRIVE_CAP_FREQUENCY_HZ    = 0,
    INTEGRIVE_CAP_SAMPLE_RATE_SPS = 1,
    INTEGRIVE_CAP_BANDWIDTH_HZ    = 2,
    INTEGRIVE_CAP_TX_GAIN_DB      = 3,
    INTEGRIVE_CAP_RX_GAIN_DB      = 4,
    INTEGRIVE_CAP_FFT_SIZE        = 5,
    INTEGRIVE_CAP_CP_LENGTH       = 6,
    INTEGRIVE_CAP_PAYLOAD_BYTES   = 7,
    INTEGRIVE_CAP_TX_CHANNELS     = 8,
    INTEGRIVE_CAP_RX_CHANNELS     = 9,
    INTEGRIVE_CAP_PS_BUFFER_BYTES = 10
} integrive_capability_t;

typedef enum {
    INTEGRIVE_FLAG_PHY_READY      = 1u << 0,
    INTEGRIVE_FLAG_TX_ACTIVE      = 1u << 1,
    INTEGRIVE_FLAG_RX_ACTIVE      = 1u << 2,
    INTEGRIVE_FLAG_SYNC_LOCK      = 1u << 3,
    /* ★ NOT IMPLEMENTED in this bitstream: its register map has no FIFO
       overflow/underflow bit, so the two flags below are NEVER set.  Section
       6A.9 treats them as the sign that a batch-mode run is INVALID, so here an
       invalid run looks exactly like a valid one.  Do not rely on them.
       See docs/conformance.md section 3. */
    INTEGRIVE_FLAG_FIFO_OVERFLOW  = 1u << 4,   /* sticky -- not implemented */
    INTEGRIVE_FLAG_FIFO_UNDERFLOW = 1u << 5,   /* sticky -- not implemented */
    INTEGRIVE_FLAG_CONFIG_ERROR   = 1u << 6    /* sticky */
} integrive_flag_t;

/* ---- 3.3 Structures ---------------------------------------------------- */
typedef struct {
    uint32_t fft_size;            /* samples, dimensionless count */
    uint32_t cp_length;           /* samples                      */
    uint32_t data_subcarriers;
    uint32_t pilot_subcarriers;
    const uint32_t *pilot_index;  /* pilot_subcarriers entries    */
} integrive_ofdm_config_t;

typedef struct {
    uint32_t            payload_bytes;
    integrive_pattern_t pattern;
    const uint8_t      *user_pattern;   /* used when pattern is USER */
    uint32_t            user_pattern_len;
    uint32_t            packet_count;   /* 0 means continuous        */
} integrive_packet_config_t;

typedef struct {
    uint64_t tx_packets;
    uint64_t rx_packets;
    uint64_t crc_errors;
    uint64_t fec_errors;
    uint64_t sync_failures;
    double   per;
    double   ber;             /* post-FEC; valid only if ber_valid */
    int      ber_valid;
    double   snr_db;          /* averaged since last clear         */
    uint32_t flags;           /* integrive_flag_t bit field        */
} integrive_status_info_t;

typedef struct {
    uint32_t sdk_major, sdk_minor, sdk_patch;
    char     bitstream_id[64];
    char     driver_version[32];
} integrive_version_t;

typedef struct {
    double   min;
    double   max;
    double   step;        /* 0 if continuous                          */
    double   def;         /* default value                            */
    int      is_list;     /* non-zero if only listed values are valid */
    unsigned list_len;
    double   list[32];
} integrive_range_t;

/* ---- 5. Device lifecycle ----------------------------------------------- */
integrive_status_t integrive_open(integrive_device_t **dev);
integrive_status_t integrive_open_by_index(unsigned index,
                                           integrive_device_t **dev);
integrive_status_t integrive_close(integrive_device_t *dev);
integrive_status_t integrive_reset(integrive_device_t *dev);

integrive_status_t integrive_get_device_count(unsigned *count);
integrive_status_t integrive_get_state(integrive_device_t *dev,
                                       integrive_state_t  *state);

/* ---- 6. Version and capability discovery ------------------------------- */
integrive_status_t integrive_get_capability(integrive_device_t     *dev,
                                            integrive_capability_t  cap,
                                            integrive_range_t      *range);

integrive_status_t integrive_is_modulation_supported(
        integrive_device_t *dev, integrive_modulation_t mod, int *supported);

integrive_status_t integrive_is_fec_supported(
        integrive_device_t *dev, integrive_fec_t fec, int *supported);

integrive_status_t integrive_get_version(integrive_device_t  *dev,
                                         integrive_version_t *version);

/* ---- 7. RF configuration ----------------------------------------------- */
integrive_status_t integrive_set_frequency_hz  (integrive_device_t *dev,
                                                unsigned channel, double hz);
integrive_status_t integrive_get_frequency_hz  (integrive_device_t *dev,
                                                unsigned channel, double *hz);

integrive_status_t integrive_set_sample_rate_sps(integrive_device_t *dev,
                                                 double sps);
integrive_status_t integrive_get_sample_rate_sps(integrive_device_t *dev,
                                                 double *sps);

integrive_status_t integrive_set_bandwidth_hz  (integrive_device_t *dev,
                                                double hz);
integrive_status_t integrive_get_bandwidth_hz  (integrive_device_t *dev,
                                                double *hz);

integrive_status_t integrive_set_tx_gain_db    (integrive_device_t *dev,
                                                unsigned channel, double db);
integrive_status_t integrive_get_tx_gain_db    (integrive_device_t *dev,
                                                unsigned channel, double *db);

integrive_status_t integrive_set_rx_gain_db    (integrive_device_t *dev,
                                                unsigned channel, double db);
integrive_status_t integrive_get_rx_gain_db    (integrive_device_t *dev,
                                                unsigned channel, double *db);

/* ---- 8. PHY configuration ---------------------------------------------- */
integrive_status_t integrive_set_modulation  (integrive_device_t *dev,
                                              integrive_modulation_t mod);
integrive_status_t integrive_get_modulation  (integrive_device_t *dev,
                                              integrive_modulation_t *mod);

integrive_status_t integrive_set_demodulation(integrive_device_t *dev,
                                              integrive_modulation_t mod);
integrive_status_t integrive_get_demodulation(integrive_device_t *dev,
                                              integrive_modulation_t *mod);

integrive_status_t integrive_set_fec         (integrive_device_t *dev,
                                              integrive_fec_t fec);
integrive_status_t integrive_get_fec         (integrive_device_t *dev,
                                              integrive_fec_t *fec);

integrive_status_t integrive_set_ofdm_config(
        integrive_device_t *dev, const integrive_ofdm_config_t *cfg);
integrive_status_t integrive_get_ofdm_config(
        integrive_device_t *dev, integrive_ofdm_config_t *cfg);

integrive_status_t integrive_set_fft_size (integrive_device_t *dev,
                                           uint32_t fft_size);
integrive_status_t integrive_set_cp_length(integrive_device_t *dev,
                                           uint32_t cp_length);

/* Derived, read-only: the absence of a setter is what marks them derived. */
integrive_status_t integrive_get_subcarrier_spacing_hz(
        integrive_device_t *dev, double *hz);
integrive_status_t integrive_get_symbol_rate_hz(
        integrive_device_t *dev, double *hz);
integrive_status_t integrive_get_payload_rate_bps(
        integrive_device_t *dev, double *bps);

integrive_status_t integrive_set_packet_config(
        integrive_device_t *dev, const integrive_packet_config_t *cfg);
integrive_status_t integrive_get_packet_config(
        integrive_device_t *dev, integrive_packet_config_t *cfg);

/* ---- 9. Runtime control ------------------------------------------------ */
integrive_status_t integrive_start   (integrive_device_t *dev);
integrive_status_t integrive_stop    (integrive_device_t *dev);

integrive_status_t integrive_start_tx(integrive_device_t *dev);
integrive_status_t integrive_stop_tx (integrive_device_t *dev);
integrive_status_t integrive_start_rx(integrive_device_t *dev);
integrive_status_t integrive_stop_rx (integrive_device_t *dev);

integrive_status_t integrive_wait_packets(integrive_device_t *dev,
                                          uint64_t            packets,
                                          uint32_t            timeout_ms);

/* ---- 10. Status and statistics ----------------------------------------- */
integrive_status_t integrive_get_status(integrive_device_t      *dev,
                                        integrive_status_info_t *info);

integrive_status_t integrive_clear_statistics(integrive_device_t *dev);

integrive_status_t integrive_get_error_flags(integrive_device_t *dev,
                                             uint32_t           *flags);
integrive_status_t integrive_clear_error_flags(integrive_device_t *dev);

/* ---- 11. PS substitution ----------------------------------------------- */
#define INTEGRIVE_PS_NONE  ((uint32_t)-1)

typedef int (*integrive_ps_fn_t)(const void *in,  size_t in_len,
                                 void       *out, size_t out_capacity,
                                 size_t     *out_len,
                                 void       *user);

integrive_status_t integrive_set_ps_span(integrive_device_t *dev,
                                         integrive_chain_t   chain,
                                         uint32_t            boundary_out,
                                         uint32_t            boundary_in);
integrive_status_t integrive_get_ps_span(integrive_device_t *dev,
                                         integrive_chain_t  *chain,
                                         uint32_t           *boundary_out,
                                         uint32_t           *boundary_in);

integrive_status_t integrive_set_ps_callback(integrive_device_t *dev,
                                             integrive_ps_fn_t   fn,
                                             void               *user);

integrive_status_t integrive_set_ps_buffer_bytes(integrive_device_t *dev,
                                                 size_t              bytes);
integrive_status_t integrive_get_ps_buffer_bytes(integrive_device_t *dev,
                                                 size_t             *bytes);

integrive_status_t integrive_set_ps_timeout_ms(integrive_device_t *dev,
                                               uint32_t            ms);
integrive_status_t integrive_get_ps_timeout_ms(integrive_device_t *dev,
                                               uint32_t           *ms);

integrive_status_t integrive_get_boundary_count(integrive_device_t *dev,
                                                integrive_chain_t   chain,
                                                uint32_t           *count);
integrive_status_t integrive_get_boundary_name(integrive_device_t *dev,
                                               integrive_chain_t   chain,
                                               uint32_t            boundary,
                                               char               *name,
                                               size_t              name_len);
integrive_status_t integrive_is_boundary_tapped(integrive_device_t *dev,
                                                integrive_chain_t   chain,
                                                uint32_t            boundary,
                                                int                *tapped);

/* Read one batch from a tapped boundary into the caller's buffer.
 *
 * This is the read half of a span: it captures what the data plane produced at
 * `boundary` for the most recent frame.  It exists alongside the callback
 * interface because the receive chain of this bitstream runs in the batch mode
 * of Guide 6A.9 -- a finite window is captured at full rate and processed
 * afterwards -- so an application that only wants to see a boundary, or to run
 * its own decoder over it, does not need to install a callback at all.
 *
 * The device must be stopped.  Returns INTEGRIVE_ERR_UNSUPPORTED for a
 * boundary that carries no tap in this bitstream.
 */
integrive_status_t integrive_read_boundary(integrive_device_t *dev,
                                           integrive_chain_t   chain,
                                           uint32_t            boundary,
                                           void               *buf,
                                           size_t              capacity,
                                           size_t             *len);

/* Read the erasure bitmap that accompanies RX B5: one bit per coded symbol,
 * packed LSB-first, a set bit marking the symbol as unreliable (PHY spec 3.3).
 * It is fetched as a separate capture because the fabric carries one stream at
 * a time, so it belongs to the frame captured by the call that precedes it.
 * Returns UNSUPPORTED for any boundary other than RX B5.
 */
integrive_status_t integrive_read_erasures(integrive_device_t *dev,
                                           integrive_chain_t   chain,
                                           uint32_t            boundary,
                                           void               *buf,
                                           size_t              capacity,
                                           size_t             *len);

#ifdef __cplusplus
}
#endif
#endif /* INTEGRIVE_H */
