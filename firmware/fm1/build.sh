#!/usr/bin/env bash
# CLATTER Pocket FM-1 firmware build (macOS, Linux; on Windows run it from Git Bash).
#
#   firmware/fm1/build.sh [--prepare-only] [--placement sram|tcm] [--single-precision]
#   firmware/fm1/build.sh --stock     (the unmodified fm1_open, same toolchain and steps:
#                                      the known-good comparison, in build/fm1-stock/)
#   firmware/fm1/build.sh --replace [--placement ...] [--single-precision]
#                                     (CLATTER Pocket instead of the stock instrument
#                                      application, in build/fm1-replace/; BUILDING.md)
#
# 1. prepares build/fm1/tree: the pinned fm1_open (third_party/fm1_open), with
#    firmware/fm1/fm1_open.patch applied, the FM-1 adapter in app/clatter_pocket/ and
#    exactly the CLATTER Pocket engine files the firmware links in pocket/;
# 2. runs firmware/fm1/Build-ClatterPocket.ps1 (PowerShell 7, the JieLi toolchain; through
#    Wine unless on Windows) with -FirmwareOnly, which checks the SDK and toolchain hashes,
#    compiles, links, runs fm1_open's firmware checks plus CLATTER Pocket's, and packages
#    build/fm1/tree/build/dx7/generated/FM1_OFFICIAL_OPEN.fwsc;
# 3. copies the package to build/fm1/clatter-pocket-fm1-UNVERIFIED.fwsc.
#
# NOTHING HERE FLASHES A DEVICE. Read HARDWARE.md (recovery) before installing any build.
# Environment: FM1_PI32_TOOLCHAIN (the toolchain's pi32 folder), PWSH (default pwsh),
# WINEPREFIX (default ~/.wine-jieli, as tools/setup-toolchain.sh installs it).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
stock=0
replace=0
placement=sram
single=()
prepare_only=0
while [[ $# -gt 0 ]]; do
	case "$1" in
	--prepare-only) prepare_only=1 ;;
	--placement) placement=$2; shift ;;
	--single-precision) single=(-SinglePrecision) ;;
	--stock) stock=1 ;;
	--replace) replace=1 ;;
	*) echo "unknown option $1" >&2; exit 2 ;;
	esac
	shift
done

out_dir="$repo/build/fm1"
[[ $stock == 1 ]] && out_dir="$repo/build/fm1-stock"
[[ $replace == 1 ]] && out_dir="$repo/build/fm1-replace"
tree="$out_dir/tree"

pinned=4c7496d
fm1="$repo/third_party/fm1_open"
[[ -f "$fm1/sdk/manifest.json" ]] || { echo "third_party/fm1_open missing: git submodule update --init" >&2; exit 1; }
actual=$(git -C "$fm1" rev-parse --short=7 HEAD)
[[ "$actual" == "$pinned" ]] || echo "warning: fm1_open is at $actual, CLATTER Pocket was written against $pinned" >&2

echo "== preparing $tree"
rm -rf "$tree"
mkdir -p "$tree"
(cd "$fm1" && git archive --format=tar HEAD) | tar -x -C "$tree"
if [[ $stock == 1 ]]; then
	echo "   unmodified fm1_open $actual"
elif [[ $replace == 1 ]]; then
	# The stock instrument application goes; the board layer (app/board/), the output mix
	# (app/audio_output.c) and the headers stay. firmware/fm1/replace/ has the replacement.
	(cd "$tree" && patch -s -p1 < "$here/replace/fm1_open-replace.patch")
	for f in screen.c project.c presets.c samples.c transfer.c midi_router.c fm_library.c fm_factory.c; do
		rm "$tree/app/$f"
	done
	mkdir -p "$tree/app/clatter_pocket"
	cp "$here/replace/clatter_pocket_app.c" "$here/replace/clatter_pocket_app.h" "$tree/app/clatter_pocket/"
else
	(cd "$tree" && patch -s -p1 < "$here/fm1_open.patch")
	cp -R "$here/app/clatter_pocket" "$tree/app/"
fi
if [[ $stock == 0 ]]; then

