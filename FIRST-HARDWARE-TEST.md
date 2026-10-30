# First hardware test of CLATTER Pocket on the FM-1

A procedure for the first installation of the replacement firmware (FM1-REPLACEMENT.md,
PRE-FLASH-AUDIT.md) on one M-VAVE FM-1. **Nothing here has been done yet.** Steps marked
**UNVERIFIED** follow from the SDK's source and build, not from a device. The owner decides
whether to start, and every step can be the point to stop.

Candidate:
- `clatter-pocket-fm1-replace-UNVERIFIED.fwsc`, SHA-256
  `bcfef5946b9c45b9c6402c7c7d177df8b43d8ce1f8686d7a31fbc3029d3550e8`;
- installer `clatter-pocket-installer-UNVERIFIED.html`, SHA-256
  `20b22912dc9a12c63aecaf7e0ef0926f1ecd6c8c7fb297741ae9fa9d43086865`.

Both are produced by:

```
firmware/fm1/build.sh --replace --single-precision
python3 tools/make-installer-page.py
```

## Stop conditions (read first)

Stop testing, and do not try more installs, if any of these happen:
- the FM-1 does not show the CLATTER Pocket start screen within about 10 s of power-on;
- it restarts by itself, or keeps restarting;
- the screen stays black or frozen;
- it gets unusually warm;
- the official workbench cannot connect to it.

Then go to Recovery.

## Before installation

1. **Hardware and firmware.** Write down:
   - the unit's label and serial number;
   - the firmware identity and build the **official M-VAVE workbench**
     (https://open.m-vave.com/fm1-workbench) shows on connecting.

   fm1_open does not document hardware revisions; record what is visible. CLATTER Pocket
   installs only over fm1_open firmware (identity `FM-1_8…`, update family 7). If the unit
   runs M-VAVE's original V15, first install official Open with the official workbench (its
   supported route) and confirm it works.
2. **Back up**, with the official workbench and official Open installed:
   - every project slot;
   - the sample bank;
   - the four DX7 library groups (A to D, read and exported separately, as fm1_open's README
     says).

   Projects do not contain sample audio.
3. **Verify the backups.** Re-open the exported files in the workbench, or check their
   sizes and counts.
4. **Confirm the way back.** Keep the official workbench page open in another tab and
   confirm it offers official Open, and V15, for this unit. The official workbench is the
   restore route; nothing in this repository replaces it.
5. **Check the candidate.** Its SHA-256 must match the value above (`shasum -a 256 …`).
   The installer page shows the package hash and checks it, and the loader hash, before it
   connects.
6. **Equipment:**
   - a computer with Chrome or Edge (Web MIDI with SysEx);
   - a data USB cable;
   - headphones or a monitor at low volume;
   - a USB-MIDI or DIN MIDI source for the MIDI tests.

   Close other MIDI software. Keep the FM-1 powered throughout.

## First installation (UNVERIFIED)

The page runs M-VAVE's own update code (`sdk/WebUpdate.js`, unchanged, in a minimal page).

1. Open `clatter-pocket-installer-UNVERIFIED.html` in Chrome, served from localhost or
   opened as a file, and allow MIDI with SysEx when asked. The log must say "Package
   verified".
2. Connect the FM-1, running official Open, by USB and press **Connect FM-1**. Expected:
   "Connected: FM-1_8000014" and "Ready to install". If the page says the FM-1 does not run
   fm1_open firmware, stop and follow step 1 of "Before installation".
3. Tick the backup confirmation and press **Install CLATTER Pocket**. The updater then:
   1. arms the update in the running firmware;
   2. sends the loader-preparation request;
   3. waits for the FM-1 to reconnect as its update loader (`ota-FM-1_015`);
   4. writes the firmware;
   5. waits for the FM-1 to restart;
   6. checks that it reports this build.

   Do not unplug, close the page or sleep the computer until it says it has finished.
4. If the install stops part-way, keep the page open, reconnect the same FM-1, press
   Connect, then Install. The updater resumes from the loader. If that fails, go to
   Recovery.

## First boot

Expected on screen:
1. a black start screen: "CLATTER POCKET", "LXR DRUM MACHINE", "HOLD HOME NOW FOR SAFE MODE",
   the version and 8 hex digits (the build);
2. about 1.5 s later, the MAIN page: "CLATTER POCKET" over "KEYS: DRUM VEL 127";
3. "MIDI WAITING";
4. the seven activity cells and a level meter.

Press **GLO** twice for SYSTEM (GLOBAL, then SYSTEM): version, build, CPU, OVERRUNS, CLIPPED, DROPPED, HEAP. The emulator
shows this same sequence (docs/POCKET-UI.md); the stock interface here means the wrong package
is installed.

Record:
- the time to the start screen and to READY;
- the build digits: the first 8 of the build identity on the installer page, in upper case;
- any restart;
- the CPU and peak figures with nothing playing (the engine's cost is the same playing or
  not);
- the OVERRUNS count after one minute;
- temperature by touch after 5 minutes.

If the screen says **ENGINE ERROR**, the output stays silent. Stop and restore.

## First audio test

