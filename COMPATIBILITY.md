# Compatibility with CLATTER

## Synthesis

CLATTER Pocket compiles the same LXR-02 firmware files as CLATTER (`lxr/upstream/`, CLATTER
revision in `lxr/CLATTER_REVISION`, firmware revision `a43cd4fc`), with the same shim, the
same `libm.c` and the same floating-point flags. `make compare` runs one script
(`host/compare/script.txt`: hits on all seven triggers, parameter changes, a one-hit lock,
decimation, filter changes; 800 blocks) through CLATTER's committed WebAssembly engine and
through CLATTER Pocket's native build, and compares every voice's output sample by sample.

Measured 2026-10-27 (native: Apple clang 21, arm64; CLATTER: lxr.wasm at `991e61b`):

| Voice | Identical blocks | Max error | Signal / error |
|---|---|---|---|
| DRUM 1 | 791 / 800 | -84.3 dBFS | 113.9 dB |
| DRUM 2 | 647 / 800 | -80.8 dBFS | 75.6 dB |
| DRUM 3 | 800 / 800 | exact | exact |
| SNARE | 788 / 800 | -90.3 dBFS | 108.6 dB |
| CYMBAL | 799 / 800 | -90.3 dBFS | 103.3 dB |
| HIHAT | 800 / 800 | exact | exact |

The largest difference is 3 steps of 16-bit audio. The causes are legitimate platform
differences: different compilers order fast-math float operations differently, and a
difference of one rounding step in a float that is converted to int16 shows up as one LSB.
The pass threshold is -60 dBFS.

Single-precision fallback (`make compare-sp`, the DSP's `double` computed as `float`): max
error -78.3 dBFS, worst voice 62.8 dB signal/error (cymbal). Inaudible differences; to be used
only if the FM-1's FPU has no double precision.

The FM-1 build will differ again (PI32v2 clang 4.0.1, FPU rounding), and has to be compared
on the device by recording USB audio of the same script. Not done yet.

The FM-1 build stores the LXR's tables packed and unpacks them at start-up
(FM1-REPLACEMENT.md); `make test` checks that the unpacked tables are byte-identical to the
firmware's and that the comparison script renders bit-identical voices with them.

Known differences from CLATTER, by design: no FX bus on the FM-1 (FX parameters are kept);
output DC blocker on the FM-1; the FM-1's 44 100 Hz is CLATTER's rate too, so pitch and timing
match each other (both 1.6 cents below the LXR-02).

## Parameters and kits

Parameters are the firmware's `PAR_*` numbers (`engine/pocket_params.h`, generated from
`Parameters.h` as CLATTER's `lxrParamIds.ts` is), with 0-127 values: a CLATTER kit's values
mean the same in CLATTER Pocket. One exception in dispatch, not in data: parameter 127
(`PAR_BANK_CHANGE`) is stored but not sent to the engine (ARCHITECTURE.md).

## Projects and patterns

No exchange format exists yet. Planned (ROADMAP.md, milestone E): a versioned format
carrying the musical data both sides share (pattern steps, velocity, conditions, seeds,
wildness/character, tempo, swing, track configuration, the kit's parameter values and
modulation targets), explicit capability flags for features one side lacks (samples, FX bus,
destruction, more than six voices), and preservation of unknown fields on round trips. CLATTER's
project serialisation has to be studied first; nothing here assumes its structure.
