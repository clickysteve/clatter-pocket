# CLATTER Pocket - host build (the portable engine, its tests and tools).
#
#   make            build the host tools and run the tests
#   make test       unit tests of the engine (the real LXR DSP, natively compiled)
#   make bench      host timing of the engine per FM-1 audio block
#   make render     render demo WAVs into build/host/
#   make compare    compare with CLATTER's WebAssembly engine (needs node and CLATTER_DIR)
#   make firmware   FM-1 firmware (see BUILDING.md; needs the JieLi toolchain)
#   make packed-tables  regenerate engine/lxr_tables_packed.c from the firmware's tables
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
# The FM-1 configuration's tables: packed in Flash, unpacked into RAM at start-up.
LXR_TABLES := lxr/upstream/Core/Src/Soundengine/DSPAudio/wavetable.c lxr/upstream/Core/Src/Soundengine/DSPAudio/transientTables.c
PACKED_SRC := $(filter-out $(LXR_TABLES),$(LXR_NOFX)) $(BRIDGE) $(ENGINE) engine/fx_off.c engine/lxr_tables_packed.c engine/lxr_tables_unpack.c
PACKED_OBJ := $(call obj,packed,$(PACKED_SRC))
$(PACKED_OBJ): DSP_CFLAGS += -DPOCKET_PACKED_TABLES

TOOLS := $(BUILD)/test_engine $(BUILD)/test_engine_nofx $(BUILD)/test_engine_packed $(BUILD)/test_panel $(BUILD)/test_midi $(BUILD)/test_fm1_libm $(BUILD)/bench $(BUILD)/render $(BUILD)/dump

.PHONY: all test bench render compare compare-sp firmware packed-tables clean
all: $(TOOLS) test

# Single-precision variant (make compare-sp): the DSP's double-precision envelope and
# smoothing arithmetic computed in float, as a fallback for an FPU without double precision.
# libm.c relies on the double bit layout and stays as it is (and is not on a per-sample path).
SP_OBJ := $(call obj,sp,$(LXR_SRC) $(BRIDGE) $(ENGINE) engine/fx_on.c)
$(filter-out %/libm.o,$(SP_OBJ)): DSP_CFLAGS += -include engine/single_precision.h -cl-single-precision-constant

$(BUILD)/obj/full/lxr/bridge/libm.o $(BUILD)/obj/nofx/lxr/bridge/libm.o $(BUILD)/obj/sp/lxr/bridge/libm.o $(BUILD)/obj/packed/lxr/bridge/libm.o: DSP_CFLAGS += $(LIBM_RENAMES)

# One rule per variant: a pattern rule with several targets would be one recipe making all
# of them, so a clean build would skip the other variants' objects.
define variant_rule
$(BUILD)/obj/$(1)/%.o: %.c
	@mkdir -p $$(dir $$@)
	$$(CC) $$(DSP_CFLAGS) -c $$< -o $$@
endef
$(foreach v,full nofx sp packed,$(eval $(call variant_rule,$(v))))

$(BUILD)/liblxr_full.a: $(FULL_OBJ)
	@rm -f $@; ar rcs $@ $^
$(BUILD)/liblxr_nofx.a: $(NOFX_OBJ)
	@rm -f $@; ar rcs $@ $^
$(BUILD)/liblxr_sp.a: $(SP_OBJ)
	@rm -f $@; ar rcs $@ $^
$(BUILD)/liblxr_packed.a: $(PACKED_OBJ)
	@rm -f $@; ar rcs $@ $^

$(BUILD)/test_engine: host/tests/test_engine.c host/wav.h $(BUILD)/liblxr_full.a
	$(CC) $(HOST_CFLAGS) $< $(BUILD)/liblxr_full.a -lm -o $@
$(BUILD)/test_engine_nofx: host/tests/test_engine.c host/wav.h $(BUILD)/liblxr_nofx.a
	$(CC) $(HOST_CFLAGS) -DPOCKET_NOFX $< $(BUILD)/liblxr_nofx.a -lm -o $@
$(BUILD)/test_engine_packed: host/tests/test_engine.c host/wav.h $(BUILD)/liblxr_packed.a
	$(CC) $(HOST_CFLAGS) -DPOCKET_NOFX $< $(BUILD)/liblxr_packed.a -lm -o $@
