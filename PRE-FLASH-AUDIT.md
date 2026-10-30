# Pre-flash audit: CLATTER Pocket replacement firmware

Audit of the replacement firmware (FM1-REPLACEMENT.md) before its first installation,
2026-10-29. Baseline: commit `abbe966`. Nothing was flashed and no device was connected or
touched.

Evidence labels:
- **SOURCE**: verified by reading source (fm1_open, the SDK headers and scripts, CLATTER
  Pocket);
- **BUILD**: verified in the build output (the linked ELF, its disassembly, link map,
  firmware report, package);
- **HOST**: verified by a host test;
- **HARDWARE**: requires the hardware test, not verified.

## Result

| | |
|---|---|
| Package | `build/fm1-replace/clatter-pocket-fm1-replace-UNVERIFIED.fwsc`, 492,084 bytes |
| Package SHA-256 | `b8c13d2c4d3a40879164261e45cf9b30124b4837ccb0bf7b3c823ca0f30e7890` |
| ELF SHA-256 | `2d3ce3a6b1e0765fe8c41f8ac7298fa9d1ebc86fd807dfe7a04dabcf248524f1` |
| Installer page | `build/fm1-replace/clatter-pocket-installer-UNVERIFIED.html`, SHA-256 `53a4e2e02f43421f696cd9518721f63cd90d1b752c77ba944907d01be85f2640` |
| Reproducibility | two complete builds from an empty `build/` gave identical package, ELF and page (BUILD) |
| App image | 421,212 bytes; limit 453,921; **headroom 32,709** (BUILD) |
| Flash layout | identical to stock fm1_open V14: VM 471,040 (32,768 bytes), BTIF 503,808, USER 507,904 to 1,044,480 (BUILD) |
| RAM | `.ram0_data` 104,924, `.ram0_bss` 284,492; **runtime heap 134,124** (SDK minimum 49,152) (BUILD) |
| SDK firmware checks | pass, in replacement mode (BUILD) |
| Host tests | 177 checks pass, plus 2 byte-identity comparisons (HOST) |

## Baseline reproduced

The `abbe966` build reproduced exactly from clean (BUILD):
- image 419,964;
- heap 110,156;
- package `3cd6a723…`.

Two of the previous report's figures were wrong:

- **Flash limit.** The report gave 479,232. `Package.Create` (SOURCE) rejects images larger
  than 0xff000 − 536,576 − 4,096 − 32,768 − 16,384 − 288 − 383 − 64 = **453,921 bytes**.
  The earlier figure left out the boot area, BTIF and the directory. Corrected throughout:
  - stock fm1_open V14 has 345 bytes to spare;
  - the `abbe966` build had 33,957, not 59,268.
- **Clean builds.** A clean host build failed: one Makefile pattern rule with four target
  patterns is a single recipe for all four in GNU make, so objects of other variants were
  never built. Earlier runs had passed on objects left from incremental builds. Fixed with
  one rule per variant (HOST).

## Findings and fixes

