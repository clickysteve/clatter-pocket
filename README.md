# CLATTER Pocket

**CLATTER's LXR-02 drum synthesis as custom firmware for the M-VAVE FM-1.**

CLATTER Pocket brings [CLATTER](https://github.com/clickysteve/clatter), the generative drum
synthesizer built on the Erica Synths LXR-02's open-source sound engine, to the M-VAVE FM-1
pocket synthesizer, through M-VAVE's open firmware SDK
([fm1_open](https://github.com/m-vave/fm1_open)).

> CLATTER Pocket is an independent project. It is **not** an Erica Synths or M-VAVE product
> and is not endorsed by either. It contains a modified version of the LXR-02 firmware's
> sound engine. "Erica Synths", "LXR" and "LXR-02" are trademarks of Erica Synths; "M-VAVE"
> and "FM-1" of their owner.

## Status

| | |
|---|---|
| LXR engine (all six voices, seven triggers, the firmware's parameter handler, modulation, locks) | runs natively; 177 host checks pass; matches CLATTER's engine to within 3 LSB; the FM-1's packed tables unpack byte-identical |
| FM-1 firmware, CLATTER Pocket **replacing** the stock instrument application | **builds and packages** reproducibly with the FM-1 toolchain: 433,628-byte image (20,293 bytes under the 453,921 limit), stock flash layout, all SDK firmware checks pass, user partition untouched, installer page ([FM1-REPLACEMENT.md](FM1-REPLACEMENT.md)) |
| Pre-flash audit | done ([PRE-FLASH-AUDIT.md](PRE-FLASH-AUDIT.md)); first-test procedure: [FIRST-HARDWARE-TEST.md](FIRST-HARDWARE-TEST.md) |
| FM-1 firmware, CLATTER Pocket **beside** the stock instrument (HOME + ARP) | compiles, links, passes the checks, but is 53,671 bytes too large for the flash layout |
| On the device | **nothing has run on hardware** |
| Website | <https://clatter-pocket.allmyfriendsaresynths.com/>: browser demo, and a working installer for the UNVERIFIED experimental release ([docs/WEBSITE.md](docs/WEBSITE.md)). The install path has been tested against a simulated device only |
| Sequencer and generator | 16 steps, seven tracks with notes, CLATTER's styles, whole-pattern and one-track generation, mutation; **tested in the FM-1 emulator** ([docs/POCKET-SEQUENCER.md](docs/POCKET-SEQUENCER.md)) |
| Playing it | DRUM and PITCH keyboards, live and step recording, 16 kits and on-device kit generation from LXR parameters, GLOBAL settings; **tested in the FM-1 emulator** ([docs/POCKET-CONTROLS.md](docs/POCKET-CONTROLS.md)) |
| Saving | 12 kits and 12 patterns in user Flash, written only to sectors checked blank or its own; **tested in the FM-1 emulator, not on a device** ([docs/POCKET-STORAGE.md](docs/POCKET-STORAGE.md)) |

## What it does on the FM-1 (once installed)

In the replacement firmware CLATTER Pocket is the instrument: the panel below about 1.5 s
after power-on (hold HOME then for SAFE MODE, engine off), MIDI notes play as the keys do, and USB/Bluetooth audio is mixed in as before. (The overlay build would open it with **HOME + ARP** beside
the stock instrument; it does not fit the flash.)

| Control | Action |
|---|---|
| note keys | DRUM: each key an instrument. PITCH: the selected voice, chromatically (SCL switches) |
| OCT− / OCT+ | PITCH: octave. DRUM: how hard the keys hit |
| PLAY, REC | start or stop; recording on or off |
| ENV, ARP, LFO | a new kit, a new pattern, mutate. Held SAVE: the selected voice only |
| PRESETS, ALGORITHM | the kit, the pattern style |
| SAVE+EDIT, SAVE+SEQ | saved kits, saved patterns |
| SELECT | select DRUM 1-3, SNARE, CYMBAL, CL HAT, OP HAT |
| HOME, SEQ, EDIT, FX, GLO | pages: MAIN, PATTERN, SOUND, MIXER, GLOBAL then SYSTEM |
| KNOB 1-4 | PATTERN: velocity, tempo, swing, step. SOUND: pitch, decay, filter, drive. MIXER: level, pan. GLOBAL: row, value |
| MASTER | master volume (stock) |
| HOME + ARP | (overlay build only) back to the stock instrument |

**[docs/POCKET-CONTROLS.md](docs/POCKET-CONTROLS.md) is the guide to playing it**, with a quick start.
docs/POCKET-UI.md has the interface, how to load it in the FM-1 emulator, and its results. The encoder positions come from the emulator, not a device.

![The Pocket panel, rendered by the host tests](docs/panel.png)

## Quick start (host)

```
git clone --recurse-submodules https://github.com/clickysteve/clatter-pocket
cd clatter-pocket
make                 # tests
make render          # build/host/kick_sweep.wav, kit_demo.wav
make bench
make compare CLATTER_DIR=../clatter
```

Firmware: BUILDING.md. **Read HARDWARE.md before flashing anything.**

## Documents

- [HARDWARE.md](HARDWARE.md) - the FM-1 and its SDK: verified facts, sources, update and
  recovery, open questions
- [BUILDING.md](BUILDING.md) - host and firmware builds, toolchain
- [ARCHITECTURE.md](ARCHITECTURE.md) - layers, real-time design, porting decisions, CPU and memory
- [COMPATIBILITY.md](COMPATIBILITY.md) - numerical comparison with CLATTER, kits, formats
- [FM1-REPLACEMENT.md](FM1-REPLACEMENT.md) - the replacement firmware: flash map, what was
  removed, what remains, sizes, RAM, checks
- [PRE-FLASH-AUDIT.md](PRE-FLASH-AUDIT.md) - the pre-flash audit: findings, evidence, risks
- [FIRST-HARDWARE-TEST.md](FIRST-HARDWARE-TEST.md) - first installation, test and recovery
- [ROADMAP.md](ROADMAP.md) - milestones A-F
- [docs/WEBSITE.md](docs/WEBSITE.md) - the website, its browser demo, and how a release enables installing
- [docs/FM1-COMMUNITY.md](docs/FM1-COMMUNITY.md) - how FM-1 firmware is distributed, updated and recovered

## Layout

```
lxr/              LXR-02 firmware DSP (unchanged) + CLATTER's shim and bridge
engine/           real-time engine host (event ring, mixing, Q23 output)
ui/               the Pocket panel (platform-independent)
host/             tests, benchmark, WAV renderer, CLATTER comparison
firmware/fm1/     FM-1 adapters (replace/: the replacement app), fm1_open patches, build scripts
third_party/      fm1_open (submodule, pinned)
tools/            generators, toolchain setup, size estimate, website build
site/             the website (docs/WEBSITE.md)
```

## Licence

GPL-3.0-only with the LXR firmware's additional terms (NOTICE.md). Firmware builds also
link M-VAVE's binary SDK under its own terms and the fm1_open linking exception
(THIRD_PARTY_NOTICES.md).
