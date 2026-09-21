#!/bin/sh
# =============================================================================
#  ad9361_autoconfig.sh  --  ONE-SHOT boot-time bring-up for the SIPDDM PHY
#                            (openwifi neptunesdr / Zynq-7020 + AD9364)
# -----------------------------------------------------------------------------
#  PURPOSE
#    Auto-run right after boot (wired into the init=/bin/sh shell) to bring the
#    board to a DECODE-READY state: it fully configures the AD9361 transceiver
#    (via the IIO sysfs / debugfs) AND the FPGA blocks (via devmem) so that
#    openofdm_rx can decode SIPDDM frames (1024-FFT, CP=26, 256-QAM, RS(255,215),
#    10 OFDM symbols = 6450 bytes = 14674 IQ samples, golden first-3 = b2 3a c9).
#
#    CONFIG ONLY (+ an OPTIONAL, safe digital self-test).  It never runs
#    tx_seqcap, never programs the PL, never reboots, never touches QSPI.
#
#  USAGE
#    sh /rooThe board default login is username: **`root`**, password: **`root`**t/ad9361_autoconfig.sh [MODE] [FS_HZ] [LO_HZ] [TXGAIN_dB]
#
#      MODE      digital_loopback  (DEFAULT) | ota_rx | rf_loopback
#      FS_HZ     AD9361 sampleThe board default login is username: **`root`**, password: **`root`** rate.  DEFAULT 61440000 (61.44 Msps).
#                Allowed: 61440000 (default) | 30720000 | 40000000 (FIR path).
#      LO_HZ     RF carrier for ota_rx / rf_loopback.  DEFAULT 2412000000 (ch1).
#                MUST equal the far-end transmitter's LO.
#      TXGAIN_dB rf_loopback TX hardwaregain.  DEFAULT -30 (SAFE attenuator).
#
#    Env knobs (optional):
#      SELFTEST=0/1   digital_loopback boot self-test (dac_replay decode loop).
#                     DEFAULT 1 (safe: dac_replay + latch read, NO S2MM DMA).
#      SELFTEST_N=N   number of self-test frames (DEFAULT 8).
#      FIR_FILE=path  TX/RX FIR .ftr (DEFAULT /root/fir.ftr).  40 MHz design.
#
#  MODES
#    digital_loopback  AD9361 BIST DATA_PORT_LOOP (loopback=1).  DAC driven by
#                      the FPGA (dac_replay golden) -> DAC -> AD9361 digital loop
#                      -> ADC -> adc_intf -> openofdm_rx.  NO analog RF, NO FIR.
#                      PROVEN byte-exact 6450/6450 at BOTH 30.72 AND 61.44 Msps
#                      this session (b2 3a c9, RS-pass, latch 0xB2C93AB2).
#    ota_rx            Real over-the-air RX.  loopback=0, RX_LO==TX_LO at a real
#                      carrier, RF bandwidth 50 MHz, slow_attack AGC, DC/quad
#                      tracking, real ADC source, openofdm_rx hw_init.
#                      Metric = RS-pass (NOT byte-exact: analog transform).
#    rf_loopback       ota_rx + TX enabled (TX_LO, TX FIR emission-enabler when
#                      Fs=40M, TX hardwaregain -30 dB default) for an SMA-cable
#                      self-loopback.  Emit later via a dac_replay trigger.
#
# =============================================================================
#  GUARDRAILS  (baked in below as comments + runtime checks)
# -----------------------------------------------------------------------------
#  * FRESH PHYSICAL POWER-CYCLE required.  A soft reboot (reboot -f) disrupts the
#    AD9361 DATA_CLK -> "Calibration TIMEOUT (0x244,0x80) -> probe failed -110"
#    -> NO iio:device0.  This script DETECTS a missing iio:device0 and tells you
#    to power-cycle.  ([[rx-1m-powercycle-method]] [[ad9361-config-guardrail]])
#  * NEVER digital_tune / initialize / an 80 MHz build -- they break the RX path
#    (dig_tune returns 0 / fails @61.44; initialize forces Fs back to ~30.72).
#    The bitstream MUST be the sym10-fix/v3 (85 MHz) or instr build.
#  * NEVER `cat` an AD9361 *debugfs* attribute -- a read can reboot the board.
#    This script only ECHO/WRITES debugfs (loopback); it reads back plain sysfs
#    (Fs, BW, LO, FIR-en, TX gain) and devmem registers only.
#  * debugfs + sysfs are NOT auto-mounted under init=/bin/sh -- we mount them
#    FIRST (else `echo 0 > .../loopback` is a silent no-op = no analog path).
#  * `adi,full-port-enable` must be in the devicetree (it is; dtb 8c903f5e) --
#    RX tuning fails 100% without it.  ([[ad9361-fullport-fix]])
#  * The shipped FIR (/root/fir.ftr) is a 40 MHz-Fs design: loading it FORCES
#    Fs=40 MHz and it will NOT enable at 30.72 or 61.44.  So the FIR step is made
#    conditional on Fs==40000000 (see rf_loopback / ota_rx).  digital_loopback
#    does NOT use the FIR and is unaffected.
#  * openofdm_tx and openofdm_rx SHARE ONE FFT and must NEVER run at once.  We
#    disable auto-ACK (ack_tx_disable) so a TX-ACK cannot contend the FFT during
#    an RX decode.  The dac_replay self-test uses the DUMB DAC path (not the FFT-
#    sharing openofdm_tx) + reads the latch (NO S2MM DMA) -> FFT-safe.
#  * At Fs=61.44 on the standard LVDS-61.44-timed build (sym10fix/instr) the REAL
#    RF/ADC path gets DATA_CLK=122.88 = 2x over the ISERDES closure -> mis-sample
#    ("e8" wrong-but-RS-valid frame).  61.44 real-RF only works on the clock-
#    decouple / FS_NO_DECIMATE build (system_top_direct87).  The DIGITAL BIST
#    loopback path (digital_loopback mode) is unaffected and decodes at 61.44.
#    -> for ota_rx/rf_loopback on a standard build use FS=30720000 (or 40M+FIR).
#    This script honors the requested FS and WARNS accordingly.
# =============================================================================