| # | Finding | Severity | Fix | Evidence |
|---|---|---|---|---|
| 1 | **`sqrtf` miscompiled to nothing.** CLATTER's shim defines `static inline sqrtf(x) { return __builtin_sqrtf(x); }`. PI32v2 has no square root, so the builtin becomes a call to `sqrtf`, which is the wrapper itself; LLVM deletes the infinite recursion as undefined behaviour. All voices' trigger-time pan gains (`sqrtf((127 - pan) / 127)`) were garbage on the device; the host compiler has a square-root instruction, so no host test could see it | critical | `pocket_fm1_config.h` renames the shim's `sqrtf`/`sqrt` wrappers, so the builtin resolves to the real, correctly rounded `sqrtf` (`pocket_fm1_libm.c`, bit-exact against the host libm on 20M+ inputs). The firmware check now requires `sqrtf` in SRAM; a double `sqrt` would fail to link | BUILD: `sqrtf` is a 232-byte SRAM function called twice each from `Drum_trigger`, `Snare_trigger`, `Cymbal_trigger` and `HiHat_trigger`. All other shim wrappers checked: `fabs`/`fabsf` (bit mask), `floorf`/`ceilf`/`truncf` (FPU instructions) are correct |
| 2 | **MIDI drum hits five octaves low.** The replacement app triggered with note 0, but the engine's programmed pitch is note 60 | high | note 60 | SOURCE (`pocket_lxr.h`), HOST (engine pitch tests) |
| 3 | **The system-settings VM moved.** A smaller image made `Package.Create` start the VM at 438,272 instead of 471,040 | high | application image padded to the stock boundary; the build asserts the stock layout | BUILD (`tools/fwsc-layout.py`); section 2 below |
| 4 | **Engine started before the updater.** `groove_screen_init` runs before `fm1_update_init`, `fm1_io_start` and `fm1_audio_start` (ELF). A fault or hang while unpacking tables or initialising the LXR would have stopped the updater from ever starting | high | the engine starts in the control loop 1.5 s later; SAFE MODE (hold HOME) skips it | BUILD (call order); the behaviour is HARDWARE |
| 5 | **Packed tables in SRAM.** The linker pattern `*lxr_*.obj` also matched `…_lxr_tables_*.obj`, putting 24 KB of init-only data and the unpacker in SRAM | medium | patterns anchored to `obj/lxr_*.obj` | BUILD: data at 0x0201…, heap +24,352 |
| 6 | **No installation route.** The SDK's workbench page needs a browser simulator built from the stock sources (WASI SDK), which the replacement tree lacks | blocker for a test | `tools/make-installer-page.py`: M-VAVE's `WebUpdate.js` and `I18n.js` byte for byte, the package description `Build.ps1 -Package` builds, a minimal page, no force mode | HOST: the page loads in Chrome without errors, and `FM1Web.prepare` accepts the package and rejects a wrong hash and a one-bit corruption. Installing is HARDWARE |
| 7 | **No way to tell from the device whether it booted correctly** | medium | start-up diagnostics on the panel (FM1-REPLACEMENT.md) | HOST (panel tests); HARDWARE |
| 8 | MIDI parsing lived in the adapter, untested | low | `ui/pocket_midi.c`, platform-independent | HOST: all 16,777,216 three-byte messages, other lengths and malformed input, under AddressSanitizer |
| 9 | The LXR's `removeMaliciousSample` NaN guard compiles to nothing under `-ffast-math` | note | none: identical to the host and to CLATTER | BUILD |

## 2. System-settings (VM) partition

1. **What controls its size.** `Package.Create` computes it; there is no setting. The VM
   starts at the end of the image, rounded up to 4 KiB, and ends at BTIF (503,808). It is
   at least 32,768 bytes. (SOURCE)
2. **Automatic expansion.** Yes. Any image smaller than stock's gets a larger VM that
   starts lower. (SOURCE, BUILD)
3. **Bluetooth pairings.** Yes, the VM holds them. The SDK reads paired devices from it
   (`get_remote_device_info_from_vm`) and keeps syscfg items there
   (`syscfg_vm_init`, `syscfg_read`/`syscfg_write`). (BUILD: symbol names)
4. **Identity and calibration.** The VM is not where these live. The update identity is in
   the image (`fm1_update_identity`). The chip key/MAC area (`key_mac`, 1,044,480) and BTIF
   (Bluetooth information) are separate fixed regions. Neither is written by an update,
   because `flash.bin` stops at the image end. Whether any radio calibration is in the VM is
   not documented. (SOURCE, BUILD; contents HARDWARE)
5. **Readability after a change.** The SDK validates the VM by ID at its start
   (`vm_check_id`, `vm_area_check`) and has reset and erase paths (`vm_reset`,
   `_vm_area_erase`). A moved VM is expected to be erased and re-created, which loses
   pairings. User data is unaffected either way. Without SDK source this is inferred, not
   proven. (BUILD: symbols; HARDWARE)
6. **Overlap.** Yes. A moved VM starts in flash the previous firmware used for code
   (438,272 to 471,040). (BUILD)
7. **Keeping the original dimensions.** Yes, and this is implemented. The application is
   padded with 28,614 bytes of `0xFF` after its last section, so the image ends at 471,040
   as stock's does. The padding is never executed or copied: the boot copies `.ram0_data`
   by its linker symbols, and the CRC covers the padding. (BUILD)
8. **Restoring stock.** Restoring official Open V14, which has the same layout, keeps the VM
   in place. Restoring M-VAVE V15 moves VM and BTIF; that is the vendor's own V15 ↔ Open
   migration, unchanged by CLATTER Pocket. (BUILD: decoded V15 package)
9. **Erase or migration by the update.** The update writes `flash.bin` (0 to the image end)
   and nothing else, so the VM, BTIF, USER and `key_mac` regions are not in the payload.
   The vendor loader's own erase behaviour beyond `flash.bin` is not documented. (SOURCE:
   `Package.cs`, `WebUpdate.js`; HARDWARE)

