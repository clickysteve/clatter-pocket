# Roadmap

Status markers: **done** (working and tested on the host), **prepared** (written and
checked as far as possible without the toolchain or device), **open**.

## A - Hardware and SDK verification

- **done** FM-1 SDK found and verified: official M-VAVE repository, licences, binary/source
  split, toolchain contract (HARDWARE.md)
- **done** hardware facts from the SDK sources: MCU, memory map, audio format and path,
  panel, MIDI, storage, update protocol; open questions listed
- **done** flashing and recovery documented, including what is not recoverable
- **done** toolchain: JieLi 2.5.3 under Wine, all 22 pinned files verified
  (`tools/setup-toolchain.sh`); PowerShell 7
- **done** the unmodified fm1_open builds and packages with it (`build.sh --stock`, the
  known-good example)
- **done** CLATTER Pocket compiles, links and passes the SDK's firmware checks (SRAM, heap,
  BLE, USB) in every configuration; footprints recorded (BUILDING.md)
- **done** the FPU question: single precision only, no sqrt, no fma
- **open** identify the owner's unit (revision, firmware identity), back up, confirm
  V15/Open restore works on it

## B - Authentic LXR audio

- **done** the LXR-02 engine natively, through CLATTER's port; real-time host with event
  ring, Q23 output in FM-1 blocks, saturation, DC blocker, statistics
- **done** host tests (48 + 17 checks): silence, pitch (COARSE semitones exact), note
  tracking, decay, level, every trigger, locks, queue bounds, clipping, determinism,
  stability, firmware behaviours (infinite decay, decimator hold)
- **done** numerical equivalence with CLATTER's WebAssembly engine (within 3 LSB)
- **done** host CPU, memory and soft-float assessment
- **done (emulator; not on hardware)** the first hardware interface: start screen, MAIN,
  PATTERN, SOUND, MIXER, GLOBAL and SYSTEM pages, trigger, MIDI and overrun feedback, 175 checks
  in the FM-1 emulator on the real package (docs/POCKET-UI.md)
- **prepared** FM-1 integration: audio hook, control hook, Pocket panel (kick and the other
  six triggers: pitch, decay, level, keys and PLAY), on-device timing display
- **done: flash budget.** CLATTER Pocket replaces the stock instrument application, the
  LXR tables are packed losslessly (102 KB -> 18 KB, unpacked into RAM at start-up), control
  code is `-Oz`: a valid package with 32,709 bytes to spare (the limit is 453,921, not the
  479,232 first reported), all SDK checks passing, the stock flash layout, the user
  partition untouched (FM1-REPLACEMENT.md). Beside the stock instrument it is still
  53,671 bytes too large. The options that were considered:
  1. pack the LXR's tables (second-order delta coding measures about 76 KB -> 11 KB for the
     wavetables, 26 -> 13 KB for the transients) and unpack them into SRAM when Pocket mode
     starts: about 77 KB, not enough on its own;
  2. size-optimise the engine's non-audio code (`-Oz`): a few KB;
  3. a smaller user area (the 536 576-byte projects/samples partition): changes the device
     layout and where user data lives; only with the owner's decision and after confirming
     how the stock firmware, loader and workbench treat it;
  4. drop parts of the stock application from the image (CLATTER Pocket as the instrument
     rather than beside it); the stock synthesis is a binary library linked whole.
- **done** pre-flash audit (PRE-FLASH-AUDIT.md): `sqrtf` miscompilation, MIDI pitch, VM
  layout, start-up order (engine after the updater, SAFE MODE), SRAM placement, clean
  builds, installer page, start-up diagnostics, exhaustive MIDI tests; reproducible package
- **next** the first hardware test (FIRST-HARDWARE-TEST.md), with the owner's approval
- **open** a settings page for the linked Bluetooth/BLE API (role, scan, connect), and a
  way to tell the user that workbench backups need the stock firmware
- **open** then on-device: audio heard, CPU per block, overruns, clipping, stability over an
  hour, latency (USB recording of a key press vs the click)
- **done** single precision is the FM-1 configuration (no soft double in the per-sample DSP)

## C - Basic drum machine

- **done (emulator; not on hardware)** a portable sequencer core (`seq/`): 16 steps, seven tracks,
  velocity, tempo, swing, start/stop, exact sample-based timing in the audio interrupt, live
  editing, undo; the PATTERN page. Timing tested to the sample on the host and by measuring the
  emulated audio (docs/POCKET-SEQUENCER.md)
- **done** tracks: one per LXR trigger, CLATTER's ten roles folded as its default project routes them
- **done (emulator; not on hardware)** save/load in the user Flash partition with CRC'd, versioned,
  two-sector banks, after its own audit (docs/POCKET-STORAGE.md)
- **done (emulator; not on hardware)** DRUM and PITCH keyboards, step and live recording with a note
  per step, GLOBAL settings (docs/POCKET-CONTROLS.md)
- **open** more than one pattern playing in turn, chaining, track lengths
- **open** MIDI clock in and out, and note output

## D - Generative CLATTER

- **done (emulator; not on hardware)** seeded, deterministic generation on the device: CLATTER's
  11 styles, DENSITY, COMPLEXITY, SEED, instrument roles, fills, Euclidean lanes, track locks; and
  MUTATE at three strengths
- **done (emulator; not on hardware)** 16 kits and kit generation from LXR parameters: archetypes,
  characters, families, WILD; one-voice sounds, one-track patterns, pitch in mutation
- **open** multi-bar phrases; per-step probability, conditional trigs, every-N
- **open** parameter locks per step (the engine has them)
- **open** kit generation and mutation within instrument-aware ranges, wildness/character
- **open** human-readable seeds (CLATTER's documented hash) and seed compatibility with CLATTER:
  today's patterns follow its rules but are not byte-identical

## E - Persistence and compatibility

Versioned interchange format shared with CLATTER (COMPATIBILITY.md), capability flags,
unknown-field preservation, import/export over USB-MIDI SysEx (the SDK's hook) or the
workbench.

## F - Hardware polish

A SETTINGS page, and more of SOUND and MIXER (first versions: docs/POCKET-UI.md), for 27 keys, 14 buttons, 7 encoders and
240 × 240; documented input map; performance work; release packaging (`-Package` with a
public source archive, GPL); decide whether CLATTER Pocket replaces or sits beside the stock
instrument.

## Later, if the budget allows

FX bus (64 KB), sample playback and HYBRID tracks (user Flash only), more voice instances,
destruction (CHOP/ROLLS/DESTROY/WILDCARD).
