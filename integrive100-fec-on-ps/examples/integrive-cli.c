/* integrive-cli.c -- command-line front end for libintegrive.
 *
 *   integrive-cli info
 *   integrive-cli caps
 *   integrive-cli boundaries
 *   integrive-cli run --frames N [--tx-gain dB] [--rx-gain dB] [--freq Hz]
 */
#include "integrive.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(call)                                                        \
    do {                                                                   \
        integrive_status_t s_ = (call);                                    \
        if (s_ != INTEGRIVE_OK) {                                          \
            fprintf(stderr, "%s: %s\n", #call, integrive_strerror(s_));    \
            return 1;                                                      \
        }                                                                  \
    } while (0)

static const char *mod_name(integrive_modulation_t m)
{
    static const char *n[] = { "BPSK", "QPSK", "16-QAM", "64-QAM", "256-QAM" };
    return ((unsigned)m < 5u) ? n[m] : "?";
}

static int cmd_info(integrive_device_t *dev)
{
    integrive_version_t v;
    double f, sps, bw, txg, rxg, sc, sym, rate;
    integrive_modulation_t m;

    CHECK(integrive_get_version(dev, &v));
    CHECK(integrive_get_frequency_hz(dev, 0, &f));
    CHECK(integrive_get_sample_rate_sps(dev, &sps));
    CHECK(integrive_get_bandwidth_hz(dev, &bw));
    CHECK(integrive_get_tx_gain_db(dev, 0, &txg));
    CHECK(integrive_get_rx_gain_db(dev, 0, &rxg));
    CHECK(integrive_get_modulation(dev, &m));
    CHECK(integrive_get_subcarrier_spacing_hz(dev, &sc));
    CHECK(integrive_get_symbol_rate_hz(dev, &sym));
    CHECK(integrive_get_payload_rate_bps(dev, &rate));

    printf("SDK            %u.%u.%u\n", v.sdk_major, v.sdk_minor, v.sdk_patch);
    printf("bitstream      %s\n", v.bitstream_id);
    printf("driver         %s\n", v.driver_version);
    printf("frequency      %.6f MHz\n", f / 1e6);
    printf("sample rate    %.6f MSPS\n", sps / 1e6);
    printf("bandwidth      %.3f MHz\n", bw / 1e6);
    printf("TX gain        %.2f dB\n", txg);
    printf("RX gain        %.2f dB\n", rxg);
    printf("modulation     %s\n", mod_name(m));
    printf("SC spacing     %.2f kHz\n", sc / 1e3);
    printf("symbol rate    %.2f kHz\n", sym / 1e3);
    printf("payload rate   %.3f Mbit/s\n", rate / 1e6);
    return 0;
}

static int cmd_caps(integrive_device_t *dev)
{
    static const char *cap_name[] = {
        "frequency_hz", "sample_rate_sps", "bandwidth_hz", "tx_gain_db",
        "rx_gain_db", "fft_size", "cp_length", "payload_bytes",
        "tx_channels", "rx_channels", "ps_buffer_bytes"
    };
    unsigned i;

    for (i = 0; i < sizeof(cap_name) / sizeof(cap_name[0]); i++) {
        integrive_range_t r;
        if (integrive_get_capability(dev, (integrive_capability_t)i, &r)
                != INTEGRIVE_OK)
            continue;
        if (r.is_list) {
            unsigned k;
            printf("%-18s list:", cap_name[i]);
            for (k = 0; k < r.list_len; k++)
                printf(" %g", r.list[k]);
            printf("  default %g\n", r.def);
        } else {
            printf("%-18s %g .. %g  step %g  default %g\n",
                   cap_name[i], r.min, r.max, r.step, r.def);
        }
    }

    for (i = 0; i < 5u; i++) {
        int sup = 0;
        integrive_is_modulation_supported(dev, (integrive_modulation_t)i, &sup);
        printf("modulation %-8s %s\n", mod_name((integrive_modulation_t)i),
               sup ? "supported" : "not built");
    }
    return 0;
}

static int cmd_boundaries(integrive_device_t *dev)
{
    int chain;

    for (chain = 0; chain < 2; chain++) {
        uint32_t n = 0, b;
        integrive_chain_t c = (integrive_chain_t)chain;

        CHECK(integrive_get_boundary_count(dev, c, &n));
        printf("%s chain, %u boundaries:\n", chain ? "RX" : "TX", n);
        for (b = 0; b < n; b++) {
            char name[80];
            int  tapped = 0;
            integrive_get_boundary_name(dev, c, b, name, sizeof(name));
            integrive_is_boundary_tapped(dev, c, b, &tapped);
            printf("  %-46s %s\n", name, tapped ? "tapped" : "-");
        }
    }
    return 0;
}

/* Load the payload the transmitter is known to send.  Supplying it turns the
   statistics from "the decoder did not complain" into a byte-for-byte
   measurement -- the difference matters, because frames have been observed to
   satisfy the decoder while most of the payload was wrong. */
static int load_reference(integrive_device_t *dev, const char *path)
{
    static uint8_t ref[8192];
    integrive_packet_config_t cfg;
    FILE  *f;
    size_t n;

    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        return 1;
    }
    n = fread(ref, 1, sizeof(ref), f);
    fclose(f);
    if (n == 0) {
        fprintf(stderr, "%s is empty\n", path);
        return 1;
    }

    CHECK(integrive_get_packet_config(dev, &cfg));
    cfg.pattern          = INTEGRIVE_PATTERN_USER;
    cfg.user_pattern     = ref;
    cfg.user_pattern_len = (uint32_t)n;
    cfg.payload_bytes    = (uint32_t)n;
    CHECK(integrive_set_packet_config(dev, &cfg));
    printf("reference payload %zu bytes from %s\n", n, path);
    return 0;
}

