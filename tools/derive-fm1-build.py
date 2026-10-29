#!/usr/bin/env python3
"""Generate firmware/fm1/Build-ClatterPocket.ps1 from fm1_open's sdk/Build.ps1.

The FM-1 Open build script is part of the SDK's hash manifest, so CLATTER Pocket does not
edit it. Instead this tool derives a copy with a fixed list of edits, each of which must
match the upstream text exactly once (otherwise it stops: review the upstream change and
update the edit). Everything else - the SDK and toolchain hash checks, the compiler and
linker flags, the firmware checks (Test-Firmware) and the packager (Package.cs) - is
upstream's, unchanged.

The edits:
  1. parameters: -Root (the prepared tree), -Wine, -Placement sram|tcm, -SinglePrecision,
     -Stock (every CLATTER Pocket edit below switched off: the unmodified fm1_open, built
     the same way, as the known-good comparison);
  2. paths: the tree instead of the script's own folder; '/' or '\\' as the platform has it;
  3. tools: run the Windows toolchain through Wine when asked;
  4. CLATTER Pocket sources (pocket/): compiled with the LXR's flags and CLATTER's shim,
     without LTO, as lxr_*.obj, and linked with the application;
  5. linker script: the LXR's tightly coupled memory sections (.itcm_text, .dtcm, .dtcmz)
     and, with -Placement sram, all lxr_*.obj code and constants, in internal SRAM;
  6. Test-Firmware: the CLATTER Pocket audio path must be in internal SRAM too;
  7. build identity: covers pocket/ as well as app/.

    python3 tools/derive-fm1-build.py      (from the repository root)

Copyright (C) 2026 The CLATTER contributors. The generated script is derived from
fm1_open's sdk/Build.ps1, Copyright (c) 2026 M-VAVE, GPL-3.0-only WITH the FM1 SDK linking
exception (third_party/fm1_open/LICENSE.md). SPDX-License-Identifier: GPL-3.0-only
"""
import hashlib
import pathlib

root = pathlib.Path(__file__).resolve().parents[1]
src_path = root / 'third_party/fm1_open/sdk/Build.ps1'
src = src_path.read_text(encoding='utf-8')
out = src


def edit(old, new):
    global out
    n = out.count(old)
    if n != 1:
        raise SystemExit(f'Build.ps1 changed: expected one match, found {n}:\n{old}')
    out = out.replace(old, new)


# 1. parameters
edit("""    [string]$Toolchain = $env:FM1_PI32_TOOLCHAIN,
    [string]$SourceUrl = $env:FM1_SOURCE_URL
)""", """    [string]$Toolchain = $env:FM1_PI32_TOOLCHAIN,
    [string]$SourceUrl = $env:FM1_SOURCE_URL,
    # CLATTER Pocket: the prepared tree (fm1_open, firmware/fm1/fm1_open.patch, pocket/).
    [Parameter(Mandatory = $true)][string]$Root,
    # Run the Windows toolchain through Wine (macOS, Linux).
    [switch]$Wine,
    # sram: all CLATTER Pocket code and constants in internal SRAM. tcm: only what the LXR
    # firmware itself keeps in tightly coupled memory (.itcm_text, .dtcm, .dtcmz).
    [ValidateSet('sram', 'tcm')][string]$Placement = 'sram',
    # Compute the LXR's double-precision arithmetic in single precision.
    [switch]$SinglePrecision,
    # Build the unmodified fm1_open tree (no pocket/): the known-good comparison.
    [switch]$Stock
)""")

# 2. paths
edit("$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))",
     "$root = [IO.Path]::GetFullPath($Root)")
edit("$sdk = $PSScriptRoot", "$sdk = Join-Path $root 'sdk'\n$sep = [string][IO.Path]::DirectorySeparatorChar")
edit("$full.StartsWith($sdk + '\\', [StringComparison]::OrdinalIgnoreCase)",
     "$full.StartsWith($sdk + $sep, [StringComparison]::OrdinalIgnoreCase)")
