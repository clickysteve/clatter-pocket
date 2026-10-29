# Architecture

```
 FM-1 (fm1_open V14, stock application + binary SDK)            CLATTER Pocket
 ─────────────────────────────────────────────────────          ─────────────────────────────────
 audio ISR  fm1_audio_irq (app/board/audio.c)            128 frames, Q23 stereo
   fm1_engine_render ── stock instrument (binary)
   clatter_pocket_fm1_render ─────────────────────────▶ pocket_lxr_render_q23 (accumulate)
   fm1_output_mix ── master gain, USB/BT in, USB rec        2 × 64-sample LXR blocks:
   copy to DMA half                                           drain ≤32 events, then the
                                                              firmware's block sequence,
 control task groove_screen_control (app/screen.c)            6 voices panned, Q12 master,
   fm1_controls_read                                          DC blocker, Q23 saturation
   MASTER knob
   clatter_pocket_fm1_control ──── HOME+ARP ──────────▶ pocket_panel_apply ──▶ event ring
     (Pocket mode owns the panel; stock UI returns)     pocket_panel_draw_band ──▶ fm1_screen_blit
   stock UI / draw / paint_flush
```

Layers, each independent of the one above:

| Layer | Files | Depends on |
|---|---|---|
| LXR-02 synthesis, unchanged | `lxr/upstream/` (byte-identical to CLATTER's import of the firmware, `lxr/MANIFEST.sha256`) | `lxr/shim` |
| CLATTER's port glue | `lxr/shim/` (HAL/CMSIS intrinsics, libc headers), `lxr/bridge/lxr_params.c` (the firmware's parameter handler), `lxr/bridge/libm.c` | nothing platform-specific |
| Engine host | `engine/pocket_lxr.c/.h`, `engine/fx_off.c` / `fx_on.c`, `engine/pocket_params.h` (generated) | the two above |
| Panel | `ui/pocket_panel.c/.h` | engine host API only |
| FM-1 adapter | `firmware/fm1/app/clatter_pocket/`, `fm1_open.patch`, `pocket_fm1_config.h` | SDK API |
| Host harness | `host/` | engine host, panel |

## Porting decisions

- **The synthesis is the firmware's, through CLATTER.** CLATTER already isolates the LXR-02
  DSP as freestanding C (30 upstream files unchanged, a shim, the parameter handler extracted
  from `MidiParser.c`). CLATTER Pocket uses exactly those files rather than re-importing from
  Erica Synths' repository, so the browser and the hardware run the same code, and checks
  that numerically (COMPATIBILITY.md). The original firmware was inspected for embedded
  integration: it already marks its per-sample code and voice state for the STM32's tightly
  coupled memories (`IN_ITCM_RAM`, `INDTCM`, `INDTCMZ`); the FM-1 linker script maps those
  sections to internal SRAM, which is where FM-1 audio code must live.
- **Only the audio context touches LXR state.** Control calls push 16-byte events into a
  256-entry single-producer/single-consumer ring; the audio context applies at most 32 per
  64-sample block before rendering, so parameter edits cannot tear a voice and ISR time stays
  bounded. The firmware's own parameter handler runs in the audio context. A kit reload is
  247 events, spread over eight blocks.
- **Trigger timing** is the LXR's own: hits take effect at 64-sample block boundaries
  (1.45 ms), as on the LXR-02. A sequencer will timestamp events in samples; the engine can
  split blocks at 64-sample granularity without changing the firmware.
- **Block size.** The FM-1 delivers 128-frame blocks; the engine renders two LXR blocks per
  FM-1 block. Sample rate: the firmware's coefficients are for `REAL_FS` = 44 140 Hz; the
  FM-1 runs at 44 100 (hardware 44 118) Hz, so pitches are 1.6 cents flat and envelopes
  0.09 % long, as in CLATTER, which made the same choice.
- **Mixing.** The six voices are mixed with the firmware's trigger-time pan law
  (`mixer_addDataToOutput`), summed in 32 bits, scaled by a Q12 master gain, converted to Q23
  (int16 full scale = Q23 full scale) and **saturated** (counted, never wrapped). They are
  added to the stock instrument's block before the FM-1's own master gain and USB/BT mixing.
- **DC blocker** (one pole, 5 Hz, on for the FM-1, off for comparisons). The LXR's decimator
  is a sample-and-hold; near rate 0 it holds a DC level for ever, which the LXR-02's
  AC-coupled outputs remove (test `test_decimator_hold`). It changes a kick's peak by 0.6 %.
- **FX bus off.** The firmware's delay needs 64 KB (`FX_BUFFER`); the FM-1 build links
  `engine/fx_off.c` (the bus exists, stays `FX_OFF`, FX parameters are kept in the kit). The
  voices do not pass through the bus, so they render identically (both test variants).
- **No allocation, no locks, no I/O** in the audio path; all state is static.
- **Lazy start.** The engine initialises when Pocket mode is first entered and renders only
  while Pocket mode is shown and for 3 s after (ring-out). Users who never press HOME+ARP get
  the stock firmware's behaviour and CPU load.