static int cmd_run(integrive_device_t *dev, int argc, char **argv)
{
    integrive_status_info_t info;
    unsigned long frames = 100;
    int i;

    for (i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            frames = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--tx-gain") && i + 1 < argc)
            CHECK(integrive_set_tx_gain_db(dev, 0, strtod(argv[++i], NULL)));
        else if (!strcmp(argv[i], "--rx-gain") && i + 1 < argc)
            CHECK(integrive_set_rx_gain_db(dev, 0, strtod(argv[++i], NULL)));
        else if (!strcmp(argv[i], "--freq") && i + 1 < argc)
            CHECK(integrive_set_frequency_hz(dev, 0, strtod(argv[++i], NULL)));
        else if (!strcmp(argv[i], "--reference") && i + 1 < argc)
            if (load_reference(dev, argv[++i]) != 0)
                return 1;
    }

    CHECK(integrive_start(dev));
    CHECK(integrive_clear_statistics(dev));
    CHECK(integrive_wait_packets(dev, frames, 0));
    CHECK(integrive_get_status(dev, &info));
    CHECK(integrive_stop(dev));

    printf("frames sent     %llu\n", (unsigned long long)info.tx_packets);
    printf("frames detected %llu\n", (unsigned long long)info.rx_packets);
    printf("sync failures   %llu\n", (unsigned long long)info.sync_failures);
    printf("FEC errors      %llu\n", (unsigned long long)info.fec_errors);
    printf("PER             %.4f%s\n", info.per,
           info.ber_valid ? "  (measured against the reference)"
                          : "  (inferred from the decoder)");
    if (info.ber_valid)
        printf("BER             %.3e\n", info.ber);
    else
        printf("BER             not measured -- pass --reference <file>\n");
    printf("flags           0x%08x\n", info.flags);
    return 0;
}

/* ps: run the (B5, B6) span -- read the coded bytes, decode them here on the
   ARM, and compare against what the fabric decoder produced for the same
   frame.  Two decoders over one capture is the cheapest way to tell a PS
   module that works from one that merely runs. */
