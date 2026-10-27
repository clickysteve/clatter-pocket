#!/usr/bin/env bash
# Footprint and soft-float estimate of the FM-1 engine configuration, without the FM-1
# toolchain.
#
# Cross-compiles exactly the sources the FM-1 build links (LXR without the FX bus,
# engine/fx_off.c, the panel), with the FM-1 build's configuration header, for a stand-in 32-bit target with a single-precision FPU (Cortex-M4F) with
# upstream LLVM, and reports:
#   - bytes per section, grouped as the FM-1 linker script will place them
#     (.itcm_text and .dtcm/.dtcmz -> internal SRAM; .text/.rodata -> Flash unless moved);
#   - every double-precision or other software floating-point helper the code calls, which on
#     a single-precision FPU runs in software (slow) - a CPU risk on the FM-1 as well.
# The PI32v2 instruction set differs, so sizes are an estimate (code size within tens of
# percent; data and tables exact, as they do not depend on the instruction set).
#
#   tools/size-estimate.sh            uses $LLVM_BIN or /usr/local/opt/llvm/bin, llvm@21
#   SP=1 tools/size-estimate.sh       with engine/single_precision.h (double computed as float)
set -euo pipefail
cd "$(dirname "$0")/.."
LLVM_BIN=${LLVM_BIN:-}
if [[ -z "$LLVM_BIN" ]]; then
	for d in /usr/local/opt/llvm/bin /usr/local/opt/llvm@21/bin /opt/homebrew/opt/llvm/bin /usr/lib/llvm-21/bin /usr/lib/llvm-18/bin; do
		[[ -x "$d/clang" ]] && LLVM_BIN=$d && break
	done
fi
[[ -x "$LLVM_BIN/clang" ]] || { echo "set LLVM_BIN to an LLVM with the ARM target" >&2; exit 1; }
out=build/size-estimate
rm -rf "$out"; mkdir -p "$out"
sources=($(find lxr/upstream/Core/Src -name '*.c' ! -name MidiParser.c ! -path '*/FX/*' ! -name FxBus.c | sort)
	lxr/bridge/lxr_params.c lxr/bridge/libm.c engine/pocket_lxr.c engine/fx_off.c ui/pocket_panel.c)
for f in "${sources[@]}"; do
	sp=()
	[[ -n "${SP:-}" && "$f" != */libm.c ]] && sp=(-include engine/single_precision.h)
	"$LLVM_BIN/clang" "${sp[@]+"${sp[@]}"}" --target=armv7em-none-eabihf -mcpu=cortex-m4 -mfloat-abi=hard -mfpu=fpv4-sp-d16 \
		-O2 -ffast-math -freciprocal-math -cl-single-precision-constant -ffreestanding -fno-builtin \
		-ffunction-sections -fdata-sections -w \
		-include firmware/fm1/pocket_fm1_config.h \
		-Ilxr/shim -Ilxr/shim/libc -Ilxr/upstream/Core/Inc -Iengine -Iui \
		'-DPOCKET_BARRIER()=__asm__ volatile("" ::: "memory")' \
		-c "$f" -o "$out/$(echo "$f" | tr / _).o"
done
echo "== bytes by section (${#sources[@]} files; Cortex-M4F stand-in, -O2)"
"$LLVM_BIN/llvm-size" -A "$out"/*.o | awk '
	/^\./ { n=$1; sub(/^\./,"",n); split(n,a,"."); k=a[1]
		if (k=="text"||k=="itcm_text"||k=="rodata"||k=="data"||k=="bss"||k=="dtcm"||k=="dtcmz") s[k]+=$2 }
	END { for (k in s) printf "  %-10s %8d\n", k, s[k] }' | sort
echo "== largest objects (DTCM/rodata/bss)"
for o in "$out"/*.o; do
	"$LLVM_BIN/llvm-nm" -S --radix=d --size-sort "$o" 2>/dev/null | awk -v f="$(basename "$o" .o)" '{ sz=$2+0; if (sz>=1024) printf "  %8d %s %s (%s)\n", sz, $3, $4, f }'
done | sort -rn | head -15
echo "== software floating-point helpers called (double precision on a single-precision FPU)"
"$LLVM_BIN/llvm-nm" -u "$out"/*.o 2>/dev/null | awk '/__aeabi_[dfl]|__aeabi_[iu]2d|__(add|sub|mul|div)df|__extend|__trunc|__fix|__float/ { print $2 }' | sort | uniq -c | sort -rn
echo "== which files call them"
for o in "$out"/*.o; do
	h=$("$LLVM_BIN/llvm-nm" -u "$o" | awk '/__aeabi_d|__aeabi_[iu]2d|__aeabi_f2d|__aeabi_d2/ {print $2}' | sort -u | tr '\n' ' ')
	[[ -n "$h" ]] && echo "  $(basename "$o" .o): $h"
done || true
