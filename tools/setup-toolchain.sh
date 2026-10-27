#!/usr/bin/env bash
# Install the JieLi Windows toolchain 2.5.3 (PI32v2 clang 4.0.1) that the FM-1 SDK requires,
# under Wine (macOS, Linux), and verify every file the SDK pins.
#
#   tools/setup-toolchain.sh
#
# 1. downloads JieLi's installer from the URL fm1_open's README gives, into
#    ~/.cache/clatter-pocket/, and checks its SHA-256 (recorded below on 2026-10-27);
# 2. runs the installer silently in its own Wine prefix (WINEPREFIX, default ~/.wine-jieli)
#    into C:\JL - this executes JieLi's installer, review before running;
# 3. checks all toolchain files against third_party/fm1_open/sdk/manifest.json.
# Then: export FM1_PI32_TOOLCHAIN=<printed path>. On Windows, install 2.5.3 normally and set
# FM1_PI32_TOOLCHAIN=C:\JL\pi32 instead.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
url=https://jl-update.oss-cn-shenzhen.aliyuncs.com/2.5.3.exe
installer_sha256=399bd518a28e47a7a0394506cebd5bf658748a58cd6b7c02512e24b4b88c5a5e
cache=$HOME/.cache/clatter-pocket
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine-jieli} WINEDEBUG=${WINEDEBUG:--all}
command -v wine >/dev/null || { echo "install Wine first (macOS: the Wine Stable app or brew install --cask wine-stable)" >&2; exit 1; }

mkdir -p "$cache"
exe="$cache/jieli-toolchain-2.5.3.exe"
[[ -f "$exe" ]] || curl -fL --retry 3 -o "$exe" "$url"
actual=$(shasum -a 256 "$exe" | cut -d' ' -f1)
[[ "$actual" == "$installer_sha256" ]] || { echo "installer SHA-256 $actual does not match $installer_sha256" >&2; exit 1; }

pi32="$WINEPREFIX/drive_c/JL/pi32"
if [[ ! -f "$pi32/bin/clang.exe" ]]; then
	echo "installing into $WINEPREFIX (C:\\JL)"
	wine "$exe" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- '/DIR=C:\JL'
fi
python3 - "$repo/third_party/fm1_open/sdk/manifest.json" "$pi32" <<'EOF'
import hashlib, json, pathlib, sys
m = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding='utf-8-sig'))['external_toolchain']
root = pathlib.Path(sys.argv[2])
bad = [n for n, h in m['files'].items() if not (root / n).is_file() or hashlib.sha256((root / n).read_bytes()).hexdigest() != h]
print(f"{m['name']}: {len(m['files']) - len(bad)}/{len(m['files'])} files match the SDK manifest")
if bad:
    print('mismatch or missing:', *bad, sep='\n  '); sys.exit(1)
EOF
echo "export FM1_PI32_TOOLCHAIN=\"$pi32\""
