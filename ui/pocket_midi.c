/*
 * pocket_midi.c - see pocket_midi.h.
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "pocket_midi.h"
#include "pocket_seq.h"

#define TICKS_PER_BEAT 24u
#define CLOCK_LOST_MS 500u     /* a tick every 83 ms at the slowest tempo */
#define SOURCE_HELD_MS 2000u   /* two ports sending clock would double it: the first keeps it */
#define NO_SOURCE 0xffu

static volatile uint32_t last_ms;
static uint32_t beat_ms;
static uint8_t ticks, source_held = NO_SOURCE;
static uint16_t heard;
static volatile uint16_t measured;

int pocket_midi_drum_trigger(unsigned note)
{
	/* notes 35..59: kicks, snares, clap and rim, toms, hats, cymbals */
	static const int8_t drums[25] = { 0, 0, 3, 3, 3, 3, 1, 5, 1, 5, 1, 6, 2, 2, 4, 2, 4, 4, -1, -1, 4, -1, 4, -1, 4 };
	return note - 35u < sizeof drums ? drums[note - 35u] : -1;
}

int pocket_midi_note_on(const uint8_t* m, size_t n, int* channel, int* note, int* velocity)
{
	if (!m || n != 3) return 0;
	if ((m[0] & 0xf0) != 0x90) return 0;          /* note-on */
	if ((m[1] | m[2]) & 0x80) return 0;           /* data bytes are 7-bit */
	if (!m[2]) return 0;                          /* velocity 0 is a note-off */
	*channel = m[0] & 15;
	*note = m[1];
	*velocity = m[2];
	return 1;
}

/* A beat at a time, so a jittered tick moves the figure by little, and a new figure must be heard
 * twice or be a real change: 120.4 does not show as 120 and 121 in turn. */
static void tick(uint32_t now)
{
	if (ticks == TICKS_PER_BEAT)
	{
		const uint32_t ms = now - beat_ms;
		const unsigned bpm = ms ? (60000u + ms / 2u) / ms : 0;
		const unsigned shown = (unsigned)pocket_seq_tempo();
		if (bpm == heard || bpm > shown + 1u || bpm + 1u < shown) measured = (uint16_t)bpm;
		heard = (uint16_t)bpm;
		ticks = 0;
	}
	if (!ticks) beat_ms = now;
	ticks++;
	pocket_seq_clock_tick();
	pocket_seq_set_external(1);
}

int pocket_midi_clock(unsigned source, const uint8_t* m, size_t n, uint32_t now, int ready)
{
	if (!m || !n) return 0;
	const unsigned status = m[0];
	if (status == 0xf2 ? n != 3 || (m[1] | m[2]) & 0x80 : n != 1 || (status != 0xf8 && status != 0xfa && status != 0xfb && status != 0xfc)) return 0;
	if (!ready) return 1;
	if (source != source_held && source_held != NO_SOURCE && (int32_t)(now - last_ms) < (int32_t)SOURCE_HELD_MS) return 1;
	source_held = (uint8_t)source;
	last_ms = now;
	if (status == 0xf8) tick(now);
	else if (status == 0xfa)
	{
		ticks = 0;
		pocket_seq_clock_start();
	}
	else if (status == 0xfb) pocket_seq_clock_continue();
	else if (status == 0xfc) pocket_seq_play(0);
	else pocket_seq_clock_position((unsigned)m[1] | (unsigned)m[2] << 7);
	return 1;
}

int pocket_midi_clock_poll(uint32_t now)
{
	/* signed: this task can read the time just before the routing task stamps a later tick */
	if (pocket_seq_external() && (int32_t)(now - last_ms) >= (int32_t)CLOCK_LOST_MS)
	{
		pocket_seq_set_external(0);
		pocket_seq_play(0);
		ticks = 0;
	}
	const int bpm = measured;
	measured = 0;
	return bpm;
}