# The firmware's tables as compiled, and the same tables after packing and unpacking.
$(BUILD)/dump_tables: host/dump_tables.c $(call obj,nofx,$(LXR_TABLES))
	$(CC) $(HOST_CFLAGS) $^ -o $@
$(BUILD)/dump_tables_packed: host/dump_tables.c $(BUILD)/liblxr_packed.a
	$(CC) $(HOST_CFLAGS) -DPOCKET_PACKED_TABLES $< $(BUILD)/liblxr_packed.a -lm -o $@
$(BUILD)/lxr_tables.bin: $(BUILD)/dump_tables
	$(BUILD)/dump_tables $@
packed-tables: $(BUILD)/lxr_tables.bin
	python3 tools/pack-lxr-tables.py $< engine/lxr_tables_packed.c
$(BUILD)/test_panel: host/tests/test_panel.c ui/pocket_panel.c ui/pocket_panel.h $(BUILD)/liblxr_nofx.a
	$(CC) $(HOST_CFLAGS) $< ui/pocket_panel.c $(BUILD)/liblxr_nofx.a -lm -o $@
# MIDI input, with AddressSanitizer: a read past a message fails the test.
$(BUILD)/test_midi: host/tests/test_midi.c ui/pocket_midi.c ui/pocket_midi.h
	$(CC) $(HOST_CFLAGS) -fsanitize=address -fno-omit-frame-pointer $< ui/pocket_midi.c -o $@
# The FM-1 build's own sqrtf and fmaf (the PI32v2 FPU and the SDK's libc have neither).
$(BUILD)/test_fm1_libm: host/tests/test_fm1_libm.c firmware/fm1/pocket_fm1_libm.c
	$(CC) $(HOST_CFLAGS) -ffp-contract=off $< -lm -o $@
$(BUILD)/bench: host/bench.c $(BUILD)/liblxr_nofx.a
	$(CC) $(HOST_CFLAGS) $< $(BUILD)/liblxr_nofx.a -lm -o $@
$(BUILD)/render: host/render.c host/wav.h $(BUILD)/liblxr_nofx.a
	$(CC) $(HOST_CFLAGS) $< $(BUILD)/liblxr_nofx.a -lm -o $@
$(BUILD)/dump: host/compare/dump.c $(BUILD)/liblxr_full.a
	$(CC) $(HOST_CFLAGS) $< $(BUILD)/liblxr_full.a -lm -o $@
$(BUILD)/dump_nofx: host/compare/dump.c $(BUILD)/liblxr_nofx.a
	$(CC) $(HOST_CFLAGS) $< $(BUILD)/liblxr_nofx.a -lm -o $@
$(BUILD)/dump_packed: host/compare/dump.c $(BUILD)/liblxr_packed.a
	$(CC) $(HOST_CFLAGS) $< $(BUILD)/liblxr_packed.a -lm -o $@
$(BUILD)/dump_sp: host/compare/dump.c $(BUILD)/liblxr_sp.a
	$(CC) $(HOST_CFLAGS) $< $(BUILD)/liblxr_sp.a -lm -o $@

test: $(BUILD)/test_engine $(BUILD)/test_engine_nofx $(BUILD)/test_engine_packed $(BUILD)/test_panel $(BUILD)/test_midi $(BUILD)/test_fm1_libm $(BUILD)/lxr_tables.bin $(BUILD)/dump_tables_packed $(BUILD)/dump_nofx $(BUILD)/dump_packed
	$(BUILD)/test_engine
	$(BUILD)/test_engine_nofx
	$(BUILD)/test_engine_packed
	$(BUILD)/dump_tables_packed $(BUILD)/lxr_tables_unpacked.bin
	cmp $(BUILD)/lxr_tables.bin $(BUILD)/lxr_tables_unpacked.bin && echo "packed tables: unpack byte-identical to the firmware's tables"
	$(BUILD)/dump_nofx > $(BUILD)/voices_nofx.f32
	$(BUILD)/dump_packed > $(BUILD)/voices_packed.f32
	cmp $(BUILD)/voices_nofx.f32 $(BUILD)/voices_packed.f32 && echo "packed tables: comparison script renders bit-identical voices"
	$(BUILD)/test_panel $(BUILD)/panel.ppm
	$(BUILD)/test_midi
	$(BUILD)/test_fm1_libm

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
