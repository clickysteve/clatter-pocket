/*
 * pocket_seq.c - the step sequencer's clock and playback. See pocket_seq.h and
 * docs/POCKET-SEQUENCER.md.
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Time is exact integer arithmetic, so a tempo cannot drift however long it runs. One step at
 * `bpm` lasts rate * 15 / bpm samples. Counting phase in units of 1 / (20 * bpm) samples:
 *
 *   each sample adds             20 * bpm
 *   a straight step needs        300 * rate
 *   the first of a swung pair    6 * rate * swing
 *   the second                   6 * rate * (100 - swing)
 *
 * The pair always adds up to two straight steps (600 * rate), whatever the swing, and what is
 * left over at each step is carried into the next: a step fires on the first engine block
 * boundary at or after its exact time, never accumulating the difference.
 */
#include "pocket_seq.h"

#ifndef POCKET_BARRIER
#define POCKET_BARRIER() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#endif

#define CLOSED_HAT 5   /* POCKET_TRIG_CLOSED_HAT */
#define OPEN_HAT 6     /* POCKET_TRIG_OPEN_HAT */

static struct pocket_pattern patterns[2];
static volatile uint8_t live;              /* which pattern plays; written by control */

/* control -> audio */
static volatile uint8_t runRequest;
static volatile uint16_t tempoBpm;
static volatile uint8_t swingPercent;
/* audio -> control */
static volatile uint8_t position;
static volatile uint32_t stepsPlayed, triggers, lastTracks;
/* audio only */
static uint8_t running, step, pairSwing;
static uint32_t phase, interval, unitPerBlock;
/* set at init */
static uint32_t unitPerSwing;              /* 6 * rate */
static pocket_seq_emit emit;

void pocket_seq_init(uint32_t sample_rate, unsigned block_frames, pocket_seq_emit fn)
{
	for (unsigned p = 0; p < 2; p++)
		for (unsigned t = 0; t < POCKET_SEQ_TRACKS; t++)
			for (unsigned s = 0; s < POCKET_SEQ_STEPS; s++) patterns[p].vel[t][s] = 0;
	live = 0;
	runRequest = 0;
	tempoBpm = 120;
	swingPercent = POCKET_SEQ_SWING_MIN;
	position = POCKET_SEQ_NOT_PLAYING;
	stepsPlayed = triggers = lastTracks = 0;
	running = 0;
	unitPerSwing = 6u * sample_rate;
	unitPerBlock = 20u * block_frames;
	emit = fn;
}

/* ------------------------------------------------------------------ control context */
void pocket_seq_play(int on) { runRequest = on ? 1 : 0; }
int pocket_seq_playing(void) { return runRequest; }

void pocket_seq_set_tempo(int bpm)
{
	tempoBpm = (uint16_t)(bpm < POCKET_SEQ_BPM_MIN ? POCKET_SEQ_BPM_MIN : bpm > POCKET_SEQ_BPM_MAX ? POCKET_SEQ_BPM_MAX : bpm);
}
int pocket_seq_tempo(void) { return tempoBpm; }

void pocket_seq_set_swing(int percent)
{
	swingPercent = (uint8_t)(percent < POCKET_SEQ_SWING_MIN ? POCKET_SEQ_SWING_MIN : percent > POCKET_SEQ_SWING_MAX ? POCKET_SEQ_SWING_MAX : percent);
}
int pocket_seq_swing(void) { return swingPercent; }

const struct pocket_pattern* pocket_seq_pattern(void) { return &patterns[live]; }

void pocket_seq_set_step(int track, int step_, int velocity)
{
	if (track < 0 || track >= POCKET_SEQ_TRACKS || step_ < 0 || step_ >= POCKET_SEQ_STEPS) return;
	patterns[live].vel[track][step_] = (uint8_t)(velocity < 0 ? 0 : velocity > 127 ? 127 : velocity);
}

struct pocket_pattern* pocket_seq_edit_begin(void)
{
	struct pocket_pattern* next = &patterns[!live];
	*next = patterns[live];
	return next;
}

void pocket_seq_edit_commit(void)
{
	POCKET_BARRIER(); /* the new pattern is complete before it can be seen */
	live = (uint8_t)!live;
}

void pocket_seq_undo(void) { pocket_seq_edit_commit(); }

int pocket_seq_position(void) { return position; }
uint32_t pocket_seq_steps_played(void) { return stepsPlayed; }
uint32_t pocket_seq_triggers(void) { return triggers; }
unsigned pocket_seq_last_tracks(void) { return lastTracks; }

/* ------------------------------------------------------------------ audio context */
/* The hats are one voice: an open hat on the step takes the voice, as CLATTER's choke does. */
static void fire(void)
{
	const struct pocket_pattern* p = &patterns[live];
	uint32_t tracks = 0, count = 0;
	for (unsigned t = 0; t < POCKET_SEQ_TRACKS; t++)
	{
		const unsigned v = p->vel[t][step];
		if (!v || (t == CLOSED_HAT && p->vel[OPEN_HAT][step])) continue;
		emit((int)t, (int)v);
		tracks |= 1u << t;
		count++;
	}
	lastTracks = tracks;
	triggers += count;
	stepsPlayed++;
	position = step;
}

void pocket_seq_block(void)
{
	if (!running)
	{
		if (!runRequest) return;
		running = 1;
		step = 0;
		phase = 0;
		pairSwing = swingPercent;
		interval = unitPerSwing * pairSwing;
		fire();
		return;
	}
	if (!runRequest)
	{
		running = 0;
		position = POCKET_SEQ_NOT_PLAYING;
		return;
	}
	phase += unitPerBlock * tempoBpm;
	if (phase < interval) return;
	phase -= interval;
	step = (step + 1u) & (POCKET_SEQ_STEPS - 1u);
	if (step & 1u)
		interval = unitPerSwing * (100u - pairSwing);
	else
	{
		pairSwing = swingPercent;
		interval = unitPerSwing * pairSwing;
	}
	fire();
}
