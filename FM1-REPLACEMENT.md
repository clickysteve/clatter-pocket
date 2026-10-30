# CLATTER Pocket as the FM-1's instrument

`firmware/fm1/build.sh --replace --single-precision` builds a firmware in which CLATTER
Pocket **replaces fm1_open's stock instrument application** and keeps the SDK's
infrastructure:
- RTOS and drivers;
- audio DMA, panel and LCD;
- USB (MIDI and audio) and Bluetooth (MIDI and audio);
- the firmware update protocol;
- the flash layout, **exactly as stock fm1_open V14**.

It produces a valid package, `build/fm1-replace/clatter-pocket-fm1-replace-UNVERIFIED.fwsc`,
which passes all of the SDK's firmware checks; the replacement-mode adjustments are listed
below. `python3 tools/make-installer-page.py` makes an installer page for it.

| | |
|---|---|
| Package | `clatter-pocket-fm1-replace-UNVERIFIED.fwsc`, 492,084 bytes |
| Package SHA-256 | `51afea18e752b71161e0e743358868481791c76912bf892a338019947a902b4b` |
| ELF SHA-256 | `ba086e868bc5ee06104643b23110ef866210f3cdcb3a8591bc922bccd002ba3b` |
| Built from | fm1_open 4c7496d, JieLi 2.5.3, `--replace --single-precision`. The flag is required: without it the build succeeds and the engine runs on software doubles, silent at 183 % CPU in the emulator; `tools/check-firmware.py` now refuses that |
| Reproducible | two builds from an empty `build/` gave identical bytes |
| Checks | `python3 tools/check-firmware.py`; `make emu-test` runs it in the emulator |

**Status, 2026-10-30: built and packaged; runs in the FM-1 emulator; not flashed; not run on
hardware.** Everything below comes from the link map, the ELF, the SDK's firmware report and
packager, host tests and the emulator. None of it comes from a device. docs/POCKET-UI.md has
the interface, and why an early emulator run appeared to show the stock interface;
docs/POCKET-SEQUENCER.md has the sequencer. PRE-FLASH-AUDIT.md has the full audit, of commit `b3092fe`;
FIRST-HARDWARE-TEST.md has the test procedure.

## The flash budget

`Package.Create` (fm1_open `sdk/Package.cs`) accepts an application image of at most:

0xff000 − 536,576 (user) − 4,096 (BTIF) − 32,768 (minimum VM) − 16,384 (boot area) − 288 − 383 − 64 = **453,921 bytes**

The first report gave 479,232: that figure left out the boot area, BTIF and the directory.

| Approach | App image | vs the 453,921 limit |
|---|---|---|
| Stock fm1_open V14 | 453,576 | 345 free |
| Beside stock, tables as compiled | 586,920 | 132,999 over |
| Beside stock, tables packed | 507,592 | 53,671 over |
| Replacing the stock application, tables packed, `-Oz` control code (first interface) | 421,212 | 32,709 free |
| The same with the page interface (docs/POCKET-UI.md) | 423,100 | 30,821 free |
| The same with the sequencer (docs/POCKET-SEQUENCER.md) | 430,908 | 23,013 free |
| **The same with keyboards, recording and kits (docs/POCKET-CONTROLS.md)** | **436,860** | **17,061 free** |

The SDK's binary library (`libfm1_engine.a`) is a single pre-linked object. `--gc-sections`
removes what nothing references any more, but the library still references its own
instrument internals, so about 40.6 KB of stock instrument code and data stay in the image,
unused and unreachable from CLATTER Pocket:

| Stock code still linked | Bytes |
|---|---|
| sequencer, arpeggiator, pads, clock and project core (`groove_*`) | 15,782 |
| DX7/FM | 6,626 |
| 303 bass | 5,824 |
| 808, sample drums and SEA codec | 5,084 |
| PSG | 4,560 |
| pluck | 2,390 |
| effects and mix bus | 368 |

Removing it would require changing the binary SDK, which this project does not do.

## Flash usage (`python3 tools/flash-map.py <sdk.map>`)

