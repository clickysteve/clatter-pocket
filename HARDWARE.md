# FM-1 hardware and SDK

What is known about the M-VAVE FM-1 as a target for CLATTER Pocket, and how it is known.
**Verified** means read from the official SDK's source, headers, linker script or binaries
(`third_party/fm1_open`, commit `4c7496d`, 2026-10-21). **Reported** means from product pages,
reviews or secondary sources. **Unknown** means nobody here has checked it yet. No physical
FM-1 was used.

## The SDK

| | |
|---|---|
| Repository | <https://github.com/m-vave/fm1_open>, owned by the `m-vave` GitHub organisation (M-VAVE, 珠海笙科智能科技有限公司); created 2026-10-13, active (last push 2026-10-21), not archived |
| Release | "FM1 Open V14" (`app/version.h`: `V14`, update product `FM-1_8000014`) |
| Licence | Application sources: GPL-3.0-only with a linking exception for the binary SDK. Engine library `sdk/lib/libfm1_engine.a`: proprietary, binary only, may be used and redistributed for FM-1 firmware. Boot/update resources and the original V15 recovery image: Apache-2.0 (`LICENSE.md`, `THIRD_PARTY.md`) |
| What is source | The application: UI (`app/screen.c`), MIDI routing, projects, samples, transfer, **the board audio driver** (`app/board/audio.c`), panel scanning, LCD wiring |
| What is binary | The instrument engine (DX7-style FM, 303, 808, sampler, sequencer), FreeRTOS, Bluetooth/USB stacks, Flash driver, startup (`libfm1_engine.a`: one ELF32 object, machine 0xF1 = PI32v2, 165 global `fm1_*`/`groove_*` symbols) |
| Toolchain | **JieLi Windows toolchain 2.5.3** (PI32v2 clang 4.0.1, `-target pi32v2 -mcpu=r3 -mfprev1`), Windows only; 22 toolchain files are pinned by SHA-256 in `sdk/manifest.json` |
| Build | `sdk/Build.ps1` (PowerShell) compiles `app/`, links with the SDK, runs firmware checks (registration tables, SRAM placement of every audio callback, RTOS ABI, heap ≥ 48 KB, allowed sections), packages with `sdk/Package.cs` |
| Simulator | A WebAssembly build of the application and open engine parts (`web/`, `tools/Build-Web.ps1`, WASI SDK) |

Other FM-1 firmware projects: there are about 70, listed by the FM-1 Firmware Hub and fm1-emu;
[docs/FM1-COMMUNITY.md](docs/FM1-COMMUNITY.md) has how they are distributed and recovered. (An earlier
version of this document said none was found.)

## Processor and memory

| | | Source |
|---|---|---|
| MCU | **JieLi AC791N** ("WL82" family) | Verified: `app/board/fm1_input.h` ("AC791N SPI2 setup"), `fm1_audio.h` ("WL82 ALNK0"), library names in the linker script (`lib_wl82_rf_version`); SDK notices cite JieLi's AC79 SDK |
| CPU core | PI32v2, `r3`, with a **single-precision** FPU (`-mfprev1`): float add, multiply, divide; **no double precision, no square root, no fused multiply-add** | Verified 2026-10-29 with the SDK's toolchain: `double` arithmetic compiles to compiler-rt calls (`__muldf3`, ...), `sqrtf`/`fmaf` to library calls the SDK's libc does not provide |
| Cores | Dual-core | Reported (secondary source, below); which core runs what in the FM-1 is **unknown** |
| Clock | Up to 320 MHz | Reported, not verified; the firmware reads it at run time (`fm1_device_clock()`) |
| Internal SRAM | RAM0 0x01C00000–0x01C7FE00 = **523 776 bytes** for the firmware, plus 28 KB of data cache usable as RAM (`cache_ram`, 7 ways × 4 KB) | Verified (`sdk/config/linker.ld.in`). A secondary source gives 578 KB in total |
| External RAM | None used (`SDRAM_SIZE = 0`) | Verified |
| Code Flash | Executed in place from 0x02000120. The 1 MiB update image (`0xff000` usable) holds the app image, at least 32 KB of VM and the fixed user area, which accepts an app image of at most **453 921 bytes** (`Package.Create`); stock fm1_open V14 uses 453 576 (345 bytes free); CLATTER Pocket replacing the stock application uses 421 212, padded in the package to the stock layout (FM1-REPLACEMENT.md). The packager keeps the user partition (507 904-1 044 480) and BTIF (503 808) fixed and sizes the VM between image and BTIF | Verified (`sdk/Package.cs`, package reports 2026-10-29) |
| User Flash | 536 576 bytes (131 × 4 KB sectors), API `fm1_flash_*`; the stock instrument already stores projects, samples and voices there | Verified (`Build.ps1` variant, `fm1.h`) |
| Runtime heap | Must stay ≥ 49 152 bytes; RTOS, Bluetooth and USB allocate from it | Verified (`Test-Firmware`) |

