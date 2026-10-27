#!/usr/bin/env bash
# Checks the prepared FM-1 tree without the JieLi toolchain (run firmware/fm1/build.sh
# --prepare-only first):
#   1. every SDK file matches sdk/manifest.json and no file was added (the first thing
#      Build-ClatterPocket.ps1 checks; the SDK must stay pristine);
#   2. the patched application and the CLATTER Pocket sources compile for a stand-in 32-bit
#      target (upstream clang, ARMv7E-M with a single-precision FPU) with the include
#      layout, configuration header and floating-point flags of the FM-1 build.
# Passing says the sources are consistent with the SDK's interfaces. It does not replace
# the real build: PI32v2 code generation, the link, the SRAM budget and the firmware checks
# happen only with the JieLi toolchain.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
tree="$repo/build/fm1/tree"
[[ -d "$tree/pocket" ]] || { echo "run firmware/fm1/build.sh --prepare-only first" >&2; exit 1; }

echo "== SDK manifest"
python3 - "$tree" <<'EOF'
import hashlib, json, pathlib, sys
root = pathlib.Path(sys.argv[1])
manifest = json.loads((root / 'sdk/manifest.json').read_text(encoding='utf-8-sig'))
bad = [n for n, h in manifest['files'].items() if hashlib.sha256((root / n).read_bytes()).hexdigest() != h]
extra = [str(p.relative_to(root)) for p in (root / 'sdk').rglob('*') if p.is_file()
         and str(p.relative_to(root)) not in manifest['files'] and p.name != 'manifest.json']
print(f"   {len(manifest['files'])} SDK files, {len(bad)} changed, {len(extra)} unexpected; toolchain: {manifest['toolchain']}")
if bad or extra:
    print('   changed:', bad, 'unexpected:', extra); sys.exit(1)
EOF

LLVM_BIN=${LLVM_BIN:-}
for d in "$LLVM_BIN" /usr/local/opt/llvm/bin /usr/local/opt/llvm@21/bin /opt/homebrew/opt/llvm/bin /usr/lib/llvm-21/bin /usr/lib/llvm-18/bin; do
	[[ -n "$d" && -x "$d/clang" ]] && LLVM_BIN=$d && break
done
cc=("$LLVM_BIN/clang" --target=armv7em-none-eabihf -mcpu=cortex-m4 -mfloat-abi=hard -mfpu=fpv4-sp-d16
	-ffreestanding -cl-single-precision-constant -fms-extensions -O2 -c -o /dev/null)
sdk_inc=(-I"$tree/sdk/include" -I"$tree/sdk/include/libc" -I"$tree/app/synth" -I"$tree/sdk/config"
	-I"$tree/app" -I"$tree/app/board" -I"$tree/pocket/engine" -I"$tree/pocket/ui")
pocket_inc=(-I"$tree/pocket/lxr/shim" -I"$tree/pocket/lxr/shim/libc" -I"$tree/pocket/lxr/upstream/Core/Inc"
	-I"$tree/pocket/engine" -I"$tree/pocket/ui" -I"$tree/sdk/include/libc")
failed=0
echo "== application files touched by CLATTER Pocket (SDK headers, -fsyntax-only)"
for f in app/board/audio.c app/screen.c app/clatter_pocket/clatter_pocket_fm1.c; do
	"$LLVM_BIN/clang" --target=armv7em-none-eabi -ffreestanding -fms-extensions -fsyntax-only -Werror=implicit-function-declaration \
		"${sdk_inc[@]}" "$tree/$f" 2>&1 | grep -E "error" && failed=1 || echo "   ok  $f"
done
echo "== CLATTER Pocket engine (FM-1 configuration)"
n=0
while read -r f; do
	if ! "${cc[@]}" -ffast-math -freciprocal-math -ffp-contract=off -w -Werror=implicit-function-declaration \
		'-DPOCKET_BARRIER()=__asm__ volatile("" ::: "memory")' -include "$tree/pocket/fm1/pocket_fm1_config.h" \
		"${pocket_inc[@]}" "$f"; then
		echo "   FAIL ${f#$tree/}"; failed=1
	fi
	n=$((n + 1))
done < <(find "$tree/pocket" -name '*.c' | sort)
echo "   $n files compiled"
[[ $failed == 0 ]] && echo "preflight passed (stand-in compiler; not a PI32v2 build)" || { echo "preflight FAILED"; exit 1; }
