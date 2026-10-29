# First hardware test of CLATTER Pocket on the FM-1

A procedure for the first installation of the replacement firmware (FM1-REPLACEMENT.md,
PRE-FLASH-AUDIT.md) on one M-VAVE FM-1. **Nothing here has been done yet.** Steps marked
**UNVERIFIED** follow from the SDK's source and build, not from a device. The owner decides
whether to start, and every step can be the point to stop.

Candidate:
- `clatter-pocket-fm1-replace-UNVERIFIED.fwsc`, SHA-256
  `b8c13d2c4d3a40879164261e45cf9b30124b4837ccb0bf7b3c823ca0f30e7890`;
- installer `clatter-pocket-installer-UNVERIFIED.html`, SHA-256
  `53a4e2e02f43421f696cd9518721f63cd90d1b752c77ba944907d01be85f2640`.

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
1. "CLATTER POCKET" with the footer "HOLD HOME NOW FOR SAFE MODE";
2. about 1.5 s later, "LXR ENGINE READY" with the footer "KEYS/PLAY HIT  ENC 7 VOICE";
3. "MIDI WAITING";
4. a level meter;
5. CPU, XRUN, CLIP and DROP figures, and BUILD followed by 8 hex digits.

Record:
- the time to the start screen and to READY;
- the BUILD digits: the first 8 of the build identity on the installer page, in upper case;
- any restart;
- the CPU and peak figures with nothing playing (the engine's cost is the same playing or
  not);
- the XRUN count after one minute;
- temperature by touch after 5 minutes.

If the screen says **ENGINE ERROR**, the output stays silent. Stop and restore.

## First audio test

1. MASTER low. Press **PLAY**: DRUM 1 sounds once at its programmed pitch.
2. Raise MASTER: the level follows smoothly. Fully down is silent.
3. The level meter moves with each hit; CLIP stays 0 at moderate MASTER. Listen for
   distortion, clicks and dropouts.
4. Play several keys: DRUM 1 at different pitches. Turn encoder 7: the VOICE line changes.
   Play each of the seven voices.
5. Turn encoders 1 to 3: pitch, decay and level of the selected voice change. Record which
   physical knobs these are, since the labels are not verified.
6. Watch for 2 minutes: no stuck or endless sounds (a voice whose DECAY is at 127 sustains
   by design), XRUN not rising, no interruptions.

## MIDI test

1. **USB MIDI from the computer.** Send note-on 36, velocity 100, on any channel: DRUM 1;
   the MIDI line shows the count, HITS and NOTE 36.
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

## Recovery

1. **Normal restore.** Connect the **official M-VAVE workbench** and install official Open
   (or V15). The FM-1 under CLATTER Pocket reports the fm1_open identity and family, so the
   workbench treats it as Open. Restore projects, samples and the DX7 library from the
   backups if needed. The user partition is not written by CLATTER Pocket, so the data
   should still be there after restoring Open. **UNVERIFIED.**
2. **If the workbench cannot connect** while CLATTER Pocket runs normally: power off, power
   on holding HOME until SAFE MODE, then try the workbench again. **UNVERIFIED.**
3. **If the install was interrupted:** the FM-1 should come back as its update loader
   (`ota-…`), and the same page (or the official workbench) resumes. **UNVERIFIED.**
4. **If the FM-1 does not boot, or never reaches the updater** (no screen, endless
   restarts, not detected over USB): **stop.** No documented user-level recovery exists.
   fm1_open says such a device needs M-VAVE's own tools. Contact M-VAVE support with the
   recorded details. Do not open the device or attempt chip-level programming without the
   vendor's guidance.

## What to report

For each step: done or stopped, what the screen showed, the figures recorded above, and
the BUILD digits. Add photos of the screen, and a USB recording of a MIDI-triggered hit
for latency and level.