# ------------------------- arguments / defaults ------------------------------
MODE="${1:-digital_loopback}"
FS="${2:-61440000}"                     # 61.44 Msps default (all modes)
LO="${3:-2412000000}"                   # 2.412 GHz (ch1) default for OTA/RF
TXGAIN="${4:--30}"                      # -30 dB SAFE electronic attenuator (RF)
SELFTEST="${SELFTEST:-1}"               # digital_loopback boot self-test on
SELFTEST_N="${SELFTEST_N:-8}"           # self-test frame count
FIR_FILE="${FIR_FILE:-/root/fir.ftr}"   # 40 MHz-designed FIR (emission enabler)
RFBW="50000000"                         # 50 MHz channel bandwidth (OTA/RF)

# ------------------------- iio / debugfs bases -------------------------------
D=/sys/bus/iio/devices/iio:device0      # ad9361-phy sysfs
DBG=/sys/kernel/debug/iio/iio:device0   # ad9361-phy debugfs (loopback etc.)

# ------------------------- FPGA register map (devmem) ------------------------
#  tx_intf   base 0x83c00000 : dac_replay trigger  slv_reg7[3] @ 0x83c0001c
#  rx_intf   base 0x83c20000 : slv_reg3 0x..0c {[8]inject [9]trig [10]loop_en
#                                               [11]cnt_clr [15:12]rate [17:16]rb}
#                              slv_reg4 0x..10 [3]bb_20M_en
#                              slv_reg7 0x..1c [0]src_sel (0=ADC)
#                              slv_reg11 0x..2c [2:0]bb_gain (digital shift 0..6)
#                              LATCH   0x..7c {[31]RS-pass [30:24]bcnt [23:0]b0b1b2}
#  openofdm_rx base 0x83c30000: 0x..00 MULTI_RST, 0x..04 ENABLE, 0x..08 POWER_THRES,
#                              0x..0c MIN_PLATEAU, 0x..10 SOFT_DECODING(MAX_LEN),
#                              0x..14 FFT_WIN_SHIFT, 0x..48 PHASE_OFFSET
#  xpu       base 0x83c40000 : 0x..04 xpu_en, 0x..2c [4]ack_tx_disable
#  DAC core  0x79024418/0x79024458 datasel(=2 FPGA), 0x79024044 sync(=1)
# -----------------------------------------------------------------------------

