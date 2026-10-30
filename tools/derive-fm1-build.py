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
  7. build identity: covers pocket/ as well as app/;
  9. flash layout: CLATTER Pocket builds pad the packaged application image with erased-flash
     bytes (0xFF, after the last section; never executed or copied) so the package keeps the
     stock fm1_open V14 layout exactly (image end and VM start 471,040, VM 32,768 bytes, BTIF
     503,808, user partition 507,904), and check the result. Package.cs otherwise starts the
     VM (system settings, Bluetooth pairings) wherever a smaller image ends;
  8. -Replace (CLATTER Pocket instead of the stock instrument, build.sh --replace): the
     firmware checks keep every platform requirement (startup, registration tables, RTOS
     ABI, Bluetooth, USB MIDI, update, heap, panel scan, the platform audio path and
     CLATTER Pocket's audio path in SRAM); stock-instrument symbols (the synth.json SRAM
     contract, the engine's render/level/sequencer entry points, its hash-named internals
     and the tuning/project hooks) may be absent because the application no longer links
     them, are still checked for SRAM placement when present, and are listed in the
     firmware report.

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
    [switch]$Stock,
    # CLATTER Pocket as the instrument: the stock instrument application is not in the tree.
    [switch]$Replace
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
        # pocket/flash/ (init-only: the packed tables) and pocket/ui/ (the panel, control task
        # only) are never placed in SRAM, and are optimised for size
        $controlOnly = $source -like 'pocket/flash/*' -or $source -like 'pocket/ui/*'
        if ($controlOnly) { $options = @($options | Where-Object { $_ -cne '-O2' }) + @('-Oz') }
        $prefix = if ($controlOnly) { 'obj/pocketflash_' } else { 'obj/lxr_' }
        $output = $prefix + ($objectIndex++) + '_' + ($source -replace '[^a-zA-Z0-9._-]', '_') + '.obj'
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
    # CLATTER Pocket -Replace: keep the SDK's Bluetooth/BLE control API linked although no
    # CLATTER Pocket page calls all of it yet (the stock UI did; the firmware checks require it).
    $keepPlatform = if ($Replace) { @('fm1_ble_scan','fm1_ble_connect','fm1_ble_disconnect','fm1_ble_get_status',
        'fm1_ble_get_device','fm1_ble_forget_bonds','fm1_bluetooth_get_status','fm1_ble_set_role','fm1_bluetooth_configure') } else { @() }
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
    # Anchored to the object prefix: source names may contain 'lxr_' too (lxr_tables_*.c).
    # memset too: the compiler makes clearing loops on the audio path into calls to it, and Flash
    # cannot be fetched from while it is being written (tools/check-audio-path.py).
    if ($Placement -eq 'sram') { $code += "`n        obj/lxr_*.obj(.text .text.* .rodata .rodata.*)`n        *lib_a-memset.o(.text .text.*)" }
    $text = Edit-Once $text '        *(.volatile_ram_code .volatile_ram_code.*)' $code
    $text = Edit-Once $text '        *(.data .data.*)' "        *(.data .data.*)`n        *(.dtcm .dtcm.*)"
    $text = Edit-Once $text '        *(.bss .bss.*)' "        *(.dtcmz .dtcmz.*)`n        *(.bss .bss.*)"
    if ($Placement -eq 'sram') {
        $text = Edit-Once $text '        *(.text*)' '        *(EXCLUDE_FILE(obj/lxr_*.obj *lib_a-memset.o) .text*)'
        $text = Edit-Once $text '        *(.rodata*)' '        *(EXCLUDE_FILE(obj/lxr_*.obj) .rodata*)'
    }
    $text
}
function Test-Firmware {""")

# 9. flash layout
edit("""            Add-Type -Path (Join-Path $sdk 'Package.cs')
            $layout = [FM1.Package]::Create($image.ToArray(),(Join-Path $sdk 'tools/pack'),$output,$variant.user_bytes,$firmwareProduct)""",
     """            # CLATTER Pocket: keep the stock fm1_open V14 flash layout. Package.cs places the VM
            # (system settings) directly after the image, rounded to 4 KiB; pad the application with
            # erased-flash bytes after its last section so the image ends where stock's does.
            $stockImageEnd = 471040   # build.sh --stock: FLASH_BIN_SIZE of fm1_open 4c7496d
            $layoutPadding = 0
            if (!$Stock) {
                $minimumApp = $stockImageEnd - 4096 - (0x4000 + 0x120 + 383 + 64) + 1
                if ($image.Length -lt $minimumApp) {
                    $layoutPadding = [int]($minimumApp - $image.Length)
                    $fill = New-Object byte[] $layoutPadding
                    for ($i = 0; $i -lt $layoutPadding; $i++) { $fill[$i] = 0xff }
                    $image.Write($fill,0,$layoutPadding)
                }
            }
            Add-Type -Path (Join-Path $sdk 'Package.cs')
            $layout = [FM1.Package]::Create($image.ToArray(),(Join-Path $sdk 'tools/pack'),$output,$variant.user_bytes,$firmwareProduct)
            if (!$Stock -and ($layout.VM_START_ADDR -ne $stockImageEnd -or $layout.VM_REAL_SIZE -ne 32768 -or
                    $layout.BTIF_RESERVED_START -ne 503808 -or $layout.USER_RESERVED_START -ne 507904)) {
                throw 'CLATTER Pocket package does not keep the stock fm1_open V14 flash layout.'
            }
            $layout['layout_padding_bytes'] = $layoutPadding""")
# 8. replacement mode (before 6: edit 6 extends the same block)
edit("""        if ($source -in @('app/screen.c','app/transfer.c','app/midi_router.c')) {""",
     """        # CLATTER Pocket -Replace: its application unit holds the SDK entry points (MIDI,
        # SysEx) that the replaced units held; same treatment, and -Oz as app/screen.c.
        if ($source -in @('app/screen.c','app/transfer.c','app/midi_router.c','app/clatter_pocket/clatter_pocket_app.c')) {""")
edit("""        if ($source -eq 'app/screen.c') { $options += '-fno-inline-functions' }""",
     """        if ($source -eq 'app/screen.c') { $options += '-fno-inline-functions' }
        if ($source -eq 'app/clatter_pocket/clatter_pocket_app.c') { $options += '-Oz' }""")
edit("""    $link += @('--undefined=audio_app_bluetooth_open','--undefined=bt_get_local_name','--undefined=bt_get_mac_addr')""",
     """    $link += @('--undefined=audio_app_bluetooth_open','--undefined=bt_get_local_name','--undefined=bt_get_mac_addr')
    foreach ($name in $keepPlatform) { $link += '--undefined=' + $name }""")
edit("""    foreach ($name in $required) { if (!$info.ContainsKey($name)) { throw "Required runtime symbol absent: $name" } }""",
     """    # CLATTER Pocket -Replace: the stock instrument's hooks may be gone with the instrument.
    $stockInstrument = @('groove_tuning_capture','groove_tuning_apply','groove_project_apply_edit',
        'fm1_engine_render','fm1_engine_output_level','groove_sequence_render','groove_sequence_clock',
        'groove_sequence_position','groove_sequence_transport','fm1_sea_decode_groups','fm1_codec_sea_mono_s16_run',
        'fm1_r_0aa010fe1de1','fm1_r_33d5e5b63b6b') + @($extra) + @($synth.sram_required) + @($synth.sram_optional) + @($synth.native_sram_optional)
    $removedStock = New-Object 'Collections.Generic.List[string]'
    foreach ($name in $required) {
        if (!$info.ContainsKey($name)) {
            if ($Replace -and $stockInstrument -ccontains $name) { $removedStock.Add($name); continue }
            throw "Required runtime symbol absent: $name"
        }
    }""")
edit("""        if (!$info.ContainsKey($name) -or $info[$name].kind -cnotin @('T','N','t','n') -or !$info[$name].size) {
            throw "Application callback was lost or replaced by a weak default: $name"
        }""",
     """        if ($Replace -and !$info.ContainsKey($name) -and $stockInstrument -ccontains $name) { $removedStock.Add($name); continue }
        if (!$info.ContainsKey($name) -or $info[$name].kind -cnotin @('T','N','t','n') -or !$info[$name].size) {
            throw "Application callback was lost or replaced by a weak default: $name"
        }""")
edit("""    foreach ($name in $callbacks) {
        if (!$info.ContainsKey($name)) { throw "Audio callback absent: $name" }""",
     """    foreach ($name in $callbacks) {
        if ($Replace -and !$info.ContainsKey($name) -and $stockInstrument -ccontains $name) { $removedStock.Add($name); continue }
        if (!$info.ContainsKey($name)) { throw "Audio callback absent: $name" }""")
edit("""        application_callbacks_verified=$true; board_validation='not performed'}""",
     """        application_callbacks_verified=$true; board_validation='not performed'}
    if ($Replace) {
        $result['instrument'] = 'CLATTER Pocket (stock instrument application replaced)'
        $result['stock_instrument_symbols_absent'] = @($removedStock | Sort-Object -Unique)
    }""")

# 6. Test-Firmware: the CLATTER Pocket audio path in internal SRAM
edit("""    foreach ($name in @($synth.sram_optional) + @($synth.native_sram_optional)) {""",
     """    # CLATTER Pocket: the audio hook, the engine host and the LXR's own TCM code and state.
    $pocketHooks = if ($Replace) { @('clatter_pocket_render','clatter_pocket_output_level') } else { @('clatter_pocket_fm1_render') }
    if (!$Stock) { $callbacks += @($pocketHooks) + @('pocket_lxr_render_q23','calcDrumVoiceSyncBlock','calcDrumVoiceAsync',
        'mixer_decimateBlock','SVF_recalcFreq','slopeEg2_calc','calcNextOscSampleBlock','sine_table','sqrtf',
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
print(f'{dest.relative_to(root)}: derived from Build.ps1 {digest[:12]}, 9 edit groups applied')
