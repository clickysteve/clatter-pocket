# First hardware test of CLATTER Pocket on the FM-1

A procedure for the first installation of the replacement firmware (FM1-REPLACEMENT.md,
PRE-FLASH-AUDIT.md) on one M-VAVE FM-1. Steps marked **UNVERIFIED** follow from the SDK's source
and build, not from a device. The owner decides whether to start, and every step can be the point
to stop.

**Result so far, on one unit.** 0.8.1, the first version on a device, showed the start screen and reset
in a loop: `voiceData` was misaligned, and the first audio block faulted (HARDWARE.md, Faults). The owner
restored V15 from a browser. 0.8.2, installed from stock through both stages, fixed the loop. On 0.9.0,
audio, USB audio, MIDI notes, MIDI clock over USB, and saving after erasing what V15 left, all work, and a
save survives a power cycle. 0.9.1 came back from the handover as `ota-FM1C_0000000000000000`, which
the installer did not take for the loader, and it timed out twice; the offline page, changed to take any
`ota-` loader, then installed it, and the keys, MIDI MAP and projects work. 0.9.2, installed from stock
V15 through both stages of the live installer, passed start-up (start screen about 1 s, MAIN about 2.5 s,
CPU 51.1 % mean and 51.3 % peak, FAULTS 0, cool), the audio and sequencer steps, MIDI clock over USB, the
eight GLOBAL settings after a power-off, and saving and loading after a power cycle. Naming a project
failed: the place was drawn in brackets the font does not have. The erase question confused. Both are
changed in 0.9.3, which has not run on a device. Not recorded: SAFE MODE, Bluetooth, long runs, leaving
for official firmware.
**From a unit looping at the start screen:** power on holding HOME until SAFE MODE shows, then connect
the installer. **UNVERIFIED**.

Candidate: firmware 0.9.3, commit `2715bae`, SHA-256
`d2eb62190986258aa041b9bf4e17496bf6c00cbcc3756e78f8ff2a998888c315`.

**Install it from <https://clatter-pocket.allmyfriendsaresynths.com/installer/>**, which runs the steps
below with the same update code. Or, offline, `clatter-pocket-installer-UNVERIFIED.html`, SHA-256
`afbe884cacb0534e2bd1def7b2c73df1a577620b14119b666d39cb01e0c767fa`.

Both packages are produced by:

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
   runs M-VAVE's original V15, the installer's Stage 1 installs official Open first, or use
   the official workbench, and confirm Open works before going on. Stock V15 has no project
   library, so the workbench cannot back up its projects, and M-VAVE warns that moving between
   firmwares can erase projects and samples.
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

The page runs M-VAVE's own update code (`sdk/WebUpdate.js`, unchanged).

1. Open the installer in Chrome. It must say "Both packages verified".
2. Connect the FM-1 by USB and press **Connect FM-1**. Allow MIDI with SysEx when asked. Connect
   writes nothing. Expected on official Open: "Official Open V14 detected. Ready to install over
   FM-1_8000014": go to step 4. Expected on stock V15: "Stock firmware detected": do step 3. If the
   page says it cannot identify the firmware, stop and follow step 1 of "Before installation".
3. **Stage 1, from stock V15 only.** This route is **UNVERIFIED** here: record whether it works.
   Read the warning, tick the confirmation and press **Install Open firmware**. The updater then
   sends the loader-preparation request, waits for the loader (`ota-FM-1_015`), writes official
   Open, waits for the restart and checks Open's exact build. Stage 2 stays locked until the page
   says "Official Open V14 detected". Now back up, as in step 2 of "Before installation", if
   there is anything to keep: close the workbench afterwards and press Connect FM-1 again.
4. Read the warning, tick the confirmation and press **Install CLATTER Pocket**. The
   updater then:
   1. arms the update in the running firmware;
   2. sends the loader-preparation request;
   3. waits for the FM-1 to reconnect as its update loader (`ota-FM-1_015`);
   4. writes the firmware;
   5. waits for the FM-1 to restart;
   6. checks that it reports this build.

   Do not unplug, close the page or sleep the computer until it says it has finished.
5. If either stage stops part-way, keep the page open, reconnect the same FM-1 and press
   Connect. Nothing is retried for you: Connect finds out what the FM-1 is first. In the
   loader, the page resumes what it remembers writing, and official Open if it has no record,
   because the vendor loader cannot say which package it was receiving. If that fails, go to
   Recovery.

## First boot

Expected on screen:
1. a black start screen: "CLATTER POCKET", "LXR DRUM MACHINE", "HOLD HOME NOW FOR SAFE MODE",
   the version and 8 hex digits (the build);
2. about 1.5 s later, the MAIN page: "CLATTER POCKET" over "KEYS: DRUM VEL 127";
3. "MIDI WAITING";
4. the seven activity cells and a level meter.

Press **GLO** twice for SYSTEM (GLOBAL, then SYSTEM): version, build, CPU, FAULTS. The emulator
shows this same sequence (docs/POCKET-UI.md); the stock interface here means the wrong package
is installed.