LB_STATE="?"          # remembered (as-set) loopback state for the status block
FIR_STATE="skipped"   # remembered FIR action for the status block

log()  { echo "[autocfg] $*"; }
wr()   { devmem "$1" 32 "$2"; }                 # 32-bit devmem write
rd()   { cat "$1" 2>/dev/null; }                # safe plain-sysfs read
rdmem(){ devmem "$1"; }                          # devmem read (returns 0x....)

# =============================================================================
#  COMMON PREAMBLE  -- mounts, printk silence, wait-for-iio
# =============================================================================
common_preamble() {
    # init=/bin/sh does NOT auto-mount these.  proc first (for /proc/sys/printk).
    mount -t proc    proc  /proc              2>/dev/null || true
    mount -t sysfs   sysfs /sys               2>/dev/null || true
    mount -t debugfs none  /sys/kernel/debug  2>/dev/null || true

    # Silence the USB error-71 flood so the status block is readable on console.
    echo 0 > /proc/sys/kernel/printk 2>/dev/null || true

    # Wait (up to ~20 s) for the AD9361 driver to expose iio:device0 = ad9361-phy.
    log "waiting for iio:device0 (ad9361-phy) ..."
    i=0
    while [ $i -lt 40 ]; do
        if [ -e "$D/name" ]; then
            NM=$(rd "$D/name")
            if [ "$NM" = "ad9361-phy" ]; then
                log "iio:device0 = ad9361-phy  (up)"
                return 0
            fi
        fi
        usleep 500000
        i=$((i + 1))
    done

    echo ""
    echo "***************************************************************"
    echo "*  ERROR: iio:device0 (ad9361-phy) NOT present.               *"
    echo "*  The AD9361 did not probe -- almost always a soft reboot    *"
    echo "*  disrupting DATA_CLK -> 'Calibration TIMEOUT 0x244'.        *"
    echo "*  FIX: PHYSICALLY POWER-CYCLE the board (do NOT reboot -f),   *"
    echo "*  boot the sym10-fix/instr (85 MHz) bitstream, re-run this.   *"
    echo "***************************************************************"
    return 1
}

# =============================================================================
#  AD9361 COMMON  -- ensm re-cal + settled sample-rate ramp
# =============================================================================
ad9361_rate() {
    # ensm alert->fdd = clean re-cal into FDD (RX+TX LOs both live).
    echo alert > $D/ensm_mode 2>/dev/null || true
    echo fdd   > $D/ensm_mode 2>/dev/null || true

    # Settle at 30.72 FIRST, then step to the target (a direct 61.44 write can be
    # rejected on some driver states; the 30.72->target ramp is the proven order).
    echo 30720000 > $D/in_voltage_sampling_frequency  2>/dev/null || true
    echo 30720000 > $D/out_voltage_sampling_frequency 2>/dev/null || true
    echo "$FS" > $D/in_voltage_sampling_frequency  2>/dev/null || true
    echo "$FS" > $D/out_voltage_sampling_frequency 2>/dev/null || true
    sync
    log "Fs requested=$FS  readback(in)=$(rd $D/in_voltage_sampling_frequency)"
}

