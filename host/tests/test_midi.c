/*
 * test_midi.c - CLATTER Pocket's MIDI input (ui/pocket_midi.c): every 3-byte message, the
 * drum map, and short, long and malformed input (built with AddressSanitizer by the
 * Makefile, so a read past a message fails the test).
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pocket_lxr.h"
#include "pocket_midi.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
	/* the documented map (pocket_midi.h) */
	static const struct { int note, trigger; } map[] = {
		{ 35, POCKET_TRIG_DRUM1 }, { 36, POCKET_TRIG_DRUM1 }, { 41, POCKET_TRIG_DRUM2 }, { 43, POCKET_TRIG_DRUM2 },
		{ 45, POCKET_TRIG_DRUM2 }, { 47, POCKET_TRIG_DRUM3 }, { 48, POCKET_TRIG_DRUM3 }, { 50, POCKET_TRIG_DRUM3 },
		{ 37, POCKET_TRIG_SNARE }, { 38, POCKET_TRIG_SNARE }, { 39, POCKET_TRIG_SNARE }, { 40, POCKET_TRIG_SNARE },
		{ 49, POCKET_TRIG_CYMBAL }, { 51, POCKET_TRIG_CYMBAL }, { 52, POCKET_TRIG_CYMBAL }, { 55, POCKET_TRIG_CYMBAL },
		{ 57, POCKET_TRIG_CYMBAL }, { 59, POCKET_TRIG_CYMBAL }, { 42, POCKET_TRIG_CLOSED_HAT }, { 44, POCKET_TRIG_CLOSED_HAT },
		{ 46, POCKET_TRIG_OPEN_HAT },
	};
	int expected[128];
	for (int n = 0; n < 128; n++) expected[n] = -1;
	for (unsigned i = 0; i < sizeof map / sizeof *map; i++) expected[map[i].note] = map[i].trigger;
	int bad = 0, reached[POCKET_LXR_TRIGGERS] = { 0 };
	for (int n = 0; n < 256; n++) bad += pocket_midi_drum_trigger((unsigned)n) != (n < 128 ? expected[n] : -1);
	CHECK(!bad, "drum map differs from the documented map at %d notes", bad);

	/* every 3-byte message: accepted exactly when it is a mapped note-on with velocity > 0 */
	long accepted = 0, wrong = 0;
	uint8_t* m = malloc(3); /* exact size: AddressSanitizer catches any read beyond it */
	for (unsigned s = 0; s < 256; s++)
		for (unsigned d1 = 0; d1 < 256; d1++)
			for (unsigned d2 = 0; d2 < 256; d2++)
			{
				m[0] = (uint8_t)s; m[1] = (uint8_t)d1; m[2] = (uint8_t)d2;
				int t = -7, v = -7;
				const int r = pocket_midi_parse(m, 3, &t, &v);
				const int want = (s & 0xf0) == 0x90 && d1 < 128 && d2 > 0 && d2 < 128 && expected[d1] >= 0;
				if (r != want || (r && (t != expected[d1] || v != (int)d2)) || (!r && (t != -7 || v != -7))) wrong++;
				if (r) { accepted++; reached[t] = 1; }
			}
	free(m);
	CHECK(!wrong, "%ld of 16777216 three-byte messages parsed wrongly", wrong);
	CHECK(accepted == 16L * 21 * 127, "accepted %ld note-ons, expected %ld", accepted, 16L * 21 * 127);
	int all = 1;
	for (int t = 0; t < POCKET_LXR_TRIGGERS; t++) all &= reached[t];
	CHECK(all, "not every LXR trigger is reachable from MIDI");

	/* other lengths never trigger; reads stay inside the message */
	int other = 0;
	for (size_t n = 0; n <= 8; n++)
	{
		if (n == 3) continue;
		for (int k = 0; k < 4096; k++)
		{
			uint8_t* b = malloc(n ? n : 1);
			for (size_t i = 0; i < n; i++) b[i] = (uint8_t)rand();
			if (n) b[0] = (uint8_t)(0x90 | (k & 15));
			if (n > 1) b[1] = 36;
			if (n > 2) b[2] = 100;
			int t, v;
			other += pocket_midi_parse(b, n, &t, &v);
			free(b);
		}
	}
	CHECK(!other, "a message of the wrong length triggered %d times", other);
	int t, v;
	CHECK(!pocket_midi_parse(NULL, 3, &t, &v), "NULL message accepted");
	static const uint8_t sysex[] = { 0xf0, 0x7d, 0x01, 0xf7 }, off[] = { 0x89, 36, 64 }, zero[] = { 0x99, 36, 0 };
	CHECK(!pocket_midi_parse(sysex, sizeof sysex, &t, &v), "SysEx accepted");
	CHECK(!pocket_midi_parse(off, 3, &t, &v), "note-off accepted");
	CHECK(!pocket_midi_parse(zero, 3, &t, &v), "note-on with velocity 0 accepted");

	printf("midi: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
