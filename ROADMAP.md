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
- **prepared** FM-1 integration: audio hook, control hook, Pocket panel (kick and the other
  six triggers: pitch, decay, level, keys and PLAY), on-device timing display
- **blocked: flash budget.** CLATTER Pocket beside the stock instrument needs about 108 KB
  more than the update image has room for (BUILDING.md, First build results). Ways out, none
  taken yet:
  1. pack the LXR's tables (second-order delta coding measures about 76 KB -> 11 KB for the
     wavetables, 26 -> 13 KB for the transients) and unpack them into SRAM when Pocket mode
     starts: about 77 KB, not enough on its own;
  2. size-optimise the engine's non-audio code (`-Oz`): a few KB;
  3. a smaller user area (the 536 576-byte projects/samples partition): changes the device
     layout and where user data lives; only with the owner's decision and after confirming
     how the stock firmware, loader and workbench treat it;
  4. drop parts of the stock application from the image (CLATTER Pocket as the instrument
     rather than beside it); the stock synthesis is a binary library linked whole.
- **open** then on-device: audio heard, CPU per block, overruns, clipping, stability over an
  hour, latency (USB recording of a key press vs the click)
- **done** single precision is the FM-1 configuration (no soft double in the per-sample DSP)

## C - Basic drum machine

Port the sequencer core from CLATTER/DOWNBEAT as portable C (`seq/`): 10 logical tracks
mapped onto the LXR voices, pattern length, steps with velocity, tempo, swing, start/stop, an
explicit sample-timestamped event scheduler driven by the audio block count (never by UI
timing), save/load in the user Flash partition with CRC'd, versioned, two-sector records.
Tests: step timing to the sample, swing, velocity, tempo changes, wrap. Then a PATTERN page.

## D - Generative CLATTER

Seeded, deterministic, on-device: human-readable seeds (CLATTER's documented hash), pattern
generation per style, conditional triggers and probability (pass counter), every-N, mutation,
kit generation and mutation within instrument-aware ranges, wildness/character. Port from
CLATTER's TypeScript after checking each algorithm's CPU and memory cost. Test seed
reproducibility against CLATTER's outputs.

## E - Persistence and compatibility

Versioned interchange format shared with CLATTER (COMPATIBILITY.md), capability flags,
unknown-field preservation, import/export over USB-MIDI SysEx (the SDK's hook) or the
workbench.

## F - Hardware polish

PATTERN / SOUND / GENERATE / MIXER / SETTINGS pages for 27 keys, 14 buttons, 7 encoders and
240 × 240; documented input map; performance work; release packaging (`-Package` with a
public source archive, GPL); decide whether CLATTER Pocket replaces or sits beside the stock
instrument.

## Later, if the budget allows

FX bus (64 KB), sample playback and HYBRID tracks (user Flash only), more voice instances,
destruction (CHOP/ROLLS/DESTROY/WILDCARD).