# =============================================================================
#  FPGA helper blocks (shared across modes)
# =============================================================================
# ============================================================================
#  2026-08-27 SAFETY FIX: TX hardwaregain was left at -10 dB.
#  Previously the line `echo "$TXGAIN" > out_voltage0_hardwaregain` was ONLY in
#  mode_rf_loopback(). The other modes (digital_loopback, ota_rx) never wrote it,
#  so it kept the driver default of -10 dB -> VIOLATING the -30 dB constraint.
#  Measured on silicon 2026-08-27: after digital_loopback, the gain read -10.000000 dB.
#  Now every mode calls enforce_tx_gain, with a hard CEILING: never above -30.
# ============================================================================
enforce_tx_gain() {
    _g="${1:-$TXGAIN}"
    # hard ceiling: a positive value, or anything above -30, is clamped to -30
    case "$_g" in
        -*) _mag="${_g#-}" ;;
        *)  _mag=0 ;;
    esac
    _int="${_mag%%.*}"
    [ -z "$_int" ] && _int=0
    if [ "$_int" -lt 30 ] 2>/dev/null; then
        log "TXGAIN $_g exceeds the safety ceiling -> forced to -30 dB"
        _g=-30
    fi
    for _f in $D/out_voltage0_hardwaregain $D/out_voltage1_hardwaregain; do
        [ -e "$_f" ] && echo "$_g" > "$_f" 2>/dev/null || true
    done
    sync
    log "TX hardwaregain = $(rd $D/out_voltage0_hardwaregain) dB"
}

dac_src_fpga() {
    # Drive the DAC from the FPGA dac_intf (dac_replay) instead of DMA/DDS.
    wr 0x79024418 2        # DAC chan0 datasel = FPGA input
    wr 0x79024458 2        # DAC chan1 datasel = FPGA input
    wr 0x79024044 1        # DAC sync (self-clearing)
    # 2026-08-26 REQUIRED: tx_intf slv_reg13[9:0] = TX-path bb_gain, resets to 0 => every sample x0.
    # This script never set it before, so after every restart the transmit path was mute.
    enforce_tx_gain        # 2026-08-27 REQUIRED: no mode may leave the gain > -30 dB
    wr 0x83c00034 0x80     # TX bb_gain = unity
    # 2026-08-26 LOAD SED PATTERN: 0x79024410/0x79024450 default to 0, so the SED test port (dac_data_sel=1)
    # reads 0 and looks EXACTLY like a dead TX port. That produced a wrong conclusion and a string of dead ends
    # on 08-26. Preload pattern 0x0780 (-> bw20 = 1920 with test bb_gain = 4) so the test port always reads correctly.
    wr 0x79024410 0x07800780   # DAC ch0 pattern (SED)
    wr 0x79024450 0x07800780   # DAC ch1 pattern (SED)
}

flush_rx_replay() {
    # rx_replay loop_en leaves its FSM active + its bw20 mux overrides the ADC
    # path and FREEZES dac_replay at a stale latch.  Single-shot flush -> active=0.
    wr 0x83c2000c 0x200
    usleep 300000
    wr 0x83c2000c 0x0
}

openofdm_rx_hwinit() {
    # FULL hw_init -- the decoder is IDLE (STATE_HISTORY=0) without ALL of these.
    wr 0x83c30004 1            # ENABLE
    wr 0x83c30008 0x0040007C   # POWER_THRES
    wr 0x83c3000c 0x64         # MIN_PLATEAU
    wr 0x83c30010 0x1942E001   # SOFT_DECODING (MAX_LEN=6466, mandatory for 6450 B)
    wr 0x83c30014 0x304        # FFT_WIN_SHIFT
    wr 0x83c30048 0xB          # PHASE_OFFSET_ABS_TH
}

xpu_init() {
    wr 0x83c40004 1            # xpu_en
    wr 0x83c4002c 0x10         # ack_tx_disable = 1 (protect shared FFT during RX)
}

multi_rst() {
    # MULTI_RST pulse arms/re-arms the detector (latch is STICKY -> re-arm/frame).
    wr 0x83c30000 1
    wr 0x83c30000 0
}

