# The CLATTER Pocket sequencer

A 16-step, seven-track drum sequencer with a seeded pattern generator, running in the firmware
on the FM-1's own controls and screen. How to play it, the control map and the size are now in
[POCKET-CONTROLS.md](POCKET-CONTROLS.md); this page is how the sequencer works. **Tested on the host and in the FM-1 emulator. Nothing here
has run on a device; the firmware stays UNVERIFIED.**

![PATTERN playing, the generator's knobs and MAIN, read from the emulated LCD](pocket-sequencer-emulator.png)

Left to right: PATTERN playing, the generator's settings on GLOBAL, MAIN.

## Pattern representation

```c
struct pocket_pattern { uint16_t cell[7][16]; };  /* velocity (0 off), and a note or 0 */
```

224 bytes, no allocation. There is one pattern. Tempo (integer BPM), swing, transport and the
position live in `seq/pocket_seq.c`; the screens read them from there and keep no copy. Only the
generator's settings, the selected step and the locks are the panel's.

## Timing

`seq/pocket_seq.c`'s `pocket_seq_block` runs in the audio interrupt, at the start of each
64-sample engine block (`pocket_lxr_set_block_hook`), after queued events and before the block
renders. So a step is timed by the audio samples themselves; the screen and the control task
have no part in it.

**Exact integer arithmetic.** Phase is counted in units of 1 / (20 × BPM) of a sample:

| | Units |
|---|---|
| each 64-sample block adds | 64 × 20 × BPM |
| a straight step needs | 300 × rate |
| the first step of a swung pair | 6 × rate × swing |
| the second | 6 × rate × (100 − swing) |

A pair always adds up to two straight steps. What is left over at a step carries into the next,
so nothing accumulates: over an hour at 137 BPM the step count is exactly right (test). The rate
is the output's real one, `FM1_AUDIO_HARDWARE_RATE`, 44,118 Hz. The audio path has no division;
it has one multiply, one add and one compare per block.

**Quantisation.** A step fires at the first block boundary at or after its exact time: never
early, at most 63 samples (1.43 ms) late, and the error does not grow. The LXR's voices cannot
start inside a block: each voice's envelope and filter state is computed per block, and
splitting blocks would change the DSP. The LXR-02 itself triggers at this boundary.

**Tempo and swing changes.** A new tempo applies from the next block and moves no position. A
new swing applies from the next pair, so the pair in progress stays two steps long. The shortest
possible step (300 BPM, swing 75) is 1,103 samples, 17 blocks, so two steps can never fall in one
block.

**Start and stop.** One byte from the control task, read in the next block: step 0 sounds in
that block, at most 1.45 ms after the press is read. STOP silences the next step; voices ring out
as they do after a key press. PLAY always starts from step 0.

## Live editing

The control task and the interrupt share only single aligned words. There are no locks and no
queue, because none is needed:

- **A step edit** is one aligned 16-bit cell, so velocity and note change together. Ahead of the
  playhead it is heard this time round; behind it, next loop.
- **A whole-pattern change** (GENERATE, MUTATE, undo) is prepared in a second pattern that the
  interrupt is not reading, and published by writing one byte. The next step plays all of the new
  pattern, from the same position; no step can see half of it. The second pattern is also why undo
  costs nothing.
- **Voices** are triggered by the same path as keys and MIDI (`pocket_lxr_trigger_now`, one
  `EV_TRIGGER` applied directly). The event ring keeps its single producer: the block hook does
  not use it.

This relies on the control task and the audio interrupt sharing a core, the same assumption as
the event ring (`POCKET_BARRIER`; HARDWARE.md lists it as an open question).

## Tracks and voices

The LXR has **six voices and seven triggers**, and the sequencer has one track per trigger, in the
order the interface already shows: D1, D2, D3, SN, CY, CH, OH. There is no 10-to-7 track mapping to
hide. Browser CLATTER's ten track roles fold into these the way its default project routes them:

| Track | Trigger | CLATTER's roles |
|---|---|---|
| D1 | DRUM 1 | kick |
| D2 | DRUM 2 | low tom |
| D3 | DRUM 3 | high tom, perc |
| SN | SNARE | snare, clap |
| CY | CYMBAL | cymbal, fx |
| CH | CL HAT | closed hat |
| OH | OP HAT | open hat |

Up to six voices sound on one step, all triggered in the same block.

**Voice sharing:**
- CH and OH are one voice. If both are on a step, the open hat plays and the closed hat does not
  (CLATTER's choke); the generator also never writes both. A closed hat cuts an open one that is
  still ringing.
- They share pitch, filter, drive, level and pan; each has its own decay (SOUND).
- A key or MIDI hit on a voice the pattern is playing restarts that voice.

**Velocity.** The LXR's velocity gain is a per-voice switch (`PAR_VOLUME_MOD_ON_OFF1-6`) that the
init kit leaves off, so without it every velocity sounds the same. It is switched on when the
sequencer starts, and every kit keeps it on. Keys send 127 until told otherwise, which is unity. **MIDI velocity is
audible too**, as FIRST-HARDWARE-TEST.md expects.

## Generation

`seq/pocket_gen.c` is a port of CLATTER's `src/rhythm/generate.ts`, in integer arithmetic, with
CLATTER's 11 styles (`seq/pocket_styles.h`, generated from `styles.ts` by
`tools/gen-seq-styles.py`). It does not draw each step independently. Each role has a **lane**, a
chance per step that says what that role is for, and the lanes are shaped by metric position:

- **Kick** anchors: lanes put certainty on the strong beats, and step 0 is kept.
- **Snare** accents: strong backbeat digits, ghost notes in the weak positions of some styles.
- **Hats** subdivide, with velocity following the beat.
- **Toms and percussion** vary: sparse lanes, and fills into the end of the bar, high toms first.

| Setting | Effect |
|---|---|
| **DENSITY** | An exponent on every chance (`1 − (1 − p)^(1/k)`, a table here), so certain steps stay and weak ones come and go. 50 is the style as written |
| **COMPLEXITY** | Moves weight off the beat: pickups where a lane is silent, mostly just before a strong beat; written weak steps loosened; ghost notes; more fills |
| **SEED** | Every decision is a keyed hash of (seed, purpose, track, step). The same settings give the same pattern on any platform, and raising DENSITY adds to a rhythm rather than drawing another |
| **STYLE** | MACHINE, ELECTRO, BREAKS, JUNGLE, HALFTIME, BROKEN, GARAGE, INDUSTRIAL, GLITCH, POLYMETER, DUB. The last-but-two and last-but-one use Euclidean lanes: cross-rhythms that do not fit the bar |

Then rules across tracks (`arrange`): hats choke; the kick clears a strong backbeat where the
style says so; toms keep off the kick; a pattern is never empty.

**Added for Pocket.** CLATTER gets its variety from phrases of several bars; here the phrase is
one bar. So above 60 % COMPLEXITY supporting tracks are displaced by a seeded rotation, and above
80 % the closed hats become a Euclidean figure of 5, 7, 11 or 13 and the kick loses its anchor.
That is the experimental end. Below that, a pattern is recognisably its style.

**Not ported:** VARIATION and multi-bar phrases (one bar), per-step probability and conditional
trigs (no room in Flash), per-style tempo and swing (GENERATE changes the pattern, never the tempo).

Patterns are **not** byte-compatible with CLATTER's for the same seed: the hash's inputs and the
arithmetic differ. Compatibility is with its musical rules.

## Mutation

Also in `seq/pocket_gen.c`, a port of CLATTER's `src/rhythm/mutate.ts` (itself adapted from
DOWNBEAT's EXTENDED mutation, MIT). It applies a short list of musical edits to a copy of the
pattern, so the result stays related to what was there.

| Strength | Edits, for about 28 hits | Edits allowed |
|---|---|---|
| LOW (0.2) | about 2 | add, remove, shift, velocity, accent, swap |
| MED (0.5) | about 7 | also density and rest runs |
| BIG (0.85) | about 14 | also recombining beats, rotating and reversing |

Edits prefer weak metric positions at low strength, so the anchors stay: removing a strong loud
hit needs more than MED. Each press uses the next value from (seed, count), so it is deterministic
and each press differs. An edit that finds nothing to do (a shift onto a hit) is drawn again, so
a press changes something: without this 1 press in 10 did nothing at LOW.

Tracks locked with SAVE are never touched, by either operation. The lock is per track; there is
no per-step protection.

Notes move with their hits, because an edit moves whole cells. Pitch changes are an added edit:
[POCKET-CONTROLS.md](POCKET-CONTROLS.md).

## Size, tests and results

In [POCKET-CONTROLS.md](POCKET-CONTROLS.md), for the firmware as it now stands.

## Known limitations

- A restart gives seed 1's pattern at 120 BPM until a saved one is loaded ([POCKET-STORAGE.md](POCKET-STORAGE.md)).
- One pattern, 16 steps, 16th notes. No chaining, no track lengths.
- Steps have velocity and a note: no probability or parameter locks, though the engine has locks
  (`pocket_lxr_trigger_locked`).
- Timing resolution is the 64-sample block, 1.45 ms.
- No MIDI clock in or out, and no MIDI note output.
- Undo is one level, and a step edit since the last GENERATE or MUTATE goes with it.
- Swing is by pairs of steps, on 16ths.
- An amp decay of 127 on SOUND never ends and 126 lasts minutes: LXR firmware behaviour
  (`test_long_decays`), the same after a key press. STOP does not mute voices, so such a voice
  keeps sounding until something else changes it.