Audio code must run from internal SRAM: "Audio code and every callee must be safe during
Flash programming" (`fm1.h`); `Test-Firmware` rejects any build whose audio callbacks are not
in 0x01C00000–0x01C80000.

## Audio

| | | Source |
|---|---|---|
| Format | 44 100 Hz nominal (hardware 44 118 Hz, 11.2896 MHz × 256 fs clocking), stereo, signed 24-bit (Q23 in int32) | Verified (`fm1_block.h`, `fm1_audio.h`) |
| Block | **128 frames** per interrupt (2.902 ms), two DMA halves | Verified |
| Output path | AC791N audio link ALNK0 (I2S-style; PC0 MCLK, PC1 SCLK, PC2 LRCLK, PC6 data) to an external DAC; no analogue input connected | Verified (`fm1_audio.h`). **The DAC part is unknown** |
| Interrupt | IRQ 11, priority 4; renders the instrument (`fm1_engine_render`), mixes USB/Bluetooth audio and master gain (`fm1_output_mix`), copies to the free DMA half; counts overruns and peak time | Verified (`app/board/audio.c`) |
| Latency | One block of rendering plus the DMA half: about 2.9-5.8 ms of buffering in the firmware; DAC and output stage unknown | Derived, not measured |
| USB audio | USB playback into the mix and recording of the final output, Q23 | Verified (`fm1_audio_io.h`) |
| Bluetooth audio | A2DP sink mixed in, 16-bit | Verified |
| Output jack | 3.5 mm stereo | Reported |

## Panel

| | | Source |
|---|---|---|
| Display | **240 × 240**, RGB565, SPI with DMA in bands of 240 × 16 pixels | Verified (`fm1.h`) |
| Display type | Colour TFT | Reported (Synthtopia) |
| Note keys | **27** (F3-G5), with LEDs | Verified (`fm1.h`: bits 0..26) |
| Buttons | **14**: OCT−, OCT+, FX, SCL, ENV, LFO, EDIT, GLO, HOME, SAVE, ARP, SEQ, PLAY, REC, with LEDs | Verified (`app/screen.c` enum, `fm1_led_set`) |
| Encoders | **7**, relative (detents) | Verified (`fm1.h`) |
| Pot | 1 (MASTER), 10-bit ADC | Verified |
| Battery | ADC reading available; 2000 mAh, about 12 h | Verified (ADC) / Reported (capacity) |
| Touch | None | Verified (no touch API or driver) |
| Velocity | **Unknown.** The scanner reports on/off only, so keys are treated as unaccented | Verified (scanner) |

Encoder positions, from fm1-emu's panel (after Felucca's `controls.jpg`) and fm1_open's use of
them, not checked on a device: 0 SELECT, 1 ALGORITHM, 2-5 KNOB 1-4, 6 PRESETS.

## MIDI, USB, storage

- MIDI in: DIN/TRS (UART), USB, BLE; out: USB, BLE (`fm1.h`). Verified.
- USB-C: MIDI, audio, power/charging. Verified (API) / Reported (connector).
- Storage: the user Flash partition only; no SD card, no file system for samples (`Build.ps1`
  rejects FAT runtimes). Samples: 8 slots sharing about 19.55 s, SEA-encoded (`fm1_sea.h`).

## Updating, recovery, backup

**Update mechanism (verified, `sdk/WebUpdate.js`):** the browser workbench talks to the
*running* firmware over **USB-MIDI SysEx** (manufacturer 00 59 / F0 22 24 35 ...). The firmware
answers an identity query, is told to start the vendor OTA loader (`ota.bin`, identity
`ota-...`), re-enumerates as the loader, and the loader writes the image block by block, then
the device restarts and reports its identity again. The SDK handles these messages before the
application's SysEx hook. The package format is `.fwsc` (`Package.cs`); the bootloader
(`uboot.boot`) and configuration resources are fixed SDK files.

**What can be restored (verified):** the SDK ships the original **M-VAVE V15** firmware
(`sdk/firmware/FM-1_015.fwsc`, SHA-256 `db1642b2...`, hash-pinned) and the workbench has
"Restore original V15" and "Switch to Official Open"; both require the device to respond on
USB-MIDI. The official workbench **does not accept external firmware files**.

