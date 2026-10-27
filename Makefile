# CLATTER Pocket - host build (the portable engine, its tests and tools).
#
#   make            build the host tools and run the tests
#   make test       unit tests of the engine (the real LXR DSP, natively compiled)
#   make bench      host timing of the engine per FM-1 audio block
#   make render     render demo WAVs into build/host/
#   make compare    compare with CLATTER's WebAssembly engine (needs node and CLATTER_DIR)
#   make firmware   FM-1 firmware (see BUILDING.md; needs the JieLi toolchain)
#
# The LXR sources are compiled as CLATTER compiles them for WebAssembly (scripts/build-lxr.mjs
# in CLATTER): CLATTER's freestanding shim and libm, -O3 -ffast-math -freciprocal-math, so the
# host output can be compared with CLATTER's numerically. FMA contraction is off because
# WebAssembly has none.

CC ?= cc
BUILD := build/host
CLATTER_DIR ?= ../clatter

LXR_SRC := $(shell find lxr/upstream/Core/Src -name '*.c' ! -name MidiParser.c | sort)
LXR_FX := lxr/upstream/Core/Src/Soundengine/FxBus.c $(wildcard lxr/upstream/Core/Src/Soundengine/DSPAudio/FX/*.c)
LXR_NOFX := $(filter-out $(LXR_FX),$(LXR_SRC))
BRIDGE := lxr/bridge/lxr_params.c lxr/bridge/libm.c
ENGINE := engine/pocket_lxr.c

DSP_CFLAGS := -O3 -ffast-math -freciprocal-math -ffp-contract=off -ffreestanding -fno-builtin -w \
	-Ilxr/shim -Ilxr/shim/libc -Ilxr/upstream/Core/Inc -Iengine
# libm.c also defines the C library's mem* and the half-float conversions for the freestanding
# WebAssembly build. Natively the platform provides those; rename CLATTER's out of the way.
LIBM_RENAMES := -Dmemset=lxr_unused_memset -Dmemcpy=lxr_unused_memcpy -Dmemmove=lxr_unused_memmove \
	-D__extendhfsf2=lxr_unused_extendhfsf2 -D__truncsfhf2=lxr_unused_truncsfhf2
HOST_CFLAGS := -O2 -Wall -Wextra -Wno-unused-function -std=c11 -Iengine -Iui -Ihost
# The firmware places its hot code and state in the STM32's tightly coupled memories with
# section attributes (config.h: IN_ITCM_RAM, INDTCM, INDTCMZ). Mach-O rejects ELF section
# names; on macOS turn them into inert annotations. (The FM-1 linker script maps the sections
# to internal SRAM; ELF hosts keep them as they are.)
ifeq ($(shell uname -s),Darwin)
DSP_CFLAGS += -Dsection=annotate
endif

obj = $(patsubst %.c,$(BUILD)/obj/$(1)/%.o,$(2))

FULL_OBJ := $(call obj,full,$(LXR_SRC) $(BRIDGE) $(ENGINE) engine/fx_on.c)
NOFX_OBJ := $(call obj,nofx,$(LXR_NOFX) $(BRIDGE) $(ENGINE) engine/fx_off.c)

TOOLS := $(BUILD)/test_engine $(BUILD)/test_engine_nofx $(BUILD)/test_panel $(BUILD)/bench $(BUILD)/render $(BUILD)/dump

.PHONY: all test bench render compare compare-sp firmware clean
all: $(TOOLS) test

# Single-precision variant (make compare-sp): the DSP's double-precision envelope and
# smoothing arithmetic computed in float, as a fallback for an FPU without double precision.
# libm.c relies on the double bit layout and stays as it is (and is not on a per-sample path).
SP_OBJ := $(call obj,sp,$(LXR_SRC) $(BRIDGE) $(ENGINE) engine/fx_on.c)
$(filter-out %/libm.o,$(SP_OBJ)): DSP_CFLAGS += -include engine/single_precision.h -cl-single-precision-constant

$(BUILD)/obj/full/lxr/bridge/libm.o $(BUILD)/obj/nofx/lxr/bridge/libm.o $(BUILD)/obj/sp/lxr/bridge/libm.o: DSP_CFLAGS += $(LIBM_RENAMES)

$(BUILD)/obj/full/%.o $(BUILD)/obj/nofx/%.o $(BUILD)/obj/sp/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(DSP_CFLAGS) -c $< -o $@

$(BUILD)/liblxr_full.a: $(FULL_OBJ)
	@rm -f $@; ar rcs $@ $^
$(BUILD)/liblxr_nofx.a: $(NOFX_OBJ)
	@rm -f $@; ar rcs $@ $^
$(BUILD)/liblxr_sp.a: $(SP_OBJ)
	@rm -f $@; ar rcs $@ $^

$(BUILD)/test_engine: host/tests/test_engine.c host/wav.h $(BUILD)/liblxr_full.a
	$(CC) $(HOST_CFLAGS) $< $(BUILD)/liblxr_full.a -lm -o $@
$(BUILD)/test_engine_nofx: host/tests/test_engine.c host/wav.h $(BUILD)/liblxr_nofx.a
	$(CC) $(HOST_CFLAGS) -DPOCKET_NOFX $< $(BUILD)/liblxr_nofx.a -lm -o $@
$(BUILD)/test_panel: host/tests/test_panel.c ui/pocket_panel.c ui/pocket_panel.h $(BUILD)/liblxr_nofx.a
	$(CC) $(HOST_CFLAGS) $< ui/pocket_panel.c $(BUILD)/liblxr_nofx.a -lm -o $@
$(BUILD)/bench: host/bench.c $(BUILD)/liblxr_nofx.a
	$(CC) $(HOST_CFLAGS) $< $(BUILD)/liblxr_nofx.a -lm -o $@
$(BUILD)/render: host/render.c host/wav.h $(BUILD)/liblxr_nofx.a
	$(CC) $(HOST_CFLAGS) $< $(BUILD)/liblxr_nofx.a -lm -o $@
$(BUILD)/dump: host/compare/dump.c $(BUILD)/liblxr_full.a
	$(CC) $(HOST_CFLAGS) $< $(BUILD)/liblxr_full.a -lm -o $@
$(BUILD)/dump_sp: host/compare/dump.c $(BUILD)/liblxr_sp.a
	$(CC) $(HOST_CFLAGS) $< $(BUILD)/liblxr_sp.a -lm -o $@

test: $(BUILD)/test_engine $(BUILD)/test_engine_nofx $(BUILD)/test_panel
	$(BUILD)/test_engine
	$(BUILD)/test_engine_nofx
	$(BUILD)/test_panel $(BUILD)/panel.ppm

bench: $(BUILD)/bench
	$(BUILD)/bench

render: $(BUILD)/render
	$(BUILD)/render $(BUILD)

compare: $(BUILD)/dump
	$(BUILD)/dump > $(BUILD)/pocket_dump.bin
	node host/compare/compare_clatter.mjs $(CLATTER_DIR)/src/audio/lxr/lxr.wasm $(BUILD)/pocket_dump.bin

compare-sp: $(BUILD)/dump_sp
	$(BUILD)/dump_sp > $(BUILD)/pocket_dump_sp.bin
	node host/compare/compare_clatter.mjs $(CLATTER_DIR)/src/audio/lxr/lxr.wasm $(BUILD)/pocket_dump_sp.bin

firmware:
	firmware/fm1/build.sh

clean:
	rm -rf build