| Flash bytes, from the link map | Stock | Replacement |
|---|---|---|
| SDK library | 375,348 | 358,071 |
| (Figures in this table are from the first interface, image 421,212.) fm1_open application. Stock: all of `app/`. Replacement: what is kept, the board layer, `audio_output.c` and the build identity | 73,570 | 4,563 |
| CLATTER Pocket application unit | | 1,219 |
| CLATTER Pocket engine (LXR-02 DSP, parameter handler, engine host, panel, MIDI, unpacker, `sqrtf`) | | 28,871 |
| LXR tables, packed (from 102,238) | | 23,892 |
| libc, compiler-rt | 4,538 | 4,452 |
| **App image** (`.text` + `.ram0_data` + `.cache_ram_data`) | **453,576** | **421,212** |

## Flash layout: identical to stock

`Package.Create` places the VM, the system settings store, directly after the image,
rounded to 4 KiB. The VM holds Bluetooth paired devices
(`get_remote_device_info_from_vm`) and syscfg items, and the SDK validates its area by ID
(`vm_check_id`, `vm_area_check`, `vm_reset`, `_vm_area_erase`). Left alone, a smaller
image would move the VM's start into flash the previous firmware used for code. So
CLATTER Pocket builds pad the packaged application with erased-flash bytes (`0xFF`,
after the last section; never executed or copied), and the build fails unless the result
is the stock layout. `tools/fwsc-layout.py` decodes the records of both packages:

| Record | fm1_open V14 stock | CLATTER Pocket | M-VAVE V15 (SDK recovery image) |
|---|---|---|---|
| `flash.bin` written by an update | 0 to 471,040 | 0 to 471,040 | 0 to 602,112 |
| VM | 471,040, 32,768 bytes | 471,040, 32,768 bytes | 602,112, 352,256 bytes |
| BTIF | 503,808, 4,096 | 503,808, 4,096 | 954,368, 4,096 |
| USER | 507,904, 536,576 | 507,904, 536,576 | (no user record) |
| `key_mac` | 1,044,480, 4,096 | 1,044,480, 4,096 | 1,044,480, 4,096 |

The boot area (0 to 0x4000: uboot, boot configuration), the update identity
(`FM-1_8000014`) and the loader (`ota.bin`, the vendor loader `ef1f2db8…`) are
byte-identical to the stock package. Only `app.bin` differs. Switching between M-VAVE
V15 and fm1_open Open already moves VM and BTIF; switching between Open V14 and CLATTER
Pocket does not.

## What was removed

All of fm1_open's instrument application:
- the UI: tracks, steps, tones, FM operators, LFOs, effects, mixer, projects, presets,
  MIDI, Bluetooth and settings pages;
- project, preset and sample storage;
- the DX7 voice library and factory voices;
- the workbench transfer protocol for projects, samples and voices;
- MIDI routing, Thru and clock settings;
- tuning;
- step recording and editing.

`docs/fm1-replace-removed.txt` lists every flash symbol of the stock image that is absent
from the replacement, by input file, with sizes, 60,831 bytes in total. It is generated by
`tools/flash-map.py --diff` from the two link maps. It includes:
- 17,426 bytes of SDK library functions that only the stock application called;
- **`fm1_flash_read`, `fm1_flash_write`, `fm1_flash_erase`, `fm1_flash_map` and
  `fm1_flash_invalidate`**. The replacement contains no code that can read or write the
  user partition.

## What the replacement keeps

| Hardware or function | State |
|---|---|
| Audio output (I2S DMA, 44.1 kHz, 128-frame blocks, Q23 stereo) | kept (`app/board/audio.c`); CLATTER Pocket renders the instrument |
| MASTER knob | output level 0 to 127 with the stock ramp (`audio_output.c`) |
| USB audio in (mixed) and out (recording); Bluetooth audio in (mixed) | kept (`audio_output.c`); Bluetooth enabled at start-up like stock (on, music and MIDI, BLE Peripheral) |
| MIDI in over USB, BLE and DIN/TRS | kept; General MIDI drum notes on any channel trigger the seven LXR voices at their programmed pitch and velocity |
| MIDI out | linked (SDK), unused |
| BLE roles, scan and connect | Peripheral at start-up; the rest of the API is linked (the SDK check requires it), with no page yet |
| 27 keys, 14 buttons, 7 encoders, LCD | kept (`app/board/panel.c`): the CLATTER Pocket interface (docs/POCKET-UI.md) |
| LEDs | switched off |
| **Firmware update over USB-MIDI** | kept, in the SDK, untouched: handled before application SysEx, in the higher-priority `update` task (PRE-FLASH-AUDIT.md) |
| User partition | **untouched**; readable again after reinstalling stock |
| Workbench project, sample and voice backup | **not available** while CLATTER Pocket is installed: back up first |
| Stock instrument, sequencer, arpeggiator, effects | not available (code partly present, unreachable); CLATTER Pocket has its own sequencer |