pocket="$tree/pocket"
mkdir -p "$pocket/lxr/upstream/Core" "$pocket/lxr/bridge" "$pocket/engine" "$pocket/ui" "$pocket/fm1"
cp -R "$repo/lxr/upstream/Core/Inc" "$pocket/lxr/upstream/Core/"
cp "$repo/lxr/upstream/LICENSE" "$repo/lxr/upstream/NOTICE.md" "$repo/lxr/upstream/UPSTREAM_REVISION" "$pocket/lxr/upstream/"
cp -R "$repo/lxr/shim" "$pocket/lxr/"
# the firmware sources the FM-1 build links: everything CLATTER compiles except the FX bus
# (engine/fx_off.c stands in) and MidiParser.c (its handler is lxr/bridge/lxr_params.c)
# and the constant tables (wavetable.c, transientTables.c: packed, pocket/flash/)
(cd "$repo" && find lxr/upstream/Core/Src -name '*.c' ! -name MidiParser.c ! -name FxBus.c ! -path '*/FX/*' \
	! -name wavetable.c ! -name transientTables.c) |
	while read -r f; do mkdir -p "$tree/pocket/$(dirname "$f")"; cp "$repo/$f" "$tree/pocket/$f"; done
cp "$repo/lxr/bridge/lxr_params.c" "$repo/lxr/bridge/libm.c" "$pocket/lxr/bridge/"
cp "$repo/engine/pocket_lxr.c" "$repo/engine/pocket_lxr.h" "$repo/engine/pocket_params.h" \
	"$repo/engine/fx_off.c" "$repo/engine/single_precision.h" "$pocket/engine/"
cp "$repo/ui/pocket_panel.c" "$repo/ui/pocket_panel.h" "$repo/ui/pocket_midi.c" "$repo/ui/pocket_midi.h" "$pocket/ui/"
cp "$here/pocket_fm1_config.h" "$here/pocket_fm1_libm.c" "$pocket/fm1/"
# init-only code and data: stays in Flash with every placement
mkdir -p "$pocket/flash"
cp "$repo/engine/lxr_tables_packed.c" "$repo/engine/lxr_tables_unpack.c" "$pocket/flash/"
cp "$repo/engine/lxr_tables_packed.h" "$pocket/engine/"
echo "   $(find "$pocket" -name '*.c' | wc -l | tr -d ' ') CLATTER Pocket sources, fm1_open $actual + $([[ $replace == 1 ]] && echo "replace/fm1_open-replace.patch, stock instrument application removed" || echo fm1_open.patch)"
fi
[[ $prepare_only == 1 ]] && exit 0

pwsh=${PWSH:-pwsh}
command -v "$pwsh" >/dev/null || { echo "PowerShell 7 (pwsh) is required: BUILDING.md" >&2; exit 1; }
[[ -n "${FM1_PI32_TOOLCHAIN:-}" ]] || { echo "set FM1_PI32_TOOLCHAIN to the JieLi 2.5.3 pi32 folder: BUILDING.md" >&2; exit 1; }
wine=()
case "$(uname -s)" in
MINGW* | MSYS* | CYGWIN*) ;;
*)
	command -v wine >/dev/null || { echo "Wine is required to run the Windows toolchain: BUILDING.md" >&2; exit 1; }
	export WINEPREFIX=${WINEPREFIX:-$HOME/.wine-jieli} WINEDEBUG=${WINEDEBUG:--all}
	wine=(-Wine)
	;;
esac

if [[ $stock == 1 ]]; then
	echo "== building unmodified fm1_open"
	single=(-Stock)
else
	echo "== building (placement $placement${single:+, single precision}$([[ $replace == 1 ]] && echo ", replacing the stock instrument"))"
	[[ $replace == 1 ]] && single+=(-Replace)
fi
"$pwsh" -NoProfile -File "$here/Build-ClatterPocket.ps1" -Root "$tree" -FirmwareOnly \
	-Toolchain "$FM1_PI32_TOOLCHAIN" -Placement "$placement" "${wine[@]+"${wine[@]}"}" "${single[@]+"${single[@]}"}"

out="$out_dir/clatter-pocket-fm1-UNVERIFIED.fwsc"
[[ $stock == 1 ]] && out="$out_dir/fm1_open-stock-$actual.fwsc"
[[ $replace == 1 ]] && out="$out_dir/clatter-pocket-fm1-replace-UNVERIFIED.fwsc"
cp "$tree/build/dx7/generated/FM1_OFFICIAL_OPEN.fwsc" "$out"
cp "$tree/build/dx7/firmware-report.json" "$out_dir/"
echo "== $out"
echo "   firmware checks: ${out_dir#$repo/}/firmware-report.json, link map: ${out_dir#$repo/}/tree/build/dx7/sdk.map"
echo "   NOT FLASHED. NOT HARDWARE-VALIDATED. See HARDWARE.md before installing."
