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

**Status: prepared, not run.** The toolchain install was not executed in the first session,
so no PI32v2 object has been compiled, nothing has been linked and no firmware package
exists. Everything up to the compiler is checked (`preflight.sh`).

### Requirements

- **JieLi Windows toolchain 2.5.3** (PI32v2 clang 4.0.1). The SDK checks 22 of its files by
  SHA-256 and refuses any other version.
- **PowerShell 7** (`pwsh`). The SDK's build and packager are PowerShell/C#. macOS:
  `brew install --cask powershell`.
- On macOS/Linux, **Wine** to run the Windows toolchain (Wine 10 checked to be installed; the
  toolchain has not yet been run under it).
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

### Build

```bash
firmware/fm1/build.sh --prepare-only   # build/fm1/tree: fm1_open + patch + CLATTER Pocket sources
firmware/fm1/preflight.sh              # SDK manifest + stand-in compile of every touched file
firmware/fm1/build.sh                  # the real build (toolchain, pwsh, Wine)
```

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

### Expected first-build problems

These are untested, in rough order of likelihood: Wine path handling for the toolchain;
the LXR objects exceeding SRAM in `sram` placement (link error or the 48 KB heap check: try
`--placement tcm`); PI32v2 clang 4.0.1 rejecting something modern clang accepted;
`EXCLUDE_FILE` support in `pi32v2-ld`; the size of soft-double code if the FPU is single
precision (try `--single-precision`).

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
