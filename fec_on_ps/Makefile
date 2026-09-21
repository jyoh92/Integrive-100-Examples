# =============================================================================
# Integrive-100 — FEC decoder on the PS, every other block in the PL
#
#   make CROSS=arm-linux-gnueabi-     build for the board   <-- what you want
#   make                              host build (compile check + RS self-test)
#
# ★ The board's rootfs is SOFT-FLOAT — its loader is /lib/ld-linux.so.3.
#   A hard-float toolchain (arm-linux-gnueabihf-) builds a binary that will not
#   load there, and the error does not say why.  Use the one WITHOUT "hf".
#   Check what you built:   file build/fec_on_ps
# =============================================================================
CROSS   ?=
CC       = $(CROSS)gcc
AR       = $(CROSS)ar
CFLAGS  ?= -O2 -std=c99 -Wall -Wextra -Iinclude
LDLIBS   = -lm -lpthread
BUILD_ROOT = build
B        = $(if $(CROSS),$(BUILD_ROOT),$(BUILD_ROOT)/host)
AUTO_INSTALL ?= 1
TOOLCHAIN_STAMP = $(B)/.toolchain-$(if $(CROSS),$(CROSS),host)

LIBOBJ = $(B)/integrive.o $(B)/integrive_hw.o

all: check-deps $(B)/fec_on_ps $(B)/ps_hello $(B)/rs_selftest $(B)/integrive-cli
ifeq ($(CROSS),)
	$(B)/rs_selftest
endif

check-deps:
	@AUTO_INSTALL=$(AUTO_INSTALL) sh scripts/check-build-deps.sh "$(CC)" "$(AR)" "$(CROSS)"

$(B):
	@mkdir -p $(B)

# Rebuild old artifacts if a different toolchain previously used this directory.
$(TOOLCHAIN_STAMP): | check-deps $(B)
	@rm -f $(B)/.toolchain-*
	@touch $@

$(B)/libintegrive.a: $(LIBOBJ) $(TOOLCHAIN_STAMP)
	$(AR) rcs $@ $(LIBOBJ)

$(B)/%.o: src/%.c $(TOOLCHAIN_STAMP)
	$(CC) $(CFLAGS) -c -o $@ $<

# The decoder, built WITHOUT its software self-test main().
$(B)/ps_rs_decode.o: examples/ps_rs_decode.c $(TOOLCHAIN_STAMP)
	$(CC) $(CFLAGS) -DPS_RS_NO_MAIN -c -o $@ $<

# THE example: FEC on the ARM, everything else in fabric.
$(B)/fec_on_ps: examples/fec_on_ps.c $(B)/ps_rs_decode.o $(B)/libintegrive.a $(TOOLCHAIN_STAMP)
	$(CC) $(CFLAGS) -o $@ $< $(B)/ps_rs_decode.o $(B)/libintegrive.a $(LDLIBS)

# Command-line front end: `run` exercises the real RF/DAC path (cable-dependent).
$(B)/integrive-cli: examples/integrive-cli.c $(B)/ps_rs_decode.o $(B)/libintegrive.a $(TOOLCHAIN_STAMP)
	$(CC) $(CFLAGS) -o $@ $< $(B)/ps_rs_decode.o $(B)/libintegrive.a $(LDLIBS)

# Minimal check that the bitstream has a tap at all, before blaming the decoder.
$(B)/ps_hello: examples/ps_hello.c $(B)/libintegrive.a $(TOOLCHAIN_STAMP)
	$(CC) $(CFLAGS) -o $@ $< $(B)/libintegrive.a $(LDLIBS)

# The decoder on its own: encode, inject errors, decode, verify.  No hardware.
$(B)/rs_selftest: examples/ps_rs_decode.c $(B)/libintegrive.a $(TOOLCHAIN_STAMP)
	$(CC) $(CFLAGS) -o $@ $< $(B)/libintegrive.a $(LDLIBS)

clean:
	rm -rf $(BUILD_ROOT)

.PHONY: all check-deps clean
