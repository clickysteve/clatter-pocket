# Saving projects

CLATTER Pocket saves 9 projects in the FM-1's user Flash, and they survive a restart. A project is the
whole musical state: the kit and the pattern together.

**Works on one real FM-1, at 0.9.0:** the leftover V15 data was erased on the second press, a project
saved, and it came back after a power-off. Also tested on the host, on a model of the Flash chip, and in
the FM-1 emulator. One unit is not many, and an emulator cannot stand in for a device's Flash: read
[What is not known](#what-is-not-known). The firmware stays UNVERIFIED.**

## To use it

| Do | |
|---|---|
| Tap **SAVE** | opens PROJECTS. In the browser emulator, `8` |
| **SELECT** or KNOB 1 | choose a slot |
| **KNOB 2**, **KNOB 3** | name the project: KNOB 2 picks the letter's place, KNOB 3 the letter (a space, A-Z, 0-9, -). The status line shows the name, `NAME ABC..... K2 MOVE K3 A-Z`: an empty place is a dot, and the place KNOB 3 changes is an orange block. A save writes it, and the slot's row shows it in place of the kit. A load brings it back; a restart starts with none |
| Tap **SAVE** again | store here. On a slot in use it asks first: tap again within 3 seconds, while OVERWRITE: SAVE AGAIN shows. Only when stopped: STOP TO SAVE otherwise. It says SAVED 3 |
| **OCT+** | load this slot, also while a pattern plays |
| **OCT−** | erase this slot. It asks first: press again while ERASE: OCT- AGAIN shows. Only when stopped |
| HOME, or any page button | leave. Keys, PLAY, ENV, ARP and LFO work here as everywhere |

A tap is SAVE pressed and released with nothing else happening. Holding SAVE with another button, a
knob or a key is a chord, and never opens or stores anything.

A load is one undo: SAVE+OCT− takes all of it back, kit, pattern and settings, and writes nothing.

| In a project | |
|---|---|
| The kit | all 246 sound parameters: every voice, levels and pans. Which factory kit or character it came from, its seed and WILD, and whether it was hand-edited |
| The pattern | every step of all four pages, including pages past the length: velocity, note or variation, condition |
| Length | how many pages play |
| Tempo and swing | |
| The generator | DENSITY, COMPLEX, SEED, STYLE, MUTATE, the track locks |
| Its name | 8 letters, in bytes every save before 0.9.2 left zero: those have none, and show their kit. A letter that is not one is a space |

GLOBAL's playing settings are not part of a project: they are how you play, not what was played.
They survive a power-off on their own terms, below.

## GLOBAL's settings

KEYS, OCTAVE, KEY VEL, FOLLOW, QUANTISE, AUTO STEP, MIDI CH and MIDI MAP come back after a power-off.
TEMPO, SWING, LENGTH and the generator rows are the project's, and come back with a load.

| | |
|---|---|
| Stored | with every project save or erase, and alone by a tap of **SAVE on GLOBAL** (SETTINGS SAVED). Only when stopped, as any save |
| Where | in the header of the sector the save writes: the 12 bytes every firmware before 0.9.2 wrote as zero, under the sector's CRC. No format change: every save from before loads, and holds no settings |
| SAVE on GLOBAL | writes the newest page again, through the same steps as a project save: at most one erase, and a power cut loses only this |
| Read | once, at start-up, after the engine is running, and never in SAFE MODE: the newest good sector's settings. Anything foreign, damaged, from before 0.9.2, or out of range means the default, and says nothing |
| Not stored | when the store is closed: FLASH IN USE says to open PROJECTS, and nothing is written |

On a device: 0.9.2 kept all eight after a power-off on the owner's unit.

## How many, and why nine

There are four sectors, 16,384 bytes, and their addresses have not changed: that is what the audit
below proved safe. A project cannot be smaller than its worst case, every step of every page in use:

| | Bytes |
|---|---|
| The kit | 247 and a header of 8: 256 with the old slot's padding |
| Tempo, swing, generator, length | 12 |
| 448 steps at 19 bits (velocity 7, note or variation 8, condition 4) | 1,064 |
| A name | 8 |
| **A project** | **1,340, in a record of 1,354** |
| A sector's header | 32 |

Three fit a sector, 32 + 3 × 1,354 = 4,094 bytes. One sector must always be free for a save to go to,
which is what makes a save safe, so three sectors of three: **9 projects**. Keeping a pattern's cells
as they are in RAM, 32 bits a step, would have made 2 a sector and 6 projects, which is why the
format packs them. A step has a note *or* a variation, not both: that is the bit that took it under.

## Saves from before projects

Firmware 0.5.0 wrote banks of 12 kits and 12 patterns to these sectors, and firmware up to 0.9.0 could read
them. 0.9.1 cannot: that code went to pay for erasing a project, because no real FM-1 has such a bank. The
first firmware ever installed on a device was 0.8.1. To the store a bank now looks like anything else
that is not its own: FLASH IN USE, and nothing happens to it unless the player says so.

## Is it safe to write to this device's Flash?

Conditionally, and that condition is what the design is built around. The firmware had no code that
could touch the user partition (FM1-REPLACEMENT.md), on purpose, until this audit.

### What exists

| Question | Answer | Source |
|---|---|---|
| A storage API? | Yes: `fm1_flash_size`, `_read`, `_write`, `_erase`, for the control task, at 4 KiB sectors. Not transactional: it says to use CRC, versions and two sectors | `sdk/include/fm1.h` |
| A file system? | No. Raw sectors. The SDK's VM holds its own settings and is not for applications | HARDWARE.md, PRE-FLASH-AUDIT.md |
| How much? | 536,576 bytes, 131 sectors | package `USER` record |
| Does the running firmware have data there? | **No**, but it is not free: see below | |
| Survive a restart? | Yes: it is NOR Flash | |
| Survive a firmware update? | By design: an update writes `flash.bin`, 0 to the end of the image, and these sectors are past it. **Not proven**: the vendor loader's erase behaviour is undocumented, and M-VAVE warn migration can erase user data | `Package.cs`, `WebUpdate.js`, HARDWARE.md |

### Who owns what

The stock fm1_open layout claims the **whole** partition: there is no unused area to move into.

```
0x7C000  USER starts
         projects, settings, staging    17 sectors    stock Open     not touched
0x8D000  sample area                                  stock Open
           ...                                        V15's VM, 0x93000..0xE9000
0xE5000    CLATTER POCKET: 4 sectors                  <- here
0xE9000    V15's BTIF                                 not touched
0xEA000    the vendor's USR area, to 0xFC000:         not touched
           FACTORY VOICES, which updates leave alone
0xFA000    Open's DX7 library, 4 sectors              not touched
0xFE000    Open's tuning, 1 sector                    not touched
0xFF000  key_mac                                      not touched
```

**The vendor's USR area is the finding that matters.** It is inside what stock Open calls the sample
area, no package declares it, and real units carry the factory bank there, because updates do not
write it (fm1-emu's `docs/stock-firmware.md`, and its V15 boot, which shows data at 0xEE000 and 0xFA000).
The first sectors chosen for this work were inside it, just under Open's library. They were blank in
the emulator. A blank-sector test alone would have passed them.

**0xE5000..0xE8FFF** was chosen because it is:

- below all of the vendor's USR area, and BTIF;
- above stock Open's projects, in space it uses only for a sample bank over 360 KB;
- the far end of V15's 352 KB VM, which an idle V15 run in the emulator does not reach;
- a place stock Open already expects to find data it does not recognise: it erases every sector
  before it writes a sample there (`samples.c`), so a later bank takes the sectors and costs nothing
  but our saves, and nothing in it ever reads past its own bank.

### The rules the code keeps

1. **Nothing is written until asked; one bounded read of our sectors at start-up.** At the end of
   start-up, after the engine is running and never in SAFE MODE, the firmware reads our four
   sectors, and nothing else, for GLOBAL's settings. A read cannot change anything, it uses the same
   driver PROJECTS uses, the updater and audio are already up, and a bad sector only means the
   defaults. Otherwise no Flash access until PROJECTS opens, or SAVE on GLOBAL. Undo and redo never
   touch it. `check-firmware.py` and the emulator tests hold this.
2. **Only blank or ours, unless the player says otherwise.** Opening the store reads all four sectors.
   Anything that is neither all 0xFF nor carrying one of our magic numbers closes it: `FLASH IN USE: TAP
   SAVE`. Since 0.9.3 nothing asks: stopped, opening PROJECTS (or the next SAVE there) erases what is in
   the way, those sectors only, and the store opens. Until then they are untouched and the instrument is
   unaffected. The owner chose this after the question confused on the device. This was needed on the first device, which had been on V15, whose settings area
   runs through these sectors: FLASH IN USE came up there, and the erase and the first save worked.
   What was there is not known. V15 is expected to rebuild its settings if it is restored; that has
   not been tried.
3. **The partition must be exactly the expected size**, or no sector here means what it should.
4. **A sample bank reaching these sectors closes the store**, because a sample can contain a blank
   sector, so blank is not proof of free: `SAMPLES IN FLASH: SAVING OFF`. This one cannot be erased from here.
5. **The sector last written is never the one erased.** See below.
6. **Saving only when stopped.** Erasing a sector silences audio for its length, and that is not
   worth a dropout in a performance. Loading writes nothing, and works while playing.

### What a save does

A sector holds three projects, a *page*, and a header saying which page it is and its generation.

1. Read the page of the slot into RAM, and fill the slot.
2. Choose a free sector: blank if there is one, otherwise the oldest that is nobody's current copy.
   It is never a current copy, so never the sector last written.
3. Erase it (unless it is blank).
4. Write the whole sector, with the commit word left erased.
5. Read it all back and check its CRC.
6. Write the commit word.
7. Read it back again.

A page's copy is the committed one with a good CRC-32, right version and the highest generation. So at
every moment there is a whole copy of every page on the chip, and **a power cut can lose only the save
in progress**. A corrupt newest copy means the one before it, if it has not yet been reused. One bad
sector is what a power cut leaves, and says nothing; two, or none good, says SAVED DATA DAMAGED. What
cannot be read shows as empty, and saving still works.

A sector that is not ours is only ever overwritten in one case: a good sector of projects is on the
chip, which is what a save cut off in its erase looks like. The first
save found all four blank or ours, and rule 4 is what would have shown anyone else taking one since.

Values loaded are range-checked as well: a good CRC says what was written, not that it is sensible.

Each save erases one sector, and the free sector moves round the ones in use. Saving to one slot over
and over alternates between two sectors, so 100,000-cycle Flash gives about 200,000 saves.

### The audio path

**Found by this audit, and fixed.** Flash cannot be read while it is being written, so anything the
audio interrupt can reach must be in SRAM. The audio path called `memset`, which was in Flash: the
compiler makes clearing loops into calls to it. The emulator cannot show that: it does not stall
fetches. `fm1_flash_write` and `fm1_flash_erase` do suspend audio, but that is not something to depend
on when it can be removed. `memset` is now in SRAM, with no change to the image, and `tools/check-audio-path.py`
walks the linked firmware from the audio entry points: 99 functions, none leaving SRAM. It also fails
if the walk stops short of the voices, which it once did without saying so: a jump table's local label
was being read as the end of a function, and 32 functions were passing for all of them.

## Testing

| | |
|---|---|
| `test_store` | 6,357 checks with AddressSanitizer and UBSan, on a model of the chip whose programming only clears bits. **A power cut at every operation of a save, from twelve different histories, for projects in every sector**, each half-finishing that operation as `fm1-emu`'s FM1_FLASH_CUT does: always the old sector or the new, whole; the rest of the sector and the other sectors unchanged; nothing outside ours changes; it saves again after. The same **beside old banks**, which stand byte for byte, and **through forgetting them**, with and without a project to vouch. Foreign bytes at every position of every sector refused without a write. A bit flipped through both copies. Flash reporting failure |
| `test_panel` | the flows by the panel's own controls, on the real engine: every parameter returns, 64 steps with notes, variations and conditions, overwrite protection, STOP TO SAVE, undo and redo of a load, every slot its own, restart, out-of-range records, the page when saving is off; old kits and patterns |
| `tools/emu-test.mjs`, storage | the firmware's real writes to the emulated chip. Below |
| `tools/check-audio-path.py` | nothing the audio interrupt reaches is in Flash |

In the emulator, what is checked is the actual firmware through the SDK's real Flash driver:

- using the instrument changes no sector of the user partition, and neither do undo and redo;
- save a project with two pages, a note, a condition, tempo and swing; change it all; load it while it
  plays; SOUND and both pages show what was saved;
- undo and redo the load;
- **restart the machine with the chip's contents, and load it back**;
- only our four sectors changed in the user partition; factory-like data placed at 0xEE000 and 0xFA000
  is intact; nothing in the vendor's USR area changed;
- a corrupted sector, an erase cut half way, a save cut before its commit: the last whole copy stands;
- both copies corrupted: SAVED DATA DAMAGED, nothing loads;
- banks written as 0.5.0 wrote them: listed, loaded, a project saved beside them, the banks byte for byte
  unchanged, erased only on a second press, and every slot free after;
- another owner's data in our sectors, and a stock sample bank over them: saving goes off, the sectors
  are unchanged;
- SAFE MODE: the save controls do nothing and nothing is written.

The emulator's own power-cut option is not reachable from its WebAssembly core, so its interrupted
states are constructed, from what the host model produces.

## What is not known

One device has shown that saving, the erase and reloading after a power-off work. The rest is still open.
The emulator's chip is an accurate model of a NOR part with instant erases and programs; a real one
is slower, and its power-cut behaviour is not half a page. Cutting power during a save has not been tried on a device.

- **Whether these four sectors are blank on your unit.** They were not on the first one, which had been
  on V15. If a unit's history left anything there, saving says so and asks before erasing it.
- **Audio during an erase.** The SDK says it is silenced. How long, and how it sounds, is unmeasured.
  A save now also rewrites the two other projects of its sector, as data, not as a longer erase.
- **Whether the vendor's loader leaves these sectors alone** on updates, and on migrating to V15 or
  Open. Keep nothing here you cannot afford to lose; there is no export.
- **What V15 does with data in the far end of its VM area** after a return to it. It would be the same
  as what a large Open sample bank leaves there. Unverified, as PRE-FLASH-AUDIT.md's VM section is.
- **Unit-to-unit differences.** The USR area's extent is documented for V13 and V15 by the emulator's
  author and in `WebUpdate.js`, not by M-VAVE.
- The browser emulator keeps Flash per package, by its hash: a new build starts with no saves.

FIRST-HARDWARE-TEST.md has the device check, as its own step, after everything else.

Cost: the store is 1,476 bytes of Flash and 4,146 of RAM, most of it one sector's buffer. A save's stack is 776 bytes through the SDK's driver and the RTOS.
`tools/check-firmware.py` holds the placement.