# Load the 40 MHz FIR ONLY when Fs==40 MHz (it forces Fs=40M and won't enable
# otherwise).  $1 = "tx" to also enable the TX (out) FIR (emission enabler).
maybe_load_fir() {
    if [ ! -f "$FIR_FILE" ]; then
        log "WARN: FIR file $FIR_FILE not found -> FIR skipped."
        FIR_STATE="missing($FIR_FILE)"
        return 0
    fi
    if [ "$FS" = "40000000" ]; then
        cat "$FIR_FILE" > $D/filter_fir_config 2>/dev/null
        sync ; usleep 500000
        echo 1 > $D/in_voltage_filter_fir_en  2>/dev/null || true
        if [ "$1" = "tx" ]; then
            echo 1 > $D/out_voltage_filter_fir_en 2>/dev/null || true
            FIR_STATE="loaded(rx+tx)@40M"
        else
            FIR_STATE="loaded(rx)@40M"
        fi
        log "FIR loaded from $FIR_FILE (Fs pinned to 40 MHz)."
    else
        FIR_STATE="skipped(Fs=$FS!=40M)"
        log "WARN: Fs=$FS != 40 MHz -> FIR NOT loaded (the .ftr is a 40 MHz"
        log "      design and would force Fs=40M).  Relying on the analog RF"
        log "      bandwidth ($RFBW) + AD9361 half-band filters instead."
        if [ "$1" = "tx" ]; then
            log "      NOTE(08-15): RF emission WITHOUT the TX FIR may give RS=0."
            log "      For a PROVEN RF-loopback emission run with FS=40000000."
        fi
    fi
}

# =============================================================================
#  MODE: digital_loopback  (AD9361 BIST DATA_PORT_LOOP -> dac_replay decode)
# =============================================================================
mode_digital_loopback() {
    log "MODE digital_loopback  (Fs=$FS, BIST DATA_PORT_LOOP)"
    ad9361_rate

    # BIST digital loopback: toggle 0->1 (reg 0x3F5 DATA_PORT_LOOP).  Re-issue is
    # deliberate: the first write can fail 'Bad file descriptor' before debugfs
    # settles.  We NEVER cat the debugfs attr back (a read can reboot the board).
    echo 0 > $DBG/loopback 2>/dev/null || true
    echo 1 > $DBG/loopback 2>/dev/null || true
    echo 1 > $DBG/loopback 2>/dev/null || true
    LB_STATE="1 (BIST digital loop, as-set)"

    dac_src_fpga                 # DAC driven by FPGA (dac_replay golden)
    flush_rx_replay              # clear any stale rx_replay FSM before dac_replay

    # rx_intf: real-ADC/loopback source path + bb_gain for the AD9361 loopback.
    wr 0x83c2000c 0x0            # slv_reg3 = 0 : replay off + inject off
    wr 0x83c20010 0x8            # slv_reg4[3] bb_20M_en
    wr 0x83c2001c 0x0            # slv_reg7[0] src_sel = ADC path
    wr 0x83c2002c 0x7            # slv_reg11[2:0] bb_gain = 7 (AD9361 loopback)

    openofdm_rx_hwinit
    xpu_init
    multi_rst

    log "digital_loopback configured.  DECODE-READY (trigger dac_replay to decode)."
}