## Start-up, safe mode, diagnostics

The SDK calls `groove_screen_init` before it starts the firmware updater, USB and audio
(the order is read from the ELF in PRE-FLASH-AUDIT.md). So `groove_screen_init` only
prepares the panel and draws the start screen, which is the same work as stock. The LXR
engine starts in the control loop **1.5 s later**, after the updater, USB and audio are
running.

- **SAFE MODE:** if HOME is held at that moment, the engine never starts. The screen says
  SAFE MODE, the output is silent, and the SDK, USB-MIDI updater and Bluetooth run
  normally. This is a way to reach the updater if the engine misbehaves.
- **ENGINE ERROR:** if the packed tables do not unpack, the engine is not started and the
  output stays silent.

Until the engine runs, and in SAFE MODE and on an engine error, the screen is the start
screen: CLATTER POCKET, LXR DRUM MACHINE, the state, the version and the build identity (the
first 8 hex digits of `fm1_build_id`). After that it is the interface in docs/POCKET-UI.md.

`-DPOCKET_DIAGNOSTICS=0` removes the per-block timing from the audio interrupt.

## SDK firmware checks in replacement mode

Generated by edit groups 8 and 9 of `tools/derive-fm1-build.py`; PRE-FLASH-AUDIT.md lists
the exact diff. Every platform check is kept:
- ELF entry and section rules, registration tables, RTOS ABI;
- the Bluetooth, BLE, USB MIDI, USB audio, update and flash-runtime symbols;
- forbidden runtimes and inputs;
- the heap minimum;
- the platform audio path and panel scan in SRAM.

CLATTER Pocket's audio path, voice state, tables and `sqrtf` must be in SRAM. Stock
instrument symbols (the `synth.json` contract, the engine's entry points, the tuning and
project hooks) may be absent. Any that are present are still checked; the absent ones are
reported: `fm1_master_gain`, `groove_project_apply_edit` and `groove_tuning_apply`. The
package must keep the stock layout.

## RAM

| | Stock | Replacement |
|---|---|---|
| `.ram0_data` (SRAM code and initialised data) | 80,968 | 105,756 |
| `.ram0_bss` | 195,232 | 284,876 (of which 102,238 are the unpacked LXR tables) |
| Runtime heap (SDK minimum 49,152) | 247,340 | **132,908** (83,756 above the minimum) |

CLATTER Pocket allocates nothing at runtime: no allocator call in its code (checked in the
ELF). The heap is left to the SDK's RTOS, Bluetooth and USB, which the 49,152-byte minimum
is for.

## The LXR tables

The LXR firmware's tables are 102,238 bytes:
- the sine table;
- the triangle, saw and square wavetables, 11 × 1024 each;
- the twelve transient samples.

They are stored losslessly packed, in 23,812 bytes:
- a second-order predictor for the waveforms and a first-order one for the transients;
- Rice coding in blocks of 64.

The packed data and the unpacker stay in flash. `pocket_lxr_init` unpacks the tables into
RAM, under their firmware names, once, on the control task, before rendering is enabled.
Nothing unpacks in the audio interrupt. `make test` checks:
- the unpacked tables are **byte-identical** to the compiled firmware tables;
- all 48 engine checks pass on the packed engine;
- the CLATTER comparison script renders **bit-identical** voices.

## Compiler settings

- **Control-task-only code** stays in flash and is compiled with `-Oz`: the CLATTER Pocket
  application unit, the panel, the pattern generator, MIDI parsing and the table unpacker.
- **The LXR DSP** and the sequencer's clock keep `-O2` and stay in SRAM: they run in the audio
  interrupt.
- **Single precision** (`--single-precision`): no CLATTER Pocket function calls a software
  double routine (checked in the ELF).
- **`sqrtf`:** the shim's wrappers are renamed so `sqrtf` resolves to a real,
  correctly rounded implementation (PRE-FLASH-AUDIT.md, finding 1).
