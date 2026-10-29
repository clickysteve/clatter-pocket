# Building CLATTER Pocket

Two builds: the **host** build (the engine, its tests and tools; any Mac or Linux machine
with a C compiler) and the **FM-1 firmware** build (needs the JieLi toolchain).

```
git clone --recurse-submodules https://github.com/clickysteve/clatter-pocket
cd clatter-pocket
make            # host tools + all tests
```

## Host build

Requirements: a C11 compiler (Apple clang 21 and LLVM 21 checked), make, python3. Optional:
node 22+ (comparison with CLATTER), LLVM with the ARM target (`tools/size-estimate.sh`,
`firmware/fm1/preflight.sh`; `brew install llvm`).

| Command | What it does |
|---|---|
| `make` / `make test` | builds and runs `test_engine` (full LXR engine, as CLATTER), `test_engine_nofx` (FX bus off, as the FM-1 build) and `test_panel` (the Pocket panel, writes `build/host/panel.ppm`) |
| `make bench` | host time per FM-1 block (128 frames) in five load scenarios |
| `make render` | `build/host/kick_sweep.wav`, `build/host/kit_demo.wav` (24-bit) |
| `make compare CLATTER_DIR=../clatter` | runs `host/compare/script.txt` on CLATTER's committed `lxr.wasm` and on this engine; compares every voice block by block |
| `make compare-sp CLATTER_DIR=...` | the same with the DSP's double arithmetic in single precision |
| `tools/size-estimate.sh` | footprint and soft-float report of the FM-1 configuration on a Cortex-M4F stand-in (`SP=1` for single precision) |
| `python3 tools/gen-param-header.py` | regenerate `engine/pocket_params.h` from the firmware's `Parameters.h` |

On macOS the firmware's ELF section attributes are mapped to `annotate` (`Makefile`).

## FM-1 firmware build

**Status (2026-10-29):** the toolchain is installed and verified; the unmodified fm1_open
builds and packages; CLATTER Pocket compiles, links and passes all of the SDK's firmware
checks, but **does not fit the FM-1's flash layout**, so no CLATTER Pocket package exists.
See "First build results" below.

### Requirements

- **JieLi Windows toolchain 2.5.3** (PI32v2 clang 4.0.1). The SDK checks 22 of its files by
  SHA-256 and refuses any other version.
- **PowerShell 7** (`pwsh`). The SDK's build and packager are PowerShell/C#. macOS:
  `brew install --cask powershell`, or Microsoft's release tarball (7.6.9 osx-arm64, checked
  against the release's `hashes.sha256`) unpacked anywhere; set `PWSH` to its `pwsh`.
- On macOS/Linux, **Wine** to run the Windows toolchain (Wine 10.0 on macOS checked).
- The fm1_open submodule at `4c7496d` (`git submodule update --init`).

### Toolchain

```bash
tools/setup-toolchain.sh
```

It downloads JieLi's installer (51 MiB, `jl-update.oss-cn-shenzhen.aliyuncs.com/2.5.3.exe`,
the URL fm1_open's README gives), checks its SHA-256 (`399bd518...c5a5e`, recorded
2026-10-27), runs it silently in a separate Wine prefix (`~/.wine-jieli`, `C:\JL`) and checks
every toolchain file against the SDK manifest. It then prints the `FM1_PI32_TOOLCHAIN` line to
export. On Windows: install 2.5.3 from the same URL and set `FM1_PI32_TOOLCHAIN=C:\JL\pi32`.

The installer is in Chinese and is written for JieLi's Code::Blocks integration: its setup
script refuses to install unless Code::Blocks is in the registry and has been run once, and it
shows message boxes that the silent-install options do not suppress. The setup script
therefore adds a placeholder Code::Blocks registry entry and an empty Code::Blocks
configuration **in the separate Wine prefix only**, and runs the installer's wizard: click OK
on the notices and go through the wizard with the defaults (folder `C:\JL`). The compiler does
not need Code::Blocks.

### Build

```bash
firmware/fm1/build.sh --stock          # the unmodified fm1_open, same toolchain and steps
firmware/fm1/build.sh --prepare-only   # build/fm1/tree: fm1_open + patch + CLATTER Pocket sources
firmware/fm1/preflight.sh              # SDK manifest + stand-in compile of every touched file
firmware/fm1/build.sh --single-precision   # the real build (toolchain, pwsh, Wine)
```