# =============================================================================
#  MODE: ota_rx  (real over-the-air receive)
# =============================================================================
mode_ota_rx() {
    log "MODE ota_rx  (Fs=$FS, LO=$LO, loopback=0, real ADC)"
    ad9361_rate

    # REAL analog path: BIST loopback OFF.
    echo 0 > $DBG/loopback 2>/dev/null || true
    LB_STATE="0 (real analog RX, as-set)"

    # RF analog bandwidth = 50 MHz channel (AD9361 RX max ~56 MHz; driver may
    # quantize -- the status block echoes the accepted readback).
    echo "$RFBW" > $D/in_voltage_rf_bandwidth  2>/dev/null || true
    echo "$RFBW" > $D/out_voltage_rf_bandwidth 2>/dev/null || true
    sync

    # RX LO = link carrier.  MUST equal the far-end TX_LO (a split = "noise only").
    echo "$LO" > $D/out_altvoltage0_RX_LO_frequency 2>/dev/null || true
    echo "$LO" > $D/out_altvoltage1_TX_LO_frequency 2>/dev/null || true   # harmless for RX-only
    sync

    # RX gain: slow_attack AGC (AD9361-internal, matches dtb gc-rx1-mode).  The
    # AGC settles on the STF then holds -- must not step mid-packet (breaks DFE).
    echo slow_attack > $D/in_voltage0_gain_control_mode 2>/dev/null || true
    # manual-gain alternative (deterministic first bring-up / strong signal):
    #   echo manual > $D/in_voltage0_gain_control_mode ; echo 60 > $D/in_voltage0_hardwaregain

    # Track real RF impairments (DC offset + I/Q imbalance).
    echo 1 > $D/in_voltage_quadrature_tracking_en   2>/dev/null || true
    echo 1 > $D/in_voltage_rf_dc_offset_tracking_en 2>/dev/null || true
    echo 1 > $D/in_voltage_bb_dc_offset_tracking_en 2>/dev/null || true

    # RX FIR: only load when Fs==40 MHz (the shipped .ftr is a 40 MHz design).
    maybe_load_fir rx

    # FPGA: route the REAL ADC into openofdm_rx (no replay, no inject).
    wr 0x83c2000c 0x0            # slv_reg3 = 0 : replay off + inject off -> ADC
    wr 0x83c20010 0x8            # bb_20M_en
    wr 0x83c2001c 0x0            # src_sel = ADC path
    wr 0x83c2002c 0x4            # bb_gain = 4 (OTA start; TUNE on HW vs STF level)

    openofdm_rx_hwinit
    xpu_init
    multi_rst

    if [ "$FS" = "61440000" ]; then
        log "WARN: Fs=61.44 real-RF works ONLY on the clock-decouple/FS_NO_DECIMATE"
        log "      build.  On the standard LVDS build -> DATA_CLK 122.88 (2x) ->"
        log "      mis-sample (e8).  Use FS=30720000 (or 40000000+FIR) there."
    fi
    log "ota_rx configured.  Now transmit SIPDDM frames from a counterpart at LO=$LO."
    log "Metric = openofdm_rx RS-pass (latch bit31); first-3 is NOT b2 3a c9 over RF."
}