Record:
- the time to the start screen and to READY;
- the build digits: the first 8 of the build identity on the installer page, in upper case;
- any restart;
- the CPU and peak figures with nothing playing (the engine's cost is the same playing or
  not);
- the FAULTS count after one minute;
- temperature by touch after 5 minutes.

If the screen says **ENGINE ERROR**, the output stays silent. Stop and restore.

## First audio test

1. MASTER low. Press **F3**, the leftmost key: KICK, on DRUM 1, sounds once at its programmed pitch.
   Since 0.9.1 the keys run in the screen's order from there: C4, the 8th key, is PERC, on DRUM 3.
2. Raise MASTER: the level follows smoothly. Fully down is silent.
3. The level meter moves with each hit and the first activity cell flashes; it stays white, not orange, at
   moderate MASTER. Listen for
   distortion, clicks and dropouts.
4. Play the first seven keys: each is a different voice, and the VOICE line follows. Press **SEL**
   for KEYS: PITCH, then several keys: the selected voice at different pitches. **OCT+** moves them
   an octave. Press **SEL** again. Turn **SELECT**: the VOICE line changes.
5. Press **SEQ**. The keys' lamps show the selected voice's steps. Tap each white key from F3 to G5
   in order: step 1 to 16 toggles, its own lamp changes as the key goes down, and nothing sounds. Tap G#3:
   the screen says PAST THE END. Press **PLAY**: a lamp runs along the white keys. Hold a white key and turn KNOB 1:
   the step's velocity changes on screen and the step stays on. This has only run in the emulator, whose
   lamps are drawn from the same bits: note any lamp that is not on its own key. Press PLAY and HOME.
6. Press **EDIT**. Turn KNOB 1 to 4: pitch, decay, filter and drive change on the next hit.
   Press **FX**. KNOB 1 and 2: level and pan. Press SEQ, GLO and HOME: the page name
   changes. Turn **PRESETS**: the kit's name shows and the sound changes. Record any control that differs from docs/POCKET-UI.md: the encoder
   positions come from the emulator, not a device.
7. Watch for 2 minutes: no stuck or endless sounds (a voice whose DECAY is at 127 sustains
   by design), FAULTS not rising, no interruptions.

## MIDI test

1. **USB MIDI from the computer.** Send note-on 36, velocity 100, on any channel: KICK, on DRUM 1;
   the MIDI line shows NOTE 36 DRUM 1, briefly inverted.
2. Then: 37 DRUM 2, 38 DRUM 3, 39 SNARE, 40 CYMBAL, 41 CL HAT, 42 OP HAT, 43 PERC, 44 CLAP. Every note
   from 36 to 62 is a different sound. Set MIDI MAP to GM on GLOBAL for the General MIDI notes.

   Velocity changes the level. Note-offs and other messages count as MIDI but trigger
   nothing.
3. **DIN or TRS MIDI**: the same notes. **BLE MIDI**: pair the computer or phone with the
   FM-1 (BLE Peripheral, as stock) and send the same notes.
4. **Simultaneous and repeated:**
   - send 36, 39 and 41 together;
   - send a fast 16th-note pattern at 180 BPM for a minute.

   DROP should stay 0, XRUN should not rise, and the sound should not stutter.

## MIDI clock test

Send clock from a DAW over USB. The emulator has only had it by the DIN route, so record what happens
here. docs/POCKET-SEQUENCER.md says what each message does.

1. With the DAW at 120 BPM and clock on, press play in the DAW. The FM-1 starts from step 1, and MAIN
   shows `TEMPO 120 CLK`.
2. Change the DAW's tempo: the figure follows within two beats. KNOB 2 on PATTERN does nothing.
3. Record the FM-1 over USB against a click from the DAW for 5 minutes: the pattern stays on the click,
   and the offset does not grow. Record the offset.
4. Stop in the DAW: the FM-1 stops. Play again, and from a bar in the middle: it starts where the DAW does.
5. With SWING above 50, listen for even swing.
6. Turn clock off in the DAW: within a second the screen says `BPM` again.

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

1. Stopped, tap **SAVE**. PROJECTS shows 9 slots, **SAMPLES IN FLASH: SAVING OFF** or **NO USER
   FLASH**. Record which, and which firmware the unit came from. Since 0.9.3, whatever another firmware
   left in the four sectors (V15 does) is erased as PROJECTS opens, without a question: record whether
   the 9 slots appear straight away. The other two are safe results and the end of this test: nothing
   has been written.
2. Tap **SAVE** again, on slot 1. Record what is heard and how long the screen takes to show SAVED 1.
3. Press ENV for another kit, then tap SAVE and press **OCT+**: the first kit is back. Press **OCT−**
   twice on a slot you do not need: it asks, then the slot is empty.
4. Power off, power on, tap SAVE: slot 1 is listed, and loads.
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