`--stock` is the known-good comparison: every CLATTER Pocket edit of the derived build
script is off, the tree is unpatched, output in `build/fm1-stock/`.

Options: `--placement sram` (default: all CLATTER Pocket code and tables in SRAM) or
`--placement tcm` (only the LXR's own TCM-marked code and state in SRAM, the rest in Flash;
smaller, relies on the SDK muting audio for all Flash writes), `--single-precision` (the DSP's
double arithmetic in float; see ARCHITECTURE.md).

What `build.sh` does:

1. extracts the pinned fm1_open into `build/fm1/tree`, applies `firmware/fm1/fm1_open.patch`
   (two hooks: `app/board/audio.c`, `app/screen.c`) and copies the adapter
   (`firmware/fm1/app/clatter_pocket/`) and exactly the engine files the firmware links
   (`pocket/`);
2. runs `firmware/fm1/Build-ClatterPocket.ps1 -FirmwareOnly`, which is **generated** from
   fm1_open's `sdk/Build.ps1` by `tools/derive-fm1-build.py` with seven listed edits
   (paths, Wine, CLATTER Pocket sources, linker placement, SRAM checks). Everything else,
   including the hash checks, all compiler/linker flags, `Test-Firmware` and the packager, is
   upstream's;
3. copies the package to `build/fm1/clatter-pocket-fm1-UNVERIFIED.fwsc` and the firmware
   report to `build/fm1/firmware-report.json`.

`-FirmwareOnly` is the SDK's local-test mode; distributable packages (`-Package`) need a
matching public source archive URL (GPL) and are not set up.

### First build results (2026-10-29, JieLi 2.5.3 under Wine 10.0, macOS arm64)

| Build | App image | Runtime heap | SDK firmware checks | Package |
|---|---|---|---|---|
| `--stock` (fm1_open 4c7496d unmodified) | 453 576 | 247 340 | pass | **built** (`fm1_open-stock-4c7496d.fwsc`) |
| CLATTER Pocket, double precision, `sram` | 588 712 | 94 252 | pass | **does not fit** |
| CLATTER Pocket, `--single-precision`, `sram` | 586 920 | 95 948 | pass | **does not fit** |
| CLATTER Pocket, `--single-precision`, `--placement tcm` | 586 888 | 182 572 | pass | **does not fit** |

All CLATTER Pocket sources compile with PI32v2 clang 4.0.1; the link resolves; the audio
path is in SRAM; RAM is not the problem (the heap minimum is 49 152). **Flash is**: the
update image leaves 479 232 bytes for the app image (HARDWARE.md) and CLATTER Pocket adds
about 133 KB to the stock 453 576, about 108 KB more than fits. Of the 133 KB, 102 KB are
the LXR's constant tables (tri/saw/rect wavetables 3 × 22.5 KB, transient samples 26 KB,
sine 8 KB). Options: ROADMAP.md, milestone B.

Fixed on the way: the Windows clang under Wine does not open `-include` files by absolute
Unix path (forced includes now go through the include path); the derived script passed the
application's include paths ahead of the engine's, so the LXR saw the SDK's `<math.h>`
instead of CLATTER's shim (now left out); the PI32v2 FPU has no square root and the SDK's
libc no `sqrtf`/`fmaf` (`firmware/fm1/pocket_fm1_libm.c`, tested bit-exact by
`host/tests/test_fm1_libm.c`).

The double-precision build calls software double routines from the per-sample DSP
(`1PoleLp.c`, `SlopeEg2.c`, `ResonantFilter.c`, `Snare.c`, `DrumVoice.c`); the
single-precision build calls them only from CLATTER's libm, which is not per sample. The
FM-1 configuration is therefore `--single-precision`.

## Flashing

Not done, not automated, and no CLATTER Pocket build has been approved for a device. The
only installation route is the vendor update protocol over USB-MIDI (HARDWARE.md); the official
workbench does not take external files, so installing a local `.fwsc` needs a local
workbench build (`tools/Build-Web.ps1` in fm1_open) or a separate tool. Read HARDWARE.md
"Updating, recovery, backup" first.

## Updating fm1_open

1. move the submodule, 2. `python3 tools/derive-fm1-build.py` (stops if Build.ps1 changed
where an edit applies), review `git diff firmware/fm1/Build-ClatterPocket.ps1`, 3. check the
patch applies (`firmware/fm1/build.sh --prepare-only`), 4. `firmware/fm1/preflight.sh`,
5. update `pinned=` in `build.sh`.