# =============================================================================
#  MODE: rf_loopback  (ota_rx + TX enabled, for an SMA-cable self-loopback)
# =============================================================================
mode_rf_loopback() {
    echo ""
    echo "###############################################################"
    echo "#  rf_loopback: TX IS ENABLED.  A DIRECT SMA CABLE WITH NO     #"
    echo "#  PHYSICAL ATTENUATOR CAN DAMAGE THE RX INPUT.                #"
    echo "#  TX hardwaregain defaults to -30 dB (electronic attenuator): #"
    echo "#  ~-23 dBm at RX, ~25 dB below the +2.5 dBm RX damage limit.   #"
    echo "#  Keep an inline attenuator on the cable regardless.          #"
    echo "###############################################################"
    log "MODE rf_loopback  (Fs=$FS, LO=$LO, TXgain=$TXGAIN dB, loopback=0)"
    ad9361_rate

    # REAL analog path: BIST loopback OFF.
    echo 0 > $DBG/loopback 2>/dev/null || true
    LB_STATE="0 (real analog TX+RX, as-set)"

    # RF analog bandwidth = 50 MHz on both RX and TX.
    echo "$RFBW" > $D/in_voltage_rf_bandwidth  2>/dev/null || true
    echo "$RFBW" > $D/out_voltage_rf_bandwidth 2>/dev/null || true
    sync

    # TX LO == RX LO, same real carrier (a split = RX cannot downconvert).
    echo "$LO" > $D/out_altvoltage0_RX_LO_frequency 2>/dev/null || true
    echo "$LO" > $D/out_altvoltage1_TX_LO_frequency 2>/dev/null || true
    sync

    # RX chain (same as ota_rx).
    echo slow_attack > $D/in_voltage0_gain_control_mode 2>/dev/null || true
    echo 1 > $D/in_voltage_quadrature_tracking_en   2>/dev/null || true
    echo 1 > $D/in_voltage_rf_dc_offset_tracking_en 2>/dev/null || true
    echo 1 > $D/in_voltage_bb_dc_offset_tracking_en 2>/dev/null || true

    # TX FIR = the RF EMISSION ENABLER (08-15: no FIR -> RS=0; with FIR -> RS=1).
    # Loaded ONLY at Fs=40 MHz; otherwise skipped with a loud warning.
    maybe_load_fir tx

    # TX power: SAFE electronic attenuator.  Pair with a physical attenuator.
    echo "$TXGAIN" > $D/out_voltage0_hardwaregain 2>/dev/null || true
    sync

    # DAC driven by the FPGA so a later dac_replay can emit through the RF chain.
    # (Self-loopback MUST use the dumb dac_replay TX, NOT openofdm_tx: openofdm_tx
    #  + openofdm_rx share one FFT and cannot run simultaneously.)
    dac_src_fpga
    flush_rx_replay

    # FPGA RX: real ADC into openofdm_rx.
    wr 0x83c2000c 0x0
    wr 0x83c20010 0x8
    wr 0x83c2001c 0x0
    wr 0x83c2002c 0x4            # bb_gain = 4 (tune vs the RX level over the cable)

    openofdm_rx_hwinit
    xpu_init
    multi_rst

    if [ "$FS" != "40000000" ]; then
        log "WARN: rf_loopback emission is PROVEN only at Fs=40M + TX FIR (08-15)."
        log "      At Fs=$FS the FIR is skipped -> emission may not RS-decode."
    fi
    log "rf_loopback configured.  Emit with a dac_replay trigger (0x83c0001c=0x8"
    log "-> settle -> 0x0), then read the latch 0x83c2007c (bit31 = RS-pass)."
    log "This script does NOT auto-emit (no RF on the wire at boot)."
}

# =============================================================================
#  OPTIONAL SELF-TEST  -- digital_loopback dac_replay decode loop (SAFE)
#    dac_replay drives the DUMB DAC path (not openofdm_tx) + reads the LATCH
#    (NO S2MM DMA) -> single FFT user -> FFT-safe.  PASS = latch[23:0]==0xC93AB2
#    (b2 3a c9) AND bit31==1 (RS-pass), i.e. latch == 0xB2C93AB2.
# =============================================================================
selftest_dac_replay() {
    log "self-test: $SELFTEST_N dac_replay frames ..."
    n=0 ; pass=0
    while [ $n -lt $SELFTEST_N ]; do
        wr 0x83c2000c 0x0            # ADC path
        multi_rst                    # per-frame re-arm (latch is sticky)
        wr 0x83c0001c 0x8            # dac_replay trigger ON (tx_intf slv_reg7[3])
        usleep 100000
        wr 0x83c0001c 0x0            # trigger OFF
        L=$(rdmem 0x83c2007c)
        LV=$(printf "%d" "$L" 2>/dev/null)
        DATA=$(( LV & 0xFFFFFF ))
        FCS=$(( (LV >> 31) & 1 ))
        OK="mismatch"
        if [ "$DATA" -eq $((0xC93AB2)) ] && [ "$FCS" -eq 1 ]; then
            OK="b2 3a c9 RS-PASS" ; pass=$((pass + 1))
        fi
        log "  frame $n latch=$L  first3=$(printf 0x%06X $DATA) fcs=$FCS  $OK"
        n=$((n + 1))
    done
    log "self-test: $pass / $SELFTEST_N frames b2 3a c9 + RS-pass"
    SELFTEST_RESULT="$pass/$SELFTEST_N"
}