**What cannot be relied on:** the workbench texts say force flashing "may ... prevent startup
and require a recovery tool", and "Firmware without the USB-MIDI update protocol requires its
matching recovery tool". **No such tool is published in fm1_open.** JieLi parts generally have
a ROM USB download mode used by JieLi's own tools, and the SDK cites kagaimiq's jl-misctools for
its packager, but whether and how the FM-1 enters a ROM recovery mode is **unknown** and
untested. Treat a firmware that does not start, or that breaks USB-MIDI, as possibly
unrecoverable without vendor help. **Reported, 2026-10-30, not tried here:** community tools write
the firmware area through the chip's ROM bootloader ([docs/FM1-COMMUNITY.md](docs/FM1-COMMUNITY.md#recovery-in-the-community)).

**Backup:** the update protocol has no firmware read-back. The original firmware is
available as the SDK's V15 image; the Open release is available from the workbench. User
data (projects, voices, sample bank) can be exported from the workbench and must be backed up
before any update: migration can erase it.

### Rules this project follows

1. Nothing is flashed without the owner's explicit approval of that specific build.
2. CLATTER Pocket builds keep the stock application, its USB-MIDI update path and the
   bootloader/configuration untouched: CLATTER Pocket is added beside the instrument, starts
   only on a deliberate gesture (HOME + ARP) and is never active at power-on. A fault inside
   CLATTER Pocket can then be escaped by power-cycling, and the device can still be updated.
   Do not make it start automatically until it has been validated on hardware.
3. The SDK stays byte-identical (checked by the build and by `firmware/fm1/preflight.sh`).
4. Before the first flash: identify the unit (hardware revision, current firmware identity
   as the workbench reports it), export projects/voices/samples, confirm the workbench can
   restore V15 and Open on that unit, and only then try a CLATTER Pocket build.

## Open questions (need the device or the vendor)

1. Hardware revision(s) in the field and whether fm1_open supports all of them.
2. ~~Does the FPU execute double precision?~~ **No** (answered with the toolchain,
   2026-10-29; see Processor and memory). The FM-1 build uses `--single-precision`.
3. Does `__attribute__((interrupt("")))` on PI32v2 save **FPU registers**? CLATTER Pocket
   runs float DSP in the audio interrupt and float code in the control task. (The SDK's own
   float kernels in the same interrupt suggest yes.)
4. Do the control task and the audio interrupt run on the **same core**? (The event ring's
   barrier assumes so: `firmware/fm1/pocket_fm1_config.h`.)
5. CPU clock, cache sizes and the real cost of the stock engine per block.
6. The DAC/codec part and whether the output is AC-coupled.
7. Physical labels of encoders 0-6 (reported by fm1-emu; not checked on a device).
8. Whether every Flash write (including the Bluetooth stack's) suspends audio. `fm1_flash_write`
   and `fm1_flash_erase` call `fm1_audio_suspend` and `_resume` (linked ELF); other paths are not
   proven. CLATTER Pocket's audio path no longer depends on it: `tools/check-audio-path.py`.
9. A ROM-level recovery procedure. The SDK contains a mask-ROM USB update entry
   (`platform_enter_mask_usb_update`, reached by SysEx `F0 22 24 35 7D F7` from running
   firmware), but M-VAVE documents no tool or procedure for it (PRE-FLASH-AUDIT.md). The community
   reports one ([docs/FM1-COMMUNITY.md](docs/FM1-COMMUNITY.md#recovery-in-the-community)); untried.
   Firmware updates run in the SDK's own `update` task (priority 21, above the application's
   15), and CLATTER Pocket starts its engine only after the updater (SAFE MODE: hold HOME).

## Sources

- SDK: <https://github.com/m-vave/fm1_open> (`README.md`, `LICENSE.md`, `THIRD_PARTY.md`,
  `sdk/include/*.h`, `sdk/config/linker.ld.in`, `sdk/Build.ps1`, `sdk/WebUpdate.js`,
  `sdk/I18n.js`, `app/board/*`, `app/screen.c`)
- AC791N (dual-core FPU DSP, up to 320 MHz, 578 KB SRAM; secondary source):
  [GitCode/CSDN AC791N vs AC792N comparison](https://gitcode.csdn.net/69e9857554b52172bc6bb671.html);
  WL82 naming: [21ic](https://passport2.21ic.com/icview-3458290-1-1.html); related part
  [AC7911B datasheet (LCSC)](https://atta.szlcsc.com/upload/public/pdf/source/20241209/7B2C0C261932781E1CA5530337098F4C.pdf)
- Product: [Synthtopia](https://www.synthtopia.com/content/2026/07/13/m-vave-introduces-fm-1-fm-pocket-synthesizer/),
  [Synth Anatomy review](https://synthanatomy.com/2026/07/m-vave-fm-1-review-low-budget-pocket-fm-ynthesizer-with-iconic-sounds.html),
  [Elektronauts](https://www.elektronauts.com/t/m-vave-fm-1/252170),
  [user manual](https://m.media-amazon.com/images/I/A1WOydif9HL.pdf)