## 3. Update and recovery path

| | |
|---|---|
| Protocol | `WebUpdate.js` (SOURCE). Stage 1, in the installed firmware's SDK: SysEx `00 59` frames for identity (17), capability (49, `FMC1`, family 7, build tag) and arm (50), then the trigger `F0 22 24 35 7F F7` and loader preparation. Stage 2, in the vendor loader (identity `ota-FM-1_015`): it requests the image in blocks and writes `flash.bin`. If stage 2 is interrupted, the device comes back as the loader and the page resumes |
| In the replacement | `fm1_update_init` runs at boot. `fm1_update_message`/`busy`, the `update` task (priority 21, above `app_core` at 15) and `fm1_midi_input_sysex` are linked; update messages are handled before `fm1_app_sysex`. The SDK also contains a mask-ROM USB update entry (`platform_enter_mask_usb_update`, SysEx `F0 22 24 35 7D F7`), undocumented. (BUILD) |
| Package versus stock | boot area, identity `FM-1_8000014` and loader byte-identical; only `app.bin` differs. M-VAVE's `FM1Web.prepare` accepts the package (BUILD, HOST) |
| Workbench compatibility | identity `FM-1_8000014` (`openIdentity`), family 7, loader `ef1f2db8…`: the official workbench treats the device as Open (tagged update), so it can install official Open over CLATTER Pocket, and V15 through its stock path (SOURCE). HARDWARE |
| Bootloader | not modified (BUILD: the boot area is identical) |

Recovery cases:
1. **Backup.** Projects, samples and the DX7 library are backed up with M-VAVE's official
   workbench while official Open is installed. The replacement has no backup protocol.
2. **Install.** Use the generated installer page; FIRST-HARDWARE-TEST.md has the procedure.
   It works only from fm1_open firmware, never from V15 directly.
3. **Restore stock.** Use M-VAVE's official workbench (Open, or V15) over CLATTER Pocket.
   This needs CLATTER Pocket to boot far enough to run the SDK's USB-MIDI updater.
4. **If the application crashes:**
   - in the panel or engine code after boot: the updater task is independent and higher
     priority, and SAFE MODE skips the engine;
   - at a hardware fault: the device resets, possibly repeatedly;
   - during `groove_screen_init` (before the updater starts): the device cannot be reached.
     That code is now minimal, the same panel and LCD start-up as stock.
5. **If the UI fails.** The updater does not depend on the UI. (SOURCE, BUILD; HARDWARE)
6. **Bootloader recovery mode.** None is documented by M-VAVE. The mask-ROM USB entry
   exists in the SDK, but its tools and use are undocumented. **Not a recovery procedure.**
7. **Opening the device or programming hardware.** Unknown; fm1_open says recovery of a
   device that does not start needs vendor tools that are not public. A firmware that
   faults before the updater starts may need them.

## 4. Startup (BUILD: disassembly of the final ELF)

The boot sequence in `app_task_handler`:
1. platform initcalls;
2. `fm1_flash_init`;
3. the SDK's app init: `groove_midi_init`, audio reset, sequencer, clock, effects restore,
   `groove_edit_init`, `groove_samples_init`, `groove_project_init`, `groove_transfer_init`,
   then **`groove_screen_init`**;
4. **`fm1_update_init`**;
5. `fm1_io_start`;
6. `fm1_audio_start`;
7. a 10 ms tick.

The control poll calls the MIDI, samples, project and transfer polls, `groove_edit_poll`,
then **`groove_screen_control`**.

CLATTER Pocket's hooks:
- `groove_screen_init`: controls, LEDs, LCD, start screen;
- `groove_screen_control`: engine start at 1.5 s, Bluetooth configuration, MIDI drain,
  panel, status.

Tasks (SDK table):

| Task | Priority | Stack | Queue |
|---|---|---|---|
| `app_core` | 15 | 2048 | 128 |
| `update` | 21 | 384 | 32 |
| `btstack` / `btctrler` / `btencry` | 18 / 19 / 16 | 512 | |
| `sys_event` / `systimer` / `sys_timer` | 29 / 14 / 9 | 512 / 256 / 512 | |

CLATTER Pocket creates no tasks.

Handlers in the final ELF:
- `fm1_app_midi_received` is a strong `T` symbol called from the SDK's `fm1_route_short`;
  `fm1_app_sysex` is called from `fm1_midi_input_sysex`;
- the LTO workaround (no LTO for the application unit, as for the stock files it replaces)
  is effective;