- **Bug found in CLATTER's bridge.** `dispatchParam(127)` (`PAR_BANK_CHANGE`, sent by
  `clatter_load_init_kit`) produces CC 0, and the firmware's handler then writes
  `originalCcValues[65536]`, 64 KB past a 255-byte array. WebAssembly's flat memory hides it;
  natively it faults. CLATTER Pocket keeps the value and never dispatches it. CLATTER itself
  was not changed (see the report).

## Performance

### Host (measured, `make bench`, Apple silicon, 20 s per scenario)

| Scenario | Mean per 128-frame block | p99 | Share of real time |
|---|---|---|---|
| idle | 25 µs | 47 µs | 0.87 % |
| kick | 21.4 µs | 29 µs | 0.74 % |
| all six voices sounding | 21.4 µs | 30 µs | 0.74 % |
| six voices retriggered every 1/16 | 21.4 µs | 30 µs | 0.74 % |

The cost does **not** depend on how many voices sound: like the firmware, the engine computes
all six voices every block. On the FM-1 the LXR is therefore a **fixed load**, whatever is
playing. (Skipping silent voices is a possible later optimisation; it changes nothing audible
only if filter and LFO state are handled with care.)

### FM-1 (not measured)

There is no device measurement. What is known:

- The same code runs all six voices, the FX bus, the sequencer and the UI of the LXR-02 on a
  216 MHz Cortex-M7 with a double-precision FPU.
- The AC791N is reported at up to 320 MHz; its pipeline and caches are unknown, and the
  stock instrument shares the same interrupt.
- **Double precision: none.** The PI32v2 FPU is single precision only (checked with the
  SDK's toolchain). The firmware computes envelopes (`slopeEg2_calc`), amplitude smoothing
  (`calcOnePole`) and some filter terms in `double` in per-sample paths; in the FM-1's
  double build these call software routines (`__muldf3`, `__adddf3`, ...) from five DSP
  files. The FM-1 configuration is therefore `--single-precision`
  (`engine/single_precision.h`): no DSP file calls a double helper (checked on the linked
  PI32v2 objects), and against CLATTER the output stays within -78 dBFS (COMPATIBILITY.md).
- No square root or fused multiply-add either: `firmware/fm1/pocket_fm1_libm.c` supplies
  `sqrtf` (correctly rounded, integer arithmetic, trigger-time only) and `fmaf` (CLATTER's
  definition).
- The FM-1 build measures itself: the audio hook times every block with the board's audio
  timer and the Pocket screen shows mean and peak share of the block period, DMA overruns
  (the SDK's counter), clipped samples and dropped events.

### Memory

Measured with the SDK's toolchain. Beside the stock instrument CLATTER Pocket does not fit
the update image (BUILDING.md, First build results). The FM-1 firmware therefore
**replaces the stock instrument application** (FM1-REPLACEMENT.md: 421,212-byte image,
32,709 bytes under the 453,921 limit, runtime heap 134 KB) and stores the LXR's 102 KB of tables packed
(24 KB; `engine/lxr_tables_unpack.c` restores them byte-identically in RAM before the
engine starts). CLATTER Pocket's engine is 28 KB of code; its RAM is the unpacked tables
(102 KB), the LXR's voice state and SRAM code, the event ring and the panel band buffer.

The earlier estimate, by section:

`tools/size-estimate.sh` compiles the FM-1 configuration for a Cortex-M4F stand-in. Tables
and state are exact; code size on PI32v2 will differ.

| | Bytes | FM-1 placement |
|---|---|---|
| LXR per-sample code (`.itcm_text`) | 23 776 | SRAM |
| other code (init, parameter handler, triggers, libm, panel) | 52 868 | SRAM (`sram`) or Flash (`tcm`) |
| constant tables: tri/saw/rect wavetables 3 × 22.5 KB, ... (`.rodata`) | 68 654 | SRAM (`sram`) or Flash (`tcm`) |
| sine table, transient samples (`.dtcm`) | 34 930 | SRAM |
| voice state (`.dtcmz`) | 3 774 | SRAM |
| event ring, mix buffers, parameter array (`.bss`) | 8 325 | SRAM |
| panel band buffer (adapter) | 7 680 | SRAM |

Total: about **200 KB** of the FM-1's 524 KB RAM0 with `--placement sram`, about **79 KB**
with `--placement tcm`. How much the stock firmware leaves free is known only after the first
link (the build fails if the runtime heap drops below 48 KB).

## Capacity outlook (to be measured, in this order)

1. one LXR voice set (six voices, seven triggers) beside the stock engine - the current build;
2. the same with the stock engine idle vs playing;
3. CLATTER Pocket as the only instrument (not calling `fm1_engine_render` needs care: the
   stock UI waits on engine state);
4. ten CLATTER tracks: tracks map onto the six voices (as in CLATTER); more simultaneous
   sounds need more voice instances, each a fixed cost;
5. modulation (the firmware's LFOs and velocity routing are already in);
6. FX bus (64 KB);
7. sample playback (only the user Flash partition; SEA-coded slots exist in the stock engine).
