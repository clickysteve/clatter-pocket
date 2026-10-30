/*
 * pocket_seq.h - CLATTER Pocket's step sequencer: one 16-step pattern of seven tracks, played
 * from the audio clock (docs/POCKET-SEQUENCER.md). Platform-independent: nothing here knows
 * about the FM-1, the screen or the LXR; the voices are reached through a function pointer.
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Threading model
 * ---------------
 * The same two contexts as pocket_lxr.h, and nothing shared but single aligned words:
 *
 *   control context  every call below except pocket_seq_block.
 *   audio context    pocket_seq_block, once per engine block. No allocation, locks, division
 *                    or loops longer than the tracks.
 *
 * A step edit is one aligned 16-bit cell, so a step's velocity and note change together. A
 * whole-pattern change (generate, mutate, undo) is prepared in the pattern the audio context is
 * not reading and published by writing one byte, so a step only ever sees all of the old pattern
 * or all of the new one.
 */
#ifndef POCKET_SEQ_H
#define POCKET_SEQ_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The tracks are the LXR's seven triggers, in the same order (POCKET_TRIG_*). */
#define POCKET_SEQ_TRACKS 7
#define POCKET_SEQ_STEPS 16
#define POCKET_SEQ_BPM_MIN 30
#define POCKET_SEQ_BPM_MAX 300
#define POCKET_SEQ_SWING_MIN 50  /* straight */
#define POCKET_SEQ_SWING_MAX 75  /* first of each pair of steps 3/4 of the pair: dotted */
#define POCKET_SEQ_NOT_PLAYING 0xffu

/* Audio-context code. The FM-1 build places this file in SRAM with the engine; see
 * tools/derive-fm1-build.py (Add-PocketSections). */

/* A step: velocity in the low byte (0: off, 1..127: on), a note override in the high byte (0: the
 * instrument's programmed pitch, otherwise the MIDI note, as the keyboard plays it). */
#define POCKET_CELL(velocity, note) ((uint16_t)((velocity) | (unsigned)(note) << 8))
#define POCKET_CELL_VELOCITY(cell) ((cell) & 0xffu)
#define POCKET_CELL_NOTE(cell) ((unsigned)(cell) >> 8)
#define POCKET_SEQ_NOTE_PROGRAMMED 60 /* the note at which a voice plays at its programmed pitch */

struct pocket_pattern {
	uint16_t cell[POCKET_SEQ_TRACKS][POCKET_SEQ_STEPS];
};

/* What the audio context does with a step: trigger `track` at `velocity` and `note`. */
typedef void (*pocket_seq_emit)(int track, int velocity, int note);

/* Control context, before the audio context starts. `sample_rate` is the output's real rate,
 * `block_frames` the frames between pocket_seq_block calls. Empties the pattern and stops. */
void pocket_seq_init(uint32_t sample_rate, unsigned block_frames, pocket_seq_emit emit);

/* ---------------------------------------------------------------- control context */
/* Transport. Taken up at the next block: step 0 sounds in that block, and a stop silences the
 * next step. Voices ring out as they do after a key press. */
void pocket_seq_play(int on);
int pocket_seq_playing(void);          /* what was asked for */
/* Tempo in BPM and swing in percent, clamped. A new tempo applies from the next block, a new
 * swing from the next pair of steps, and neither moves the current position. */
void pocket_seq_set_tempo(int bpm);
int pocket_seq_tempo(void);
void pocket_seq_set_swing(int percent);
int pocket_seq_swing(void);

/* The pattern as it plays. */
const struct pocket_pattern* pocket_seq_pattern(void);
/* One step: velocity 0 clears it, 1..127 sets it, with a note override or 0. Heard the next time
 * the playhead reaches it. */
void pocket_seq_set_step(int track, int step, int velocity, int note);
/* Where a live hit belongs: the step that is sounding, or with `nearest` the next one if the
 * hit is in the later half of the step. Times use the step's real length, so swing is followed.
 * Sets *ahead when that step has not played yet: pocket_seq_skip then keeps it from sounding a
 * hit that was already heard from the key. Before the first step, step 0. */
int pocket_seq_quantise(int nearest, int* ahead);
/* The next time `step` plays, `track` stays silent, for that one pass. Forgotten as soon as any
 * step plays, so a hit that arrives too late cannot silence a later one. */
void pocket_seq_skip(int track, int step);

/* A whole-pattern change: edit the copy that _begin returns, then _commit publishes it at once
 * (the next step plays it, from the same position). The pattern it replaced is kept. */
struct pocket_pattern* pocket_seq_edit_begin(void);
void pocket_seq_edit_commit(void);
/* Back to the pattern the last commit replaced; again, forward. Later step edits go with it. */
void pocket_seq_undo(void);

/* Where the audio context is. */
int pocket_seq_position(void);         /* step last played, or POCKET_SEQ_NOT_PLAYING */
uint32_t pocket_seq_steps_played(void);/* steps since init: a change means a new step */
uint32_t pocket_seq_triggers(void);    /* voices triggered since init */
unsigned pocket_seq_last_tracks(void); /* tracks of the last step, bit per track */

/* ---------------------------------------------------------------- audio context */
void pocket_seq_block(void);

#ifdef __cplusplus
}
#endif

#endif