- `clatter_pocket_render` and `clatter_pocket_output_level` are in SRAM and called from the
  audio interrupt and the output mix.

Concurrency:
- **Event ring.** Single producer (control task) and single consumer (audio interrupt),
  ordered with the SDK's `csync`. That the control task and the interrupt run on the same
  core is an assumption (HARDWARE).
- **MIDI ring.** The routing task writes, the control task reads.
- **Statistics.** Read with interrupts held.
- **Engine start.** `render_enabled` is set after a fence. Until then the interrupt writes
  silence.
- Nothing in the audio path blocks or allocates.

## 5. Audio path

| | |
|---|---|
| Format | 44.1 kHz (hardware 44,118 Hz), 128-frame blocks, interleaved stereo, signed Q23. The engine renders two 64-sample LXR blocks per FM-1 block. Q12 master, saturation, 5 Hz DC blocker. MASTER is applied in `fm1_output_mix` (0 to 127, ramped 8 steps per block), with USB and Bluetooth mixing and the 24-bit limit as stock. Overruns are counted by the SDK's audio interrupt. (SOURCE) |
| Precision | `--single-precision`: no software-double call in any of the 189 CLATTER Pocket functions of the final ELF. The only SRAM function calling soft-double is the stock `PSG_setClock`, which is unreachable (BUILD) |
| `sqrtf`, `fmaf` | `sqrtf`: see finding 1; called only at triggers (8 call sites). `fmaf`: the shim's definition, compiled in single precision as multiply-add (2 local SRAM copies, in `calcOnePole` and `SVF_calcBlockCheapZDF`) (BUILD). Host: `sqrtf` is bit-exact to libm; `fmaf` equals the shim's definition (HOST) |
| Tables | unpacked once on the control task before `render_enabled`, into static arrays of the exact firmware sizes. A size or consistency mismatch is reported (ENGINE ERROR) instead of rendering. No unpacking in the interrupt. No allocation (HOST: byte-identical; BUILD: no allocator calls) |
| Synthesis | bit-identical voices with packed tables (HOST); matches CLATTER within −80.8 dBFS in double precision and −78.3 dBFS in single (HOST, earlier measurement) |

## 6. Real-time performance

**No hardware measurement exists.** What is known:

- The engine's cost is **fixed per block**: all six voices are computed every block, as on
  the LXR-02. On the host: 21.4 µs per 128-frame block, 0.74 % of an Apple M3 core (HOST).
- The same code is the LXR-02's own sound engine, which runs alongside its sequencer, UI and
  FX on a 216 MHz Cortex-M7 with a double-precision FPU.
- The FM-1: PI32v2 r3, reported up to 320 MHz (unverified), with a **single-precision FPU
  (add, multiply, divide; no square root or fma)**. The DSP is in single precision with no
  software-double calls, and square roots run only at triggers.
- The most expensive paths, by code and frame size: the oscillators (`calcNextOscSampleBlock`
  and the FM/PWM variants), the voice blocks (`calcDrumVoiceSyncBlock`, `Snare_`, `Cymbal_`,
  `HiHat_calcSyncBlock`), the SVF (`SVF_calcBlockCheapZDF`) and the envelopes
  (`slopeEg2_calc`), all in SRAM.
- **Estimate (not a measurement):** feasible, but the margin is unknown until measured. The
  panel shows the audio CPU share per block (mean and peak), DMA overruns, clipping and
  dropped events. That is the first number to read on the device.
- Worst-case stack (BUILD, call graph from the ELF):

  | Entry point | Bytes |
  |---|---|
  | audio interrupt, via the render path | 400 |
  | `groove_screen_control`, including engine start | 720 |
  | `groove_screen_init` | 580 |
  | MIDI routing hook | 208 |

  None makes indirect calls. The `app_core` stack is 2048 units, and the stock 808 renderer
  alone has a 516-byte frame.
- **Not available:** an active-voice count (the engine has no cheap one) and the runtime
  free heap (the SDK has no API for it). The heap is checked at link time.

## 7. Memory

| | |
|---|---|
| Flash | image 421,212 + 28,614 layout padding: `flash.bin` is 471,040, as stock (BUILD) |
| SRAM | `.ram0_data` 104,924: LXR code and tables needed in the interrupt, voice state, SDK code. `.ram0_bss` 284,492: 102,238 unpacked tables, 7,680 panel band buffer, 4,064 project scratch, engine state, SDK buffers. Heap 134,124, 84,972 above the SDK minimum (BUILD) |
| Allocation | none by CLATTER Pocket; Bluetooth, USB and RTOS allocate from the heap as in stock (BUILD) |
| Buffers | event ring 256 × 16 bytes, MIDI ring 64; bounded drains (32 events per block); full rings drop and count (SOURCE, HOST) |

