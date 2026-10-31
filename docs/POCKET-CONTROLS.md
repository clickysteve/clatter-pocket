# Playing CLATTER Pocket

The FM-1's own controls, as a generative drum machine: two keyboard modes, recording, kits and
rhythms made on the device. **Tested on the host and in the FM-1 emulator. Nothing here has run on a
device; the firmware stays UNVERIFIED.**

![A new kit, PITCH mode, step entry and the scrolled GLOBAL page, read from the emulated LCD](pocket-controls-emulator.png)

## Quick start

In the [browser emulator](https://joakimeriksson.github.io/fm1-emu/) at 240 MHz (loading it:
[POCKET-UI.md](POCKET-UI.md)). The computer key for each control is in brackets; the knobs have
none, so use the mouse.

1. **Generate a kit.** Press ENV `3`. The status line says GENERATING KIT, then the kit's name and
   seed. To choose a character first, turn PRESETS `↑ ↓`.
2. **Generate a pattern.** Press ARP `9`. ALGORITHM `, .` chooses the style first, if you want one.
3. **Press PLAY** `Space`. Leave it running: everything below works while it plays.
4. **Switch keyboard modes.** SCL `2` changes KEYS: DRUM to KEYS: PITCH. In DRUM, keys 1 to 9 are the
   nine sounds, and 10 to 18 and 19 to 27 are the same nine in variations. In PITCH every key plays the
   selected voice, and OCT− `Z` and OCT+ `X` change the octave.
5. **Record notes.** Press REC `R`. Playing, what you hit lands on the nearest step. Stopped, what
   you hit lands on the selected step, and hitting it again removes it.
6. **Edit the pattern.** SEQ `0` opens PATTERN. KNOB 4 moves the selected step, KNOB 1 sets its
   velocity, KNOB 2 the tempo, KNOB 3 the swing. Hold SAVE `8` and turn KNOB 3 for more pages, KNOB 1
   for the step's condition.
7. **Regenerate one instrument.** Turn SELECT `← →` to a voice, or hit its key. **Hold SAVE `8`** and
   press ENV `3` for a new sound, or ARP `9` for a new rhythm. The rest stays as it was.
8. **Mutate the rhythm.** LFO `4` varies the pattern; hold SAVE and press LFO for one track only.

SAVE+OCT− `8`+`Z` undoes the last thing you did, kit or pattern, and SAVE+OCT+ `8`+`X` redoes it.

To keep it: tap SAVE `8`, choose a slot with SELECT, tap SAVE again ([POCKET-STORAGE.md](POCKET-STORAGE.md)).

## Control map

Every control below works on every page, unless it says otherwise.

| Control | Action | Held SAVE + it |
|---|---|---|
| **Keys** | play: DRUM or PITCH, below | |
| **SCL** | KEYS: DRUM or KEYS: PITCH | |
| **OCT− / OCT+** | PITCH: octave, −3 to +3. DRUM: how hard the keys hit, 40, 70, 100 or 127 | OCT−: undo. OCT+: redo. One history of kits and patterns |
| **PLAY** | start or stop | |
| **REC** | recording on or off | |
| **ENV** | a new kit, in the character PRESETS chose | a new sound for the selected voice |
| **ARP** | a new pattern, in the style ALGORITHM chose | a new pattern for the selected track |
| **LFO** | mutate the pattern, at GLOBAL's MUTATE strength | mutate the selected track |
| **SAVE** | a tap opens PROJECTS. There, a tap stores | |
| **FX** | MIXER | lock or unlock the selected track: ARP and LFO leave it alone |
| **SELECT** | the selected voice. On PROJECTS, the slot | |
| **PRESETS** | the kit: INIT, then 16 characters | |
| **ALGORITHM** | the pattern style, of 11 | |
| **HOME SEQ EDIT** | MAIN, PATTERN, SOUND | |
| **GLO** | GLOBAL; again, SYSTEM | |
| **MASTER** | output level | |
| HOME held at 1.5 s after power-on | SAFE MODE, unchanged | |

| Knob | PATTERN | PATTERN, SAVE held | SOUND | MIXER | GLOBAL | PROJECTS |
|---|---|---|---|---|---|---|
| KNOB 1 | selected step's velocity | its condition | pitch | level | row | slot |
| KNOB 2 | tempo | | decay | pan | value | |
| KNOB 3 | swing | length, a page at a time | filter | | | |
| KNOB 4 | selected step, across pages | a page at a time | drive | | | |

