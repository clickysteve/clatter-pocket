#!/usr/bin/env bash
# The package in the FM-1 emulator under the chip's alignment rule. On a device a misaligned 16- or
# 32-bit RAM access is a fault that cannot be masked, and it resets the chip; the emulator's default
# mode and tools/emu-test.mjs allow it, so nothing else here can see one. This boots the package, plays
# seven keys and two bars of the sequencer, and fails on any misaligned access or on a second reset
# (every firmware, stock too, resets once near 270 ms).
#
#   EMU_STRICT=path/to/fm1-emu tools/emu-strict.sh [PACKAGE]     about 10 minutes
#
# EMU_STRICT is the native fm1-emu with tools/fm1-emu-strict.patch, which turns the rule on outside
# fm1-live (its FM1_STRICT=1, which does not build on every Mac) and names each instruction:
#
#   git clone https://github.com/joakimeriksson/fm1-emu.git && cd fm1-emu
#   git checkout 9de1ef053a3c4bf56f16f05b5f0de193f4608a1d
#   git apply path/to/tools/fm1-emu-strict.patch
#   cargo build --release -p fm1-emu
#
# Rust 1.94 needs RUSTC_BOOTSTRAP=1 RUSTFLAGS='-Zcrate-attr=feature(if_let_guard) -Awarnings'.
#
# Copyright (C) 2026 The CLATTER contributors. SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail

package=${1:-build/fm1-replace/clatter-pocket-fm1-replace-UNVERIFIED.fwsc}
[[ -x "${EMU_STRICT:-}" ]] || { echo "set EMU_STRICT to the patched fm1-emu: see the top of this file" >&2; exit 2; }
hash=$(shasum -a 256 "$package" | cut -d' ' -f1)
log=$(mktemp)
trap 'rm -f "$log"' EXIT

# nN is a note key and kN a button: k12 is PLAY
keys='2300-2400:n7,2500-2600:n8,2700-2800:n9,2900-3000:n10,3100-3200:n11,3300-3400:n12,3500-3600:n13,3900-4000:k12'
FM1_ALIGN=1 FM1_ALIGN_SURVEY=1 "$EMU_STRICT" --fwsc "$package" --ms 9000 --keys "$keys" >"$log" 2>&1

misaligned=$(grep -c '^MISALIGNED' "$log" || true)
resets=$(grep -c '^chip reset' "$log" || true)
grep -E '^(MISALIGNED|chip reset)' "$log" | head -20 || true
if [[ $misaligned != 0 || $resets != 1 ]]; then
	echo "strict alignment: FAILED, $misaligned misaligned, $resets resets, $hash"
	exit 1
fi
echo "strict alignment: passed, $hash"