## 8. Controls and MIDI

| | |
|---|---|
| Keys, PLAY | trigger the selected voice: keys chromatically from note 48, PLAY at 60 (HOST: panel). Since this audit PLAY is the sequencer's transport (docs/POCKET-SEQUENCER.md) |
| Encoders | 1 to 3 set pitch, decay and level of the selected voice; 7 selects the voice (HOST). Physical labels unverified (HARDWARE) |
| MASTER | ADC 0 to 1023 maps to level 0 to 127 with a 4-count dead band (SOURCE) |
| HOME | at the 1.5 s point, SAFE MODE; otherwise unused. HOME+ARP does nothing in replacement mode (SOURCE) |
| MIDI | General MIDI drum map, any channel. Note-on with velocity 1 to 127 triggers at the programmed pitch. Note-off, velocity 0, other messages, SysEx and malformed or short input are ignored (HOST: exhaustive). A full ring drops (counted). MIDI never touches the engine from the routing task |
| Note-off and stuck notes | the LXR voices are one-shot (decay envelopes), so note-offs are not needed. Decay 127 sustains by design (ARCHITECTURE.md) |
| USB, BLE, DIN | all arrive through the SDK's router at `fm1_app_midi_received` (BUILD); `fm1_io_trs_midi_enabled` returns 1 (SOURCE). Each port is HARDWARE |

## 9. Diagnostics

The start screen, engine states, SAFE MODE, MIDI activity, level meter, CPU, overrun,
clipping and drop counters, and the build identity are described in FM1-REPLACEMENT.md.
They use only the panel and LCD calls the stock UI used (`fm1_screen_blit`,
`fm1_controls_read`, `fm1_led_set`), with no new graphics assets.
- HOST: every band in every state at extreme values stays inside its buffer; the state and
  MIDI lines change.
- HARDWARE: how the screen actually looks and responds.

## 10. SDK checks: exact changes

The derived `Test-Firmware` differs from fm1_open's in these places only (the generator's
edit groups 3, 6, 8 and 9):

1. `llvm-nm` runs through Wine (tooling only).
2. Replacement mode only: a named list of stock-instrument symbols may be absent. The list
   is:
   - the `synth.json` SRAM contract;
   - the engine's own SRAM list (`$extra`);
   - `fm1_engine_render` and `fm1_engine_output_level`;
   - the `groove_sequence_*` entry points;
   - the SEA decoders;
   - the tuning and project hooks;
   - the two "instrument ready" hash symbols.

   Any of these that are present are still checked for SRAM placement. Absent in this
   build: `fm1_master_gain`, `groove_project_apply_edit`, `groove_tuning_apply`.
3. CLATTER Pocket's SRAM requirements are added: hooks, render path, LXR per-sample code,
   voice state, tables, `sqrtf`.
4. Report fields.
5. Packaging: the stock-layout assertion.

**Unchanged:**
- ELF and entry rules, section whitelist, registration tables;
- every required platform symbol (update, flash runtime, Bluetooth discovery, BLE API, USB
  audio and MIDI);
- forbidden runtimes, members and inputs;
- RTOS ABI, ACL pools, heap minimum;
- platform audio path and panel scan in SRAM;
- the `fm1_io_trs_midi_enabled` and `fm1_app_sysex` callback rules;
- build-identity sizes;
- the packager.

## 11. Remaining risks (HARDWARE)

1. **The firmware has never run.** Boot, LCD, panel scan, audio and MIDI are unproven.
2. **Recovery depends on the firmware booting far enough to start the SDK updater.** A
   fault before `fm1_update_init`, in CLATTER Pocket's minimal `groove_screen_init` or in
   the unchanged SDK, would need M-VAVE's unpublished tools.
3. **The installer page** drives M-VAVE's updater code unchanged, but this combination has
   not run against a device.
4. **CPU load of the LXR engine on PI32v2** is unknown, as is the core and interrupt
   arrangement (the event ring's barrier).
5. **VM contents** across the change, Bluetooth pairings in particular.
6. **Physical control labels**; the HOME button's index comes from fm1_open's button order.
7. **Output level and DC behaviour** of the analogue stage; the DAC part is unknown.