1. MASTER low. Press **C4**, the 8th note key: DRUM 1 sounds once at its programmed pitch.
2. Raise MASTER: the level follows smoothly. Fully down is silent.
3. The level meter moves with each hit and the DRUM 1 cell flashes; CLIPPED stays 0 at
   moderate MASTER. Listen for
   distortion, clicks and dropouts.
4. Play the keys C4 to E4: each is a different voice, and the VOICE line follows. Press **SCL**
   for KEYS: PITCH, then several keys: the selected voice at different pitches. **OCT+** moves them
   an octave. Press **SCL** again. Turn **SELECT**: the VOICE line changes.
5. Press **EDIT**. Turn KNOB 1 to 4: pitch, decay, filter and drive change on the next hit.
   Press **FX**. KNOB 1 and 2: level and pan. Press SEQ, GLO and HOME: the page name
   changes. Turn **PRESETS**: the kit's name shows and the sound changes. Record any control that differs from docs/POCKET-UI.md: the encoder
   positions come from the emulator, not a device.
6. Watch for 2 minutes: no stuck or endless sounds (a voice whose DECAY is at 127 sustains
   by design), OVERRUNS not rising, no interruptions.

## MIDI test

1. **USB MIDI from the computer.** Send note-on 36, velocity 100, on any channel: DRUM 1;
   the MIDI line shows NOTE 36 DRUM 1, briefly inverted.
2. Then:
   - 38: SNARE;
   - 42: CL HAT;
   - 46: OP HAT;
   - 49: CYMBAL;
   - 45: DRUM 2;
   - 48: DRUM 3.

   Velocity changes the level. Note-offs and other messages count as MIDI but trigger
   nothing.
3. **DIN or TRS MIDI**: the same notes. **BLE MIDI**: pair the computer or phone with the
   FM-1 (BLE Peripheral, as stock) and send the same notes.
4. **Simultaneous and repeated:**
   - send 36, 38 and 42 together;
   - send a fast 16th-note pattern at 180 BPM for a minute.

   DROP should stay 0, XRUN should not rise, and the sound should not stutter.

## Extended test

- **Stability:**
  - play a mixed MIDI pattern for 30 minutes, then for 2 hours;
  - record CPU peak, XRUN, CLIP and DROP at the start and the end;
  - restart and check the counters start from 0.
- **USB audio:** play audio from the computer into the FM-1 (mixed with CLATTER Pocket),
  and record the FM-1 over USB.
- **Bluetooth audio** in, if used.
- **SAFE MODE:** power on holding HOME until the screen shows "SAFE MODE"; the output is
  silent and the keys do nothing. Then connect the **official** workbench and confirm it
  sees the device (connect only; do not install unless restoring).

## Saving test

**The first write this firmware makes to the device's Flash. Do it last, and only after the backups
in "Before installation".** docs/POCKET-STORAGE.md says what is and is not known.

1. Stopped, hold **SAVE** and press **EDIT**. The page shows 12 empty slots, or **FLASH IN USE: SAVING
   OFF** or **NO USER FLASH**. Either of those two is a safe result and the end of this test: nothing has
   been written. Record it, and which firmware the unit came from.
2. Press **SAVE** on slot 1. Record what is heard and how long the screen takes to show SAVED 1.
3. Press ENV for another kit, then SAVE+EDIT and **OCT−**: the first kit is back.
4. Power off, power on, SAVE+EDIT: slot 1 is listed, and loads.
5. Install the same package again and check again: this is what shows whether an update leaves the
   sectors alone. Record the result either way.

## Recovery

1. **Normal restore.** Connect the **official M-VAVE workbench** and install official Open
   (or V15). The FM-1 under CLATTER Pocket reports the fm1_open identity and family, so the
   workbench treats it as Open. Restore projects, samples and the DX7 library from the
   backups if needed. CLATTER Pocket writes the user partition only when a kit or pattern is
   saved, and only in four sectors it has found blank or its own (docs/POCKET-STORAGE.md), so the
   stock data should still be there after restoring Open. **UNVERIFIED.**
2. **If the workbench cannot connect** while CLATTER Pocket runs normally: power off, power
   on holding HOME until SAFE MODE, then try the workbench again. **UNVERIFIED.**
3. **If the install was interrupted:** the FM-1 should come back as its update loader
   (`ota-…`), and the same page (or the official workbench) resumes. **UNVERIFIED.**
4. **If the FM-1 does not boot, or never reaches the updater** (no screen, endless
   restarts, not detected over USB): **stop.** No documented user-level recovery exists.
   fm1_open says such a device needs M-VAVE's own tools. Contact M-VAVE support with the
   recorded details. Do not open the device or attempt chip-level programming without the
   vendor's guidance. Community tools for this case are reported, and have not been tried with CLATTER
   Pocket: docs/FM1-COMMUNITY.md. Community firmwares' OCT− USB rescue is not something this firmware
   implements.

## What to report

For each step: done or stopped, what the screen showed, the figures recorded above, and
the BUILD digits. Add photos of the screen, and a USB recording of a MIDI-triggered hit
for latency and level.