Lights: PLAY playing, REC recording, SCL in PITCH, SAVE on a locked track.

### The FM-1 is not what was asked for in one respect

**SELECT cannot be held.** It is a rotary encoder with no switch: the SDK reports 14 buttons, 27 keys
and 7 encoders, and an encoder is two contacts (`fm1_input.h`, `struct fm1_controls`). So every "hold
SELECT" in the brief is **hold SAVE** here. A tap of SAVE acts on release, and only if nothing else
happened while it was down: any button, knob or key makes it a chord. On a track that is locked,
SAVE+ARP and SAVE+LFO refuse and say so. The footer shows the chords while SAVE is down.

**The keys have no velocity.** The scanner reports on and off, nothing else (`fm1_scan_decode`; the
emulator's `fm1_velocity` is for another board, the SMK-37). So the keys play at one level that you
set, with OCT in DRUM or KEY VEL on GLOBAL. MIDI notes keep their own velocity.

### What moved, and where it went

| Was | Now |
|---|---|
| OCT− / OCT+ changed the page | the page buttons; every page has one |
| REC toggled the selected step | keys, with REC on and stopped. There is no longer a way to turn a step off while playing: stop first, or use SAVE+ARP |
| ALGORITHM moved the step | KNOB 4 |
| SCL put the knobs on the generator | GLOBAL has DENSITY, COMPLEX, SEED and MUTATE. The style is on ALGORITHM |
| KNOB 4 set the mutation strength | GLOBAL's MUTATE |
| ENV undid the pattern | SAVE+OCT− |
| The buttons acted on PATTERN only | on every page: you can generate from SOUND. Each change has its undo |
| Keys played the selected voice; the 13th key (F4) was the programmed pitch | DRUM at start-up. In PITCH, **C4** is the programmed pitch, which is what the engine calls note 60 |

## The keyboard

### DRUM

Three banks of nine keys, the same nine sounds in the same order. Key 1 is F3, the lowest.

| Keys | Bank | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 to 9 | original | kick | snare | closed hat | open hat | low tom | high tom | cymbal | perc | clap |
| 10 to 18 | A | … A | | | | | | | | |
| 19 to 27 | B | … B | | | | | | | | |
| | Track | D1 | SN | CH | OH | D2 | D3 | CY | D3 | SN |

The order is the drum machine's usual reading order: the backbeat under the first fingers, the hats next
to it, toms then cymbal, and the two extra sounds last. MAIN and the other pages name what you played,
KICK A or PERC B, beside the voice.

**Nine sounds from seven triggers.** The LXR has **six voices and seven triggers**, and two more
independent voices would be a new synthesis engine, so there are not nine independent instruments.
PERC and CLAP are the voices of D3 and SN with their parameters moved for one hit:

- they are genuinely other sounds. PERC is high, short and FM; CLAP is noise with envelope repeats and
  a resonant filter. Across all 17 kits their distance from the sound they share a voice with is
  measured (below);
- **they cannot sound with the sound they share a voice with**. HI TOM cuts PERC and SNARE cuts CLAP, and
  the reverse, as any hit on a voice does. In a pattern, D3 and SN have one step each, so a step is the
  tom or the perc, the snare or the clap. Say so here rather than pretend: six things can sound at
  once, not nine.
- CH and OH are one voice, as they always were.

**Variations.** A is a moderate alternative, B goes further:

| Sound | A | B |
|---|---|---|
| Kick | tighter, more click and drive | long, lower, driven |
| Snare | tighter, higher, a little drive | longer, dirty, decimated |
| Closed hat | darker | metallic: higher, resonant, driven |
| Open hat | shorter, darker | longer, driven, decimated |
| Low, high tom | higher, shorter | FM, longer, driven |
| Cymbal | choked, brighter | a gong: lower, longer, driven, decimated |
| Perc | higher, more FM | lower, the most FM, decimated |
| Clap | more repeats, darker, longer | more repeats, decimated, driven: an FX |

They are **offsets from the kit**, so they stay related to whatever the kit made of the voice.
Decays stay between 2 and 100, and a decimator above 16: a variation cannot fall silent or ring for
minutes.

**Per hit.** A variation is up to four of the LXR's own one-hit parameter locks (`AutomationNode`, as its
sequencer's step automation), read from the kit at the moment of the hit. **The kit is not written**.
The next hit on that voice puts the locks back before it triggers. One thing needed help: a drum's
drive is smoothed, and only while it sounds, so the next plain hit would have begun with the old
drive and glided for a few milliseconds. The smoother is set when a lock comes or goes. Tests: the kit is
byte-identical after every variation, and the next plain hit is the plain sound.

OCT sets the keys' velocity rather than switching banks, as before.

With FOLLOW on (the default), the selected voice follows the last DRUM key or MIDI note, so a hit
selects what SAVE+ENV and SAVE+ARP act on. SCL never changes it.

### PITCH

Every key plays the selected voice, chromatically: key F3 is MIDI note 53 and G5 is 79, plus 12 for
each octave. The hit carries the note to the engine, which is the LXR's own `osc_setBaseNote(note)`,
`coarse + note − 60`, per trigger. **No parameter is written**, so nothing can accumulate and the
patch does not change: in the emulator, 97 Hz, an octave up 192 Hz, and 97 Hz again after six octave
presses. On COARSE-tuned voices the pitch is the oscillator's: **the snare's noise does not transpose**
(an upstream TODO), and the hats' and cymbal's noisy modulators follow their oscillators but are not
tonal.

### MIDI

Note-ons take the same path as the keys, so they play and record the same way, at their own velocity.
In PITCH, a note plays the selected voice. In DRUM, the General MIDI drum notes play their voices, or
with MIDI MAP on KEYS, the 27 keys of the map above from F3, repeating past them.

## Recording

REC is a mode: `REC ON` on the status line and the REC light. Off, the keys record nothing.

Each step is one aligned 32-bit cell:

```c
uint32_t cell[7][64]; /* velocity 0-6 (0 off); tone 8-15; condition 16-19 */
```

A step changes in one write, so the audio interrupt cannot see half of an edit. The tone is 0 for the
programmed pitch, a MIDI note, or 128 plus a variation: **a step has a note or a variation**, which is
what keeps a project to 19 bits a step. A pitch of C4 is stored as 0, because it is the programmed
pitch: the same sound, so the same key removes it. Modes do not reinterpret anything: a tone is what
was recorded, in either mode. The track is the key's in DRUM and the selected voice in PITCH. A new tone
on a step keeps its condition.

**Stopped: step entry, at the selected step.**

| Press | The step |
|---|---|
| a key for an empty step | is written |
| the same key again | is removed |
| another note or variation on the track | takes the new one and its velocity |
| another track | is left alone |

AUTO STEP on GLOBAL moves the cursor on 1 to 4 steps after an entry (a chord is one entry, a removal
does not move it), across pages and round the pattern. It is off by default.

**Playing: overdub.** A hit never removes anything, so hitting the same step on every pass is safe.
It writes the step, replacing that track's note there; other tracks are untouched. While recording
the grid follows the music from page to page.

**Quantisation.** QUANTISE on GLOBAL:

| | A hit goes to |
|---|---|
| NEAREST (default) | the step sounding if it is in its first half, otherwise the next |
| BEHIND | the step sounding |

Half is half of that step's real length, swing included, and the end of the pattern rounds to step 1.

**No double trigger.** A hit rounded up was heard from the key, then the step would play it a moment
later. So that step skips that track, once. The skip lasts for one step whatever happens: a hit that
arrives too late to be useful (the step fired first) is forgotten, and cannot silence a later one.

## Kits

A kit is a set of LXR parameters. There are no samples and no stored parameter tables: a kit is a
function of **(character, seed, WILD)**, so the 16 factory kits cost a row of eight numbers each, and
the whole system 2.4 KB. `seq/pocket_kit.c` is a compact port of CLATTER's `kitgen.ts`:

- **Archetypes** know how the parameters relate: a kick is a low sine with a fast, deep pitch sweep, a
  snare tone under noise, a hat a high carrier with two noisy FM modulators through a high-pass, a
  clap the snare voice's noise with envelope repeats, a cymbal the same at length; toms keep their
  pitch, percussion and zaps go further. Eight recipes of about 20 ranges and menus each, three
  bytes an entry.
- **Characters** lean them: CLASSIC, ANALOG, DIGITAL, INDUSTRIAL, MINIMAL, GLITCH, TECHNO, ELECTRO, DUB,
  NOISE, METALLIC, SOFT, BROKEN, ACID, LO-FI, CHAOS. Each is wildness, decay, drive, FM, decimation,
  brightness, tuning and resonance.
- **The family** is what makes a set of voices a kit. Drawn from the seed alone, it gives every voice
  the same root (the three drums on one minor pentatonic), length, brightness and dirt.
- **WILD** is how strictly a voice follows its archetype: narrow at 0, as written at 50, and above
  that wider ranges, open menus, voices becoming other archetypes, swells, sirens and rolls. Level,
  drive and resonance are not widened: wild is different, not louder. **The kick stays a kick at any
  WILD**, so a rhythm keeps its anchor.

**Three ways to design silence are prevented by rule**: a low-pass over a noise voice, a high-pass over
a drum, a pulse wave with no width. CLATTER finds these by rendering each voice (`kitcheck.ts`),
which firmware cannot do; rendering every kit in the host tests is how the last two were found here.

**PRESETS** browses INIT (the firmware's init kit, the sound at power-on) and the 16 factory kits.
Nothing is stored, so a browse never overwrites anything.

**ENV** makes a new kit in the character PRESETS is on, or CLASSIC from INIT. If KIT SEED or WILD
moved, from those; otherwise from the next seed. So a kit can be regenerated from the seed on screen.

**SAVE+ENV** redraws one voice of the same family; `take` counts, so each press differs. CH and OH are
one voice, so on either, both change.

**The rhythm is never touched**, by any of these.

**Applying a kit.** The generator runs on the control task, into a buffer, and at most 12 changed
parameters per control poll go to the engine through its event ring, so the audio interrupt does at
most the bounded work it always did. With a pattern playing, emulated CPU peaks at 29.5 % while a kit
loads (28.6 % playing), with no overruns or dropped events.

**Protecting unsaved sounds.** A + after the name means hand edits. Undo takes back any ENV, SAVE+ENV
or browse, edits and all. A turn through the kits with PRESETS is one undo, whose way back is the kit
it started from, however many it passed.

Kits do not touch LFOs or the FX bus: the first are off in every kit, the second is not in this build.

## Patterns

**ARP** uses the existing generator (`seq/pocket_gen.c`, [POCKET-SEQUENCER.md](POCKET-SEQUENCER.md)),
unchanged: the first pattern is bit-identical to the previous firmware's. Kicks anchor, snares accent,
hats subdivide, toms and percussion add movement. If a setting moved, from those settings; otherwise
from the next seed.

**ALGORITHM** selects CLATTER's 11 styles: MACHINE, ELECTRO, BREAKS, JUNGLE, HALFTIME, BROKEN, GARAGE,
INDUSTRIAL, GLITCH, POLYMETER, DUB. They were used rather than the six names suggested (STRAIGHT,
SYNCOPATED, BROKEN, SPARSE, DENSE, EXPERIMENTAL), because they already are different rhythmic
structures rather than seeds: each has its own lanes per instrument, and two are Euclidean. SPARSE and
DENSE are GLOBAL's DENSITY; EXPERIMENTAL is high COMPLEX. Moving ALGORITHM counts as a setting moved,
so the next ARP plays the same seed in the new style.

**SAVE+ARP** replaces one track: the same rules, from `(seed, track, take)`. It returns neither an empty
track nor the rhythm it replaced: a lane of certainties, such as a MACHINE kick, can only draw itself,
so after eight tries it falls back to a Euclidean figure. Notes on that track go with the old rhythm.

**LFO** applies short lists of musical edits (add, remove, shift, velocity, accent, swap and at
higher strength density, rests, recombining, rotating and reversing) to a copy, so the pattern stays
related. Notes move with their hits. **Pitch changes** are a new edit: at LOW and MED only on a track
already playing notes, so a rhythm of programmed pitches keeps its sound; at BIG on any track, up to
an octave either way.

**SAVE+LFO** does one track, which matches what SAVE means with ENV and ARP. Locked tracks stay out
of ARP and LFO, and all of this is deterministic: the same seed gives the same result.

**Past one page** a pattern is a phrase ([POCKET-SEQUENCER.md](POCKET-SEQUENCER.md)).

**Not implemented.** Per-track lengths were secondary and are deferred. A/B/C/D pattern variations were
dropped.

## Undo and redo

**SAVE+OCT−** undoes and **SAVE+OCT+** redoes, one history in the order things happened: UNDO and REDO
on the status line, NOTHING TO UNDO and NOTHING TO REDO at the ends.

| Undoable | As |
|---|---|
| ENV, SAVE+ENV, PRESETS | one action. A turn through PRESETS is one |
| ARP, SAVE+ARP, LFO, SAVE+LFO | one action |
| A knob on SOUND or MIXER | one action per turn: within 1.5 s on one parameter |
| A step's velocity or condition | the same, per step |
| Length | the same |
| A step entered stopped | one action; a chord is one |
| A live take | one action: hits less than 2 s apart, until PLAY or REC |
| Loading a project or an old save | one action, kit, pattern and settings |

Tempo, swing and the rows of GLOBAL are not actions, but a load's change to them goes back with it.

**Nothing is regenerated.** What is recorded is what changed, never how: a word per changed step or
parameter in a ring of 4,096 words, 16 KB of RAM, and the oldest actions go when it is full. So a knob
turn costs one word and a new 64-step pattern what actually differed. A new edit after an undo forgets
what redo held; an action that changed nothing does not. Nothing reaches Flash, and a restart starts
with no history.

The generators' seeds go back with their results, so ARP after an undo plays what it played the first
time. The mutation and take counters do not: those keep moving, so the next one is a different one.


## GLOBAL

GLO opens it; GLO again opens SYSTEM, the diagnostics, which are as they were. KNOB 1 picks a row,
KNOB 2 changes it. Every row works, and is tested at both of its ends. These settings last until
power-off.

| Row | Values | |
|---|---|---|
| KEYS | DRUM, PITCH | as SCL |
| OCTAVE | −3 to 3 | as OCT in PITCH |
| KEY VEL | 1-127 | the keys' velocity |
| FOLLOW | OFF, ON | the selection follows hits |
| QUANTISE | BEHIND, NEAREST | live recording |
| AUTO STEP | 0-4 | after a stopped entry |
| TEMPO | 30-300 | as KNOB 2 on PATTERN |
| SWING | 50-75 | as KNOB 3 on PATTERN |
| LENGTH | 16, 32, 48, 64 | as SAVE+KNOB 3 on PATTERN |
| STYLE | of 11 | as ALGORITHM |
| DENSITY, COMPLEX | 0-100 | the generator |
| SEED | 0-9999 | the next ARP |
| MUTATE | LOW, MED, BIG | LFO's strength |
| KIT SEED | 0-9999 | the next ENV |
| WILD | 0-100 | the next ENV and SAVE+ENV |
| MIDI CH | ALL, 1-16 | input channel |
| MIDI MAP | GM, KEYS | DRUM's note map |

**Left out, because they would not work:** MIDI output and thru, external clock (the firmware sends
nothing and the clock is the audio's), a velocity curve, and a default tempo (these settings are not stored).

## The screen

White on black. Orange means selected, sounding or recording, and nothing else. Colours are RGB565:
the display is a colour panel, read from the emulated LCD, and **how this orange looks on a real panel
has not been seen**.

| Where | What |
|---|---|
| Band 1, the status line | `KEYS: DRUM VEL 127  REC ON` or `KEYS: PITCH OCT 1  REC ON`, REC in orange. In orange for a moment: what a control just did. An audio overrun is the one full orange band |
| Voice line | the selected voice, in orange; on PATTERN the step, and what it holds (`S17 N67 1:3`, `S5 PA 25%`); elsewhere the DRUM sound last played (`PERC B`) |
| PATTERN | PAGE 2/4, and a block a page: orange where it is playing, white on screen, grey in use. The step grid. A note is an orange bar; a variation hollow (A, X, XB) or striped (B, XA); a condition an orange corner. `X` is PERC on D3 and CLAP on SN |
| MAIN | transport, tempo, swing, style, hits, a row of the pattern, the kit |
| Footer | what the controls do here; with SAVE held, the chords, in orange |
| SYSTEM's footer | `BOOT+HOME SAFE`: how to reach SAFE MODE |

## Size

`tools/flash-map.py` breaks it down; the figures below are for this firmware, 0.6.0.
[POCKET-STORAGE.md](POCKET-STORAGE.md) has what a project costs. Both builds `--replace --single-precision`:

| | 0.5.0 (4d8e2f4) | 0.6.0 | Change |
|---|---|---|---|
| App image | 433,628 | **437,436** | +3,808 |
| Headroom under 453,921 | 20,293 | **16,485** | −3,808 |
| Over the 16,384 hardware-fix margin | 3,909 | **101** | |
| `.ram0_data` (SRAM code and data) | 105,564 | 103,740 | −1,824 |
| `.ram0_bss` | 289,292 | 309,964 | +20,672 |
| Runtime heap (SDK minimum 49,152) | 128,684 | **109,836** | −18,848 |

**The margin is kept by 101 bytes.** The next feature needs flash found for it first.

| RAM | Bytes |
|---|---|
| Undo history | +16,404 |
| Two patterns of 64 steps with conditions: 2 × 1,796 for 2 × 224 | +3,144 |
| The sector in RAM, 4,096 for 3,104 | +992 |

The heap falls by less than bss rises because the init-only parameter table moved out of SRAM.

| Flash | Bytes |
|---|---|
| Panel: pages, conditions, 27 keys, one history, PROJECTS, old saves | +3,316 |
| Store: nine projects in four sectors, and reading old banks | +698 |
| History | +586 |
| Generator: phrases, 64 steps, offset by tighter style tables | +175 |
| Engine and sequencer: variations, conditions, on the audio path | +670 |
| **Recovered** | |
| `parameterArray_init` as a table of 3 bytes an entry, checked against upstream's | −1,637 |
| | **+3,808** |

Of the image, 359 KB is the binary SDK. The upstream LXR file behind the table is included unchanged;
`test_engine_packed` compares what the two fill in, entry by entry, and renders voices bit-identical.

**Stack**, from the frames in the linked ELF (`tools/stack-depth.py`):

| Path | Bytes |
|---|---|
| a save, deepest, through the SDK's driver and RTOS | 776 |
| `pocket_panel_draw_band`, through a Flash read on PROJECTS | 644 |
| `pocket_gen_track` | 304 (was about 500: no arrays on the stack now) |
| `pocket_gen_mutate` | 168 |
| `pocket_kit_voice` | 144 |
| audio interrupt: render, deepest | 420 |
| audio interrupt: the sequencer firing a variation | 340 |

There is no division on the audio path.

**CPU**, in the emulator at 240 MHz:

| | 0.5.0 | 0.6.0 |
|---|---|---|
| Stopped | 28.1 % | 28.1 % |
| Playing, mean | 28.6 % | 27.9 % |
| Playing, peak | 29.4 % | 29.2 % |
| While a kit loads, peak | 29.5 % | 30.3 % |
| 64 dense steps with hits on every variation, 300 BPM: mean, peak | | 28.7 %, 30.6 % |

0 overruns and 0 dropped events through all of it. **Not a device measurement**: the emulator counts
about one instruction per cycle and models no cache or Flash wait states.

## Tests

| Command | Covers |
|---|---|
| `make test` | `test_panel`, 708 checks on the real engine: everything below by the panel's own controls, on a model of the Flash chip |
| | `test_seq`, 23,986 checks with AddressSanitizer and UBSan, including every length playing in order, wrapping, shortening under the playhead, quantising and skips across pages; every condition at every length over 24 passes and after STOP and PLAY; phrases in every style and length; mutation to the last page; steps keeping their condition. The golden pattern hash is **unchanged** |
| | `test_history`, 173,786 checks: order, redo, merging, overflow, and random actions against a shadow of every state |
| | `test_store`, 6,357 checks: [POCKET-STORAGE.md](POCKET-STORAGE.md) |
| | `test_kit`, 23 checks; `test_midi`; engine, libm and table checks; the parameter table against upstream |
| `make emu-test EMU_WASM=…` | `tools/emu-test.mjs`, below |
| `python3 tools/check-firmware.py` | size, margin, layout, placement, and that the build is `--single-precision` |
| `node tools/emu-golden.mjs` | the firmware's own audio, each voice hashed: **bit-identical to 0.5.0**, all seven voices |
| `python3 tools/check-audio-path.py` | nothing the audio interrupt reaches is in Flash: 99 functions |
| `python3 tools/stack-depth.py` | the stack figures above |

**Variations, measured.** Every one of the 27 keys is rendered on the init kit and all 16 characters,
and compared by length, brightness and envelope, level left out: 0.14 is about 15 % longer, or two
semitones. 561 comparisons: each variation against its bank's others, PERC against HIGH TOM and CLAP against
SNARE. **12 of 561 fall below 0.14**, scattered, not one position: BROKEN's PERC against its A and B, CHAOS's
PERC against its HI TOM, a few of the moderate A variations on single kits. On those, that variation
is hardly one. None of the 27 is silent on any kit. That is measurement:
**nobody has listened to these**.

### Emulator results

The package below, the emulator's WebAssembly core (the deployed `fm1_web.wasm`), 240 MHz, **361 of 361
checks**. All input is through the emulated panel; text and grids come from the LCD's pixels, pitch and
timing from the emulated audio.

| Asked for | Result |
|---|---|
| All 27 keys | each sounds, on its voice, named on screen, in the order of the map |
| Variations recorded | stopped and live, on the host; played back the key's sound exactly |
| 16 to 64 steps | PATTERN's blocks visit every page in order and wrap, at each length |
| A long pattern repeats | 64 steps recur at 705,888 samples, from the emulated audio |
| Conditions | 1:2, 2:2, 1:3, 2:3, 1:4, 4:4 heard on exactly the right passes, by onsets in the audio, again after STOP and PLAY |
| One history | sound, pattern, kit back in that order and forward exact, to the ends of it, both messages shown |
| Chords | SAVE with each of OCT−, OCT+, ENV, ARP, LFO opens no save page |
| A project | saved, changed, loaded while playing, kit, both pages, note, condition, tempo, swing, length; a restart; a load undone and redone |
| Flash | only our sectors changed; factory data intact; the vendor's area untouched; undo writes nothing |
| Damage | a corrupted sector, an erase cut half way, a save cut before commit, both copies corrupted |
| Old saves | listed, loaded, byte for byte untouched by a project beside them, erased only on a second press |
| Refusals | foreign data, a stock sample bank over the sectors, SAFE MODE |

All earlier checks still pass: step timing, tempo and swing by measurement, SOUND and MIXER, DIN MIDI, 20 s
of the sequence with hits through it, no reset, 0 overruns and 0 dropped throughout, SAFE MODE silent
with every new control tried, and the stock package failing the identity check.

**Not shown by any of this.** That it sounds good: kits and variations were checked by rule and by
measuring renders, **not by ear**, and whether a kit change clicks on a ringing voice has not been
listened to. Real timing, the real display and its orange, encoder direction and acceleration, key
feel. USB MIDI and BLE were not run: the emulator raises no USB controller interrupt. The update path was
not exercised, though it is unchanged. The emulator's core was scripted; the browser page itself was
not driven by hand. The website still offers 0.5.0.

## Known limitations

- GLOBAL's playing preferences are not saved, and a restart returns the init kit and seed 1 until a
  project is loaded: [POCKET-STORAGE.md](POCKET-STORAGE.md).
- The keys have no velocity, and SELECT cannot be held: above.
- Seven tracks: PERC shares a voice and a track with the high tom, CLAP with the snare, and neither
  sounds with its partner. CH and OH are one voice with a shared sound.
- A step can have one note or variation per track. Hats on one step still choke: an open hat that
  sounds wins, and one its condition leaves out takes nothing.
- The snare's noise does not transpose. Very high notes on a voice clamp at note 127.
- The history holds about 4,096 changed things, not a number of actions: 21 levels of the densest new
  patterns and kits together. Tempo and swing alone are not undoable.
- While playing, recording cannot remove a step, and steps are 16ths, up to 64, no chaining.
- Recording's timing is when the control task reads the key (after the scanner's debounce), then 16ths.
- No MIDI clock, no MIDI output, no thru.
- At high WILD a kit can be odd: that is the point. 1 voice of the 1,344 rendered rang past 2
  seconds. Kits have no LFOs or FX.
- HOME+ARP stays the reserved exit gesture, so ARP does nothing while HOME is held.
