#!/usr/bin/env bash
# CLATTER Pocket FM-1 firmware build (macOS, Linux; on Windows run it from Git Bash).
#
#   firmware/fm1/build.sh [--prepare-only] [--placement sram|tcm] [--single-precision]
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
tree="$repo/build/fm1/tree"
placement=sram
single=()
prepare_only=0
while [[ $# -gt 0 ]]; do
	case "$1" in
	--prepare-only) prepare_only=1 ;;
	--placement) placement=$2; shift ;;
	--single-precision) single=(-SinglePrecision) ;;
	*) echo "unknown option $1" >&2; exit 2 ;;
	esac
	shift
done

pinned=4c7496d
fm1="$repo/third_party/fm1_open"
[[ -f "$fm1/sdk/manifest.json" ]] || { echo "third_party/fm1_open missing: git submodule update --init" >&2; exit 1; }
actual=$(git -C "$fm1" rev-parse --short=7 HEAD)
[[ "$actual" == "$pinned" ]] || echo "warning: fm1_open is at $actual, CLATTER Pocket was written against $pinned" >&2

echo "== preparing $tree"
rm -rf "$tree"
mkdir -p "$tree"
(cd "$fm1" && git archive --format=tar HEAD) | tar -x -C "$tree"
(cd "$tree" && patch -s -p1 < "$here/fm1_open.patch")
cp -R "$here/app/clatter_pocket" "$tree/app/"

pocket="$tree/pocket"
mkdir -p "$pocket/lxr/upstream/Core" "$pocket/lxr/bridge" "$pocket/engine" "$pocket/ui" "$pocket/fm1"
cp -R "$repo/lxr/upstream/Core/Inc" "$pocket/lxr/upstream/Core/"
cp "$repo/lxr/upstream/LICENSE" "$repo/lxr/upstream/NOTICE.md" "$repo/lxr/upstream/UPSTREAM_REVISION" "$pocket/lxr/upstream/"
cp -R "$repo/lxr/shim" "$pocket/lxr/"
# the firmware sources the FM-1 build links: everything CLATTER compiles except the FX bus
# (engine/fx_off.c stands in) and MidiParser.c (its handler is lxr/bridge/lxr_params.c)
(cd "$repo" && find lxr/upstream/Core/Src -name '*.c' ! -name MidiParser.c ! -name FxBus.c ! -path '*/FX/*') |
	while read -r f; do mkdir -p "$tree/pocket/$(dirname "$f")"; cp "$repo/$f" "$tree/pocket/$f"; done
cp "$repo/lxr/bridge/lxr_params.c" "$repo/lxr/bridge/libm.c" "$pocket/lxr/bridge/"
cp "$repo/engine/pocket_lxr.c" "$repo/engine/pocket_lxr.h" "$repo/engine/pocket_params.h" \
	"$repo/engine/fx_off.c" "$repo/engine/single_precision.h" "$pocket/engine/"
cp "$repo/ui/pocket_panel.c" "$repo/ui/pocket_panel.h" "$pocket/ui/"
cp "$here/pocket_fm1_config.h" "$pocket/fm1/"
echo "   $(find "$pocket" -name '*.c' | wc -l | tr -d ' ') CLATTER Pocket sources, fm1_open $actual + fm1_open.patch"
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

echo "== building (placement $placement${single:+, single precision})"
"$pwsh" -NoProfile -File "$here/Build-ClatterPocket.ps1" -Root "$tree" -FirmwareOnly \
	-Toolchain "$FM1_PI32_TOOLCHAIN" -Placement "$placement" "${wine[@]+"${wine[@]}"}" "${single[@]+"${single[@]}"}"

out="$repo/build/fm1/clatter-pocket-fm1-UNVERIFIED.fwsc"
cp "$tree/build/dx7/generated/FM1_OFFICIAL_OPEN.fwsc" "$out"
cp "$tree/build/dx7/firmware-report.json" "$repo/build/fm1/"
echo "== $out"
echo "   firmware checks: build/fm1/firmware-report.json, link map: build/fm1/tree/build/dx7/sdk.map"
echo "   NOT FLASHED. NOT HARDWARE-VALIDATED. See HARDWARE.md before installing."
