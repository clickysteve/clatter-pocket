/*
 * pocket_midi.c - see pocket_midi.h.
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "pocket_midi.h"

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
