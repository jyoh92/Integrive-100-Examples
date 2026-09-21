#!/bin/sh
# =============================================================================
#  khuanh.sh -- apply the receiver's IQ image-rejection correction before a run.
#
#  A cabled TX->RX loop sees the AD9361 mixer's image. Left uncorrected it sits
#  ~30 dB below the signal on this board; correcting it clears that. This writes
#  the correction matrix into the AD9361's IQCOR registers and freezes the chip's
#  own quadrature tracking so it cannot drift on top of the fixed coefficients.
#
#  ! THESE COEFFICIENTS ARE THIS BOARD'S, MEASURED. Read back 2026-09-17:
#      0x79020414 = 0x3FFE0229    0x79020454 = 0x02254002
#    They decode to b ~ +0.00012 - 0.0336j (a raw image of ~29.5 dB IRR),
#    i.e. mostly a baseband-path imbalance. Per the image-rejection procedure the
#    value drifts with the chip's internal QEC state across a power cycle, `initialize`,
#    or re-enabling quadrature tracking -- so treat this as a good DEFAULT for
#    this board, and re-derive with tools_board/refine2.py (or blind.py) if the
#    image is ever shown to limit a measurement. Changing only the LO does NOT
#    need a re-derive.
#
#  Register format (ADI ad_iqcor, Q1.14, 0x4000 = 1.0), each reg = {mult_I, mult_Q}:
#      0x79020414 = { (1 - b_re)*16384 , (-b_im)*16384 }
#      0x79020454 = { (-b_im)*16384    , (1 + b_re)*16384 }
#    IQCOR_ENB is bit 9 of 0x79020400 / 0x79020440 (the autoconfig leaves it set).
# =============================================================================
IIO=/sys/bus/iio/devices/iio:device0

# 1. Freeze the chip's quadrature tracking FIRST, so it does not keep adjusting
#    underneath the fixed correction (image-rejection procedure, 4.2). Only this
#    attribute -- writing 0 to bb_dc_offset_tracking_en instead crashes the chip.
echo 0 > $IIO/in_voltage_quadrature_tracking_en 2>/dev/null

# 2. Load the correction matrix.
devmem 0x79020414 32 0x3FFE0229
devmem 0x79020454 32 0x02254002

echo "image rejection: 0x79020414=$(devmem 0x79020414) 0x79020454=$(devmem 0x79020454)" \
     "enb=$(devmem 0x79020400) qtrack=$(cat $IIO/in_voltage_quadrature_tracking_en 2>/dev/null)"