extern int ps_rs_decode(const void *in, size_t in_len,
                        void *out, size_t out_capacity,
                        size_t *out_len, void *user);

static int cmd_ps(integrive_device_t *dev, int argc, char **argv)
{
    static uint8_t msg_pl[8192];
    integrive_status_info_t info;
    unsigned long frames = 10;
    size_t   npl = 0;
    int      i;

    for (i = 0; i < argc; i++)
        if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            frames = strtoul(argv[++i], NULL, 0);

    /* Substitute the FEC decoder: data leaves at B5, the module below decodes
       it, and the result would re-enter at B6.  Selecting the pair is how a
       span is named -- Chapter 6A does not name blocks. */
    CHECK(integrive_set_ps_span(dev, INTEGRIVE_CHAIN_RX, 5, 6));
    CHECK(integrive_set_ps_buffer_bytes(dev, 65536));
    CHECK(integrive_set_ps_timeout_ms(dev, 100));
    CHECK(integrive_set_ps_callback(dev, ps_rs_decode, NULL));

    CHECK(integrive_start(dev));
    CHECK(integrive_clear_statistics(dev));

    {   /* The SDK now invokes the callback once per batch; a non-zero return
           aborts the run and surfaces as ABORTED, a slow one as TIMEOUT. */
        integrive_status_t st = integrive_wait_packets(dev, frames, 0);
        if (st != INTEGRIVE_OK) {
            fprintf(stderr, "run aborted: %s\n", integrive_strerror(st));
            integrive_stop(dev);
            return 1;
        }
    }
    CHECK(integrive_get_status(dev, &info));
    CHECK(integrive_stop(dev));

    /* Compare the PS module's answer against the fabric decoder for the last
       frame: two decoders over one capture is the cheapest way to tell a
       module that works from one that merely runs. */
    CHECK(integrive_set_ps_span(dev, INTEGRIVE_CHAIN_RX,
                                INTEGRIVE_PS_NONE, INTEGRIVE_PS_NONE));
    CHECK(integrive_set_ps_callback(dev, NULL, NULL));
    CHECK(integrive_read_boundary(dev, INTEGRIVE_CHAIN_RX, 6,
                                  msg_pl, sizeof(msg_pl), &npl));

    printf("batches through the PS module  %lu\n", frames);
    printf("frames detected                %llu\n",
           (unsigned long long)info.rx_packets);
    printf("PL bytes read back at B6       %zu\n", npl);
    printf("flags                          0x%08x\n", info.flags);
    return 0;
}

int main(int argc, char **argv)
{
    integrive_device_t *dev = NULL;
    integrive_status_t  st;
    int rc;

    if (argc < 2) {
        fprintf(stderr,
                "usage: %s {info|caps|boundaries|run [--frames N] "
                "[--tx-gain dB] [--rx-gain dB] [--freq Hz] "
                "[--reference <file>]|ps [--frames N]}\n", argv[0]);
        return 2;
    }

    st = integrive_open(&dev);
    if (st != INTEGRIVE_OK) {
        fprintf(stderr, "open: %s\n", integrive_strerror(st));
        return 1;
    }

    if      (!strcmp(argv[1], "info"))       rc = cmd_info(dev);
    else if (!strcmp(argv[1], "caps"))       rc = cmd_caps(dev);
    else if (!strcmp(argv[1], "boundaries")) rc = cmd_boundaries(dev);
    else if (!strcmp(argv[1], "run"))        rc = cmd_run(dev, argc - 2, argv + 2);
    else if (!strcmp(argv[1], "ps"))         rc = cmd_ps(dev, argc - 2, argv + 2);
    else {
        fprintf(stderr, "unknown command: %s\n", argv[1]);
        rc = 2;
    }

    integrive_close(dev);
    return rc;
}