edit("(Join-Path $root 'app/synth'))+'\\')", "(Join-Path $root 'app/synth'))+$sep)")
edit("$objectDirectory.StartsWith($root + '\\build\\', [StringComparison]::OrdinalIgnoreCase)",
     "$objectDirectory.StartsWith((Join-Path $root 'build') + $sep, [StringComparison]::OrdinalIgnoreCase)")

# 3. tools through Wine
edit("try { & $program @arguments; $result = $LASTEXITCODE }",
     "try { if ($Wine) { & wine $program @arguments } else { & $program @arguments }; $result = $LASTEXITCODE }")
edit("$symbols = & (Join-Path $bin 'llvm-nm.exe') --print-size $elf",
     "$symbols = if ($Wine) { & wine (Join-Path $bin 'llvm-nm.exe') --print-size $elf } else { & (Join-Path $bin 'llvm-nm.exe') --print-size $elf }")

# 4. CLATTER Pocket sources
edit("$include = @('sdk/include','sdk/include/libc','app/synth','sdk/config','app','app/board')",
     "$include = @('sdk/include','sdk/include/libc','app/synth','sdk/config','app','app/board') + @(if (!$Stock) { 'pocket/engine','pocket/ui' })")
edit("""    # The device reports this source/SDK build identity after a browser update.""",
     """    # CLATTER Pocket: the LXR engine (pocket/, prepared by firmware/fm1/build.sh). Compiled
    # with the LXR firmware's floating-point flags and CLATTER's shim, without LTO so the
    # linker script can place these objects by name (lxr_*.obj). Forced includes go by name
    # through the include path: the Windows clang under Wine does not open them by Unix path.
    $pocketInclude = @('pocket/fm1','pocket/lxr/shim','pocket/lxr/shim/libc','pocket/lxr/upstream/Core/Inc','pocket/engine','pocket/ui','sdk/include/libc')
    # The application's include paths are left out: the engine resolves <math.h> etc. to
    # CLATTER's shim first, as on the host, and only falls back to the SDK's libc headers.
    $pocketFlags = @($flags | Where-Object { $_ -cne '-flto' -and $_ -cne '-O2' -and !$_.StartsWith('-I') }) + @('-fno-lto','-O2',
        '-ffast-math','-freciprocal-math','-ffp-contract=off','-w','-include','pocket_fm1_config.h')
    foreach ($dir in $pocketInclude) { $pocketFlags += '-I' + (Join-Path $root $dir) }
    $pocketSources = if ($Stock) { @() } else { @(Get-ChildItem -LiteralPath (Join-Path $root 'pocket') -Recurse -File -Filter '*.c' | Sort-Object FullName |
        ForEach-Object { $_.FullName.Substring($root.Length+1).Replace('\\','/') }) }
    if (!$Stock -and $pocketSources.Count -lt 25) { throw 'CLATTER Pocket sources missing: run firmware/fm1/build.sh.' }
    foreach ($source in $pocketSources) {
        $options = @($pocketFlags)
        if ($SinglePrecision -and $source -notmatch '/libm\\.c$') { $options += @('-include','single_precision.h') }
        $output = 'obj/lxr_' + ($objectIndex++) + '_' + ($source -replace '[^a-zA-Z0-9._-]', '_') + '.obj'
        $arguments = @($options) + @('-MMD','-MF',($output + '.d'),'-c',(Join-Path $root $source),'-o',$output)
        Write-Host "Compile $source"
        Run (Join-Path $bin 'clang.exe') $arguments
        $objects += $output
        $commands += [ordered]@{directory=$build; file=(Join-Path $root $source); output=(Join-Path $build $output); arguments=@((Join-Path $bin 'clang.exe')) + $arguments}
    }
    # The device reports this source/SDK build identity after a browser update.""")

# 5. linker script
edit("""    Write-Text (Join-Path $build 'generated/linker') $linker""",
     """    if (!$Stock) { $linker = Add-PocketSections $linker }
    Write-Text (Join-Path $build 'generated/linker') $linker""")
