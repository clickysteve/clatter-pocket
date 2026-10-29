/*
 * pocket_midi.c - see pocket_midi.h.
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "pocket_midi.h"

int pocket_midi_drum_trigger(unsigned note)
{
	switch (note)
	{
	case 35: case 36: return 0;                             /* DRUM 1 (kicks) */
	case 41: case 43: case 45: return 1;                    /* DRUM 2 (low toms) */
	case 47: case 48: case 50: return 2;                    /* DRUM 3 (high toms) */
	case 37: case 38: case 39: case 40: return 3;           /* SNARE (rim, snares, clap) */
	case 49: case 51: case 52: case 55: case 57: case 59: return 4; /* CYMBAL */
	case 42: case 44: return 5;                             /* CLOSED HAT (closed, pedal) */
	case 46: return 6;                                      /* OPEN HAT */
	default: return -1;
	}
}

int pocket_midi_parse(const uint8_t* m, size_t n, int* trigger, int* velocity)
{
	if (!m || n != 3) return 0;
	if ((m[0] & 0xf0) != 0x90) return 0;          /* note-on, any channel */
	if ((m[1] | m[2]) & 0x80) return 0;           /* data bytes are 7-bit */
	if (!m[2]) return 0;                          /* velocity 0 is a note-off */
	const int t = pocket_midi_drum_trigger(m[1]);
	if (t < 0) return 0;
	*trigger = t;
	*velocity = m[2];
	return 1;
}
