# Saving projects

CLATTER Pocket saves 9 projects in the FM-1's user Flash, and they survive a restart. A project is the
whole musical state: the kit and the pattern together.

**Tested on the host, on a model of the Flash chip, and in the FM-1 emulator. Nothing here has run on
a device, and a device's Flash is the one thing an emulator cannot stand in for: read
[What is not known](#what-is-not-known). The firmware stays UNVERIFIED.**

## To use it

| Do | |
|---|---|
| Tap **SAVE** | opens PROJECTS. In the browser emulator, `8` |
| **SELECT** or KNOB 1 | choose a slot |
| Tap **SAVE** again | store here. On a slot in use it asks first: tap again while OVERWRITE: SAVE AGAIN shows. Only when stopped: STOP TO SAVE otherwise. It says SAVED 3 |
| **OCT−** | load this slot, also while a pattern plays |
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

GLOBAL's playing preferences (key velocity, FOLLOW, QUANTISE, AUTO STEP, MIDI) are not part of a
project: they are how you play, not what was played.

## How many, and why nine

There are four sectors, 16,384 bytes, and their addresses have not changed: that is what the audit
below proved safe. A project cannot be smaller than its worst case, every step of every page in use:

| | Bytes |
|---|---|
| The kit | 247 and a header of 8: 256 with the old slot's padding |
| Tempo, swing, generator, length | 12 |
| 448 steps at 19 bits (velocity 7, note or variation 8, condition 4) | 1,064 |
| **A project** | **1,332, in a record of 1,354** |
| A sector's header | 32 |

Three fit a sector, 32 + 3 × 1,354 = 4,094 bytes. One sector must always be free for a save to go to,
which is what makes a save safe, so three sectors of three: **9 projects**. Keeping a pattern's cells
as they are in RAM, 32 bits a step, would have made 2 a sector and 6 projects, which is why the
format packs them. A step has a note *or* a variation, not both: that is the bit that took it under.

## Saves from before projects

Firmware 0.5.0 wrote banks of 12 kits and 12 patterns to the same sectors. **They are not converted,
and they are not overwritten.** Twenty-four old records do not fit nine projects, so no automatic
conversion could keep them all.

On a chip that has them:

- each kind's current bank keeps its sector, untouched, byte for byte (`test_store`, 50 saves beside
  them, and at every power cut);
- its superseded copy, which no page could ever show, is what gives the projects their room;
- PROJECTS lists them after the nine slots, OLD KIT 2, OLD PAT 3. OCT− loads one, an old pattern as one
  page of 16 steps. A kit alone leaves the pattern; a pattern alone leaves the kit;
- with both kinds present only slots 1 to 3 can save, and 4 to 9 say HELD BY OLD SAVES;
- the last row, ERASE OLD SAVES, gives their sectors back. It asks first, and it is the only thing that
  ever erases them. Load what you want into a project first: there is no way back.

So the migration is manual and safe: load the kit, load the pattern, save the project. The cost is
one sector of slots until you decide.

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

1. **Nothing happens until asked.** No Flash access at start-up, in SAFE MODE, or at all until PROJECTS
   opens. Undo and redo never touch it. `check-firmware.py` and the emulator tests hold this.
2. **Only blank or ours.** Opening the store reads all four sectors. Anything that is neither all
   0xFF nor carrying one of our magic numbers closes it: `FLASH IN USE: SAVING OFF`, and those sectors
   are never erased or written. Saving stays off on such a device; the instrument is unaffected.
3. **The partition must be exactly the expected size**, or no sector here means what it should.
4. **A sample bank reaching these sectors closes the store**, because a sample can contain a blank
   sector, so blank is not proof of free.
5. **The sector last written is never the one erased.** See below.
6. **Saving only when stopped.** Erasing a sector silences audio for its length, and that is not
   worth a dropout in a performance. Loading writes nothing, and works while playing.

### What a save does

A sector holds three projects, a *page*, and a header saying which page it is and its generation.

1. Read the page of the slot into RAM, and fill the slot.
2. Choose a free sector: blank if there is one, otherwise the oldest that is nobody's current copy.
   It is never a current copy, so never the sector last written, and never an old bank.
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
chip, or a good old bank beside it, which is what a save cut off in its erase looks like. The first
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

**None of this has run on a device.** The emulator's chip is an accurate model of a NOR part with
instant erases and programs; a real one is slower, and its power-cut behaviour is not half a page.

- **Whether these four sectors are blank on your unit.** If a unit's history left anything there,
  saving turns itself off and says so. That is the safe failure, and nothing can be said for sure
  until a device is looked at.
- **Audio during an erase.** The SDK says it is silenced. How long, and how it sounds, is unmeasured.
  A save now also rewrites the two other projects of its sector, as data, not as a longer erase.
- **Whether the vendor's loader leaves these sectors alone** on updates, and on migrating to V15 or
  Open. Keep nothing here you cannot afford to lose; there is no export.
- **What V15 does with data in the far end of its VM area** after a return to it. It would be the same
  as what a large Open sample bank leaves there. Unverified, as PRE-FLASH-AUDIT.md's VM section is.
- **Unit-to-unit differences.** The USR area's extent is documented for V13 and V15 by the emulator's
  author and in `WebUpdate.js`, not by M-VAVE.
- The browser emulator keeps Flash per package, by its hash: a new build starts with no saves.

FIRST-HARDWARE-TEST.md has the first device check, as its own step, after everything else.

Cost: the store is 1,476 bytes of Flash and 4,146 of RAM, most of it one sector's buffer. A save's stack is 776 bytes through the SDK's driver and the RTOS.
`tools/check-firmware.py` holds the placement.