# =============================================================================
#  STATUS / VERIFICATION block  -- echo the key readback values
#    (plain sysfs + devmem only; NEVER cat the debugfs loopback attr)
# =============================================================================
print_status() {
    echo ""
    echo "================= ad9361_autoconfig STATUS ====================="
    echo " mode              : $MODE"
    echo " --- AD9361 (iio sysfs readback) ---"
    echo " Fs in  (Hz)       : $(rd $D/in_voltage_sampling_frequency)"
    echo " Fs out (Hz)       : $(rd $D/out_voltage_sampling_frequency)"
    echo " ensm_mode         : $(rd $D/ensm_mode)"
    echo " loopback (as-set) : $LB_STATE"
    if [ "$MODE" != "digital_loopback" ]; then
        echo " RX RF bw (Hz)     : $(rd $D/in_voltage_rf_bandwidth)"
        echo " TX RF bw (Hz)     : $(rd $D/out_voltage_rf_bandwidth)"
        echo " RX_LO (Hz)        : $(rd $D/out_altvoltage0_RX_LO_frequency)"
        echo " TX_LO (Hz)        : $(rd $D/out_altvoltage1_TX_LO_frequency)"
        echo " RX gain mode      : $(rd $D/in_voltage0_gain_control_mode)"
        echo " RX FIR en         : $(rd $D/in_voltage_filter_fir_en)"
        echo " TX FIR en         : $(rd $D/out_voltage_filter_fir_en)"
        echo " FIR action        : $FIR_STATE"
    fi
    if [ "$MODE" = "rf_loopback" ]; then
        echo " TX hardwaregain   : $(rd $D/out_voltage0_hardwaregain) (dB)"
    fi
    echo " --- FPGA registers (devmem readback) ---"
    echo " rx_intf slv_reg3  : $(rdmem 0x83c2000c)   (0=ADC path)"
    echo " rx_intf bb_20M    : $(rdmem 0x83c20010)   (0x8)"
    echo " rx_intf src_sel   : $(rdmem 0x83c2001c)   (0x0=ADC)"
    echo " rx_intf bb_gain   : $(rdmem 0x83c2002c)"
    echo " openofdm ENABLE   : $(rdmem 0x83c30004)   (1)"
    echo " openofdm POWER_TH : $(rdmem 0x83c30008)   (0x0040007C)"
    echo " openofdm SOFT_DEC : $(rdmem 0x83c30010)   (0x1942E001 MAX_LEN=6466)"
    echo " openofdm FFT_WIN  : $(rdmem 0x83c30014)   (0x304)"
    echo " openofdm PHASE    : $(rdmem 0x83c30048)   (0xB)"
    echo " xpu_en            : $(rdmem 0x83c40004)   (1)"
    echo " xpu ack_tx_dis    : $(rdmem 0x83c4002c)   (0x10)"
    echo " rx latch 0x..207c : $(rdmem 0x83c2007c)   ({[31]RS [30:24]bcnt [23:0]b0b1b2})"
    if [ -n "$SELFTEST_RESULT" ]; then
        echo " self-test         : $SELFTEST_RESULT  (b2 3a c9 + RS-pass)"
    fi
    echo "================================================================"
    echo "DECODE-READY (mode=$MODE, Fs=$FS)"
}

# =============================================================================
#  MAIN
# =============================================================================
SELFTEST_RESULT=""

common_preamble || exit 1

case "$MODE" in
    digital_loopback)
        mode_digital_loopback
        if [ "$SELFTEST" = "1" ]; then
            selftest_dac_replay
        fi
        ;;
    ota_rx)
        mode_ota_rx
        ;;
    rf_loopback)
        mode_rf_loopback
        ;;
    *)
        echo "[autocfg] ERROR: unknown MODE '$MODE'."
        echo "          use: digital_loopback (default) | ota_rx | rf_loopback"
        exit 2
        ;;
esac

# 2026-08-27 FINAL SAFETY LATCH: run after EVERY mode, including mode_ota_rx
# (that mode does not call dac_src_fpga, so it would slip through if the clamp were only there).
enforce_tx_gain

print_status
exit 0