edit("""function Test-Firmware {""", """function Add-PocketSections([string]$text) {
    # The LXR firmware keeps its per-sample code and voice state in the STM32's tightly coupled
    # memories (config.h IN_ITCM_RAM, INDTCM, INDTCMZ). On the FM-1 the equivalent is internal
    # SRAM, where audio code must live anyway (fm1.h FM1_AUDIO_CODE).
    function Edit-Once([string]$t, [string]$old, [string]$new) {
        $at = $t.IndexOf($old)
        if ($at -lt 0 -or $t.IndexOf($old, $at + 1) -ge 0) { throw "linker.ld.in changed near: $old" }
        $t.Substring(0, $at) + $new + $t.Substring($at + $old.Length)
    }
    $code = "        *(.volatile_ram_code .volatile_ram_code.*)`n        *(.itcm_text .itcm_text.*)"
    if ($Placement -eq 'sram') { $code += "`n        *lxr_*.obj(.text .text.* .rodata .rodata.*)" }
    $text = Edit-Once $text '        *(.volatile_ram_code .volatile_ram_code.*)' $code
    $text = Edit-Once $text '        *(.data .data.*)' "        *(.data .data.*)`n        *(.dtcm .dtcm.*)"
    $text = Edit-Once $text '        *(.bss .bss.*)' "        *(.dtcmz .dtcmz.*)`n        *(.bss .bss.*)"
    if ($Placement -eq 'sram') {
        $text = Edit-Once $text '        *(.text*)' '        *(EXCLUDE_FILE(*lxr_*.obj) .text*)'
        $text = Edit-Once $text '        *(.rodata*)' '        *(EXCLUDE_FILE(*lxr_*.obj) .rodata*)'
    }
    $text
}
function Test-Firmware {""")

# 6. Test-Firmware: the CLATTER Pocket audio path in internal SRAM
edit("""    foreach ($name in @($synth.sram_optional) + @($synth.native_sram_optional)) {""",
     """    # CLATTER Pocket: the audio hook, the engine host and the LXR's own TCM code and state.
    if (!$Stock) { $callbacks += @('clatter_pocket_fm1_render','pocket_lxr_render_q23','calcDrumVoiceSyncBlock','calcDrumVoiceAsync',
        'mixer_decimateBlock','SVF_recalcFreq','slopeEg2_calc','calcNextOscSampleBlock','sine_table',
        'voiceArray','snareVoice','cymbalVoice','hatVoice')
    if ($Placement -eq 'sram') {
        $callbacks += @('midiParser_ccHandler','Drum_trigger','Snare_calcSyncBlock','Cymbal_calcSyncBlock',
            'HiHat_calcSyncBlock','lfo_dispatchNextValue','triTable','sawTable','recTable','transientData')
    } }
    foreach ($name in @($synth.sram_optional) + @($synth.native_sram_optional)) {""")

# 7. build identity
edit("""    foreach ($file in Get-ChildItem -LiteralPath (Join-Path $root 'app') -Recurse -File | Sort-Object FullName) {""",
     """    foreach ($file in @(Get-ChildItem -LiteralPath (Join-Path $root 'app') -Recurse -File) + @(if (!$Stock) { Get-ChildItem -LiteralPath (Join-Path $root 'pocket') -Recurse -File }) | Sort-Object FullName) {""")

digest = hashlib.sha256(src.encode('utf-8')).hexdigest()
header = f"""# GENERATED by tools/derive-fm1-build.py from third_party/fm1_open/sdk/Build.ps1
# (sha256 {digest}). Do not edit: change the generator.
# CLATTER Pocket's FM-1 firmware build. Builds with JieLi 2.5.3 (BUILDING.md, First build
# results); no package has been run on hardware. Derived work: GPL-3.0-only WITH the FM1 SDK
# linking exception.
"""
dest = root / 'firmware/fm1/Build-ClatterPocket.ps1'
dest.write_text(header + out, encoding='utf-8')
print(f'{dest.relative_to(root)}: derived from Build.ps1 {digest[:12]}, 7 edit groups applied')
