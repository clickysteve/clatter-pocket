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
#include "pocket_seq.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void emit_nothing(int track, int velocity, int note, int variation)
{
	(void)track; (void)velocity; (void)note; (void)variation;
}

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

	/* every 3-byte message: accepted exactly when it is a note-on with velocity > 0 */
	long accepted = 0, wrong = 0;
	uint8_t* m = malloc(3); /* exact size: AddressSanitizer catches any read beyond it */
	for (unsigned s = 0; s < 256; s++)
		for (unsigned d1 = 0; d1 < 256; d1++)
			for (unsigned d2 = 0; d2 < 256; d2++)
			{
				m[0] = (uint8_t)s; m[1] = (uint8_t)d1; m[2] = (uint8_t)d2;
				int c = -7, t = -7, v = -7;
				const int r = pocket_midi_note_on(m, 3, &c, &t, &v);
				const int want = (s & 0xf0) == 0x90 && d1 < 128 && d2 > 0 && d2 < 128;
				if (r != want || (r && (c != (int)(s & 15) || t != (int)d1 || v != (int)d2)) || (!r && (c != -7 || t != -7 || v != -7))) wrong++;
				if (r) accepted++;
				if (r && expected[t] >= 0) reached[expected[t]] = 1;
			}
	free(m);
	CHECK(!wrong, "%ld of 16777216 three-byte messages parsed wrongly", wrong);
	CHECK(accepted == 16L * 128 * 127, "accepted %ld note-ons, expected %ld", accepted, 16L * 128 * 127);
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
			int c, t, v;
			other += pocket_midi_note_on(b, n, &c, &t, &v);
			free(b);
		}
	}
	CHECK(!other, "a message of the wrong length triggered %d times", other);
	int c, t, v;
	CHECK(!pocket_midi_note_on(NULL, 3, &c, &t, &v), "NULL message accepted");
	static const uint8_t sysex[] = { 0xf0, 0x7d, 0x01, 0xf7 }, off[] = { 0x89, 36, 64 }, zero[] = { 0x99, 36, 0 };
	CHECK(!pocket_midi_note_on(sysex, sizeof sysex, &c, &t, &v), "SysEx accepted");
	CHECK(!pocket_midi_note_on(off, 3, &c, &t, &v), "note-off accepted");
	CHECK(!pocket_midi_note_on(zero, 3, &c, &t, &v), "note-on with velocity 0 accepted");

	/* the clock's messages, and nothing else, are the clock's */
	static const uint8_t clock[] = { 0xf8 }, start[] = { 0xfa }, resume[] = { 0xfb }, stop[] = { 0xfc }, sensing[] = { 0xfe };
	static const uint8_t song[] = { 0xf2, 3, 0 }, bad_song[] = { 0xf2, 0x83, 0 }, note[] = { 0x90, 36, 100 };
	pocket_seq_init(44118, 64, emit_nothing);
	CHECK(pocket_midi_clock(0, clock, 1, 0, 0) && !pocket_seq_external(), "before the engine runs clock is taken and not used");
	CHECK(!pocket_midi_clock(0, sensing, 1, 0, 1) && !pocket_midi_clock(0, note, 3, 0, 1) && !pocket_midi_clock(0, bad_song, 3, 0, 1)
	      && !pocket_midi_clock(0, NULL, 1, 0, 1) && !pocket_midi_clock(0, song, 2, 0, 1) && !pocket_midi_clock(0, clock, 2, 0, 1), "other messages are not the clock's");
	int notes = 0;
	for (unsigned s = 0; s < 256; s++)
	{
		const uint8_t m1[1] = { (uint8_t)s };
		const int c1 = pocket_midi_clock(9, m1, 1, 0, 0);
		if (c1 != (s == 0xf8 || s == 0xfa || s == 0xfb || s == 0xfc)) notes++;
	}
	CHECK(!notes, "%d one-byte messages wrongly taken", notes);

	/* the tempo: from a beat of ticks, on a clock of whole milliseconds with a jittered tick */
	static const int tempos[] = { 30, 60, 90, 120, 125, 137, 174, 200, 300 };
	for (unsigned t = 0; t < sizeof tempos / sizeof *tempos; t++)
	{
		pocket_seq_init(44118, 64, emit_nothing);
		uint64_t at = 1000000u * (t + 1u);
		pocket_midi_clock(0, start, 1, (uint32_t)(at / 1000u), 1);
		int got = 0, last = 0, changes = 0;
		for (unsigned i = 0; i < 24u * 8u; i++)
		{
			at += 2500000u / (unsigned)tempos[t];
			pocket_midi_clock(0, clock, 1, (uint32_t)((at + (i & 1u) * 900u) / 1000u), 1);
			const int bpm = pocket_midi_clock_poll((uint32_t)(at / 1000u));
			if (bpm)
			{
				got = bpm;
				pocket_seq_set_tempo(bpm);
				changes += last && bpm != last;
				last = bpm;
			}
		}
		const int off = got - tempos[t];
		CHECK(off >= -1 && off <= 1, "%d BPM read as %d", tempos[t], got);
		CHECK(changes <= 1, "%d BPM: the figure moved %d times", tempos[t], changes);
		pocket_midi_clock(0, stop, 1, (uint32_t)(at / 1000u), 1);
		CHECK(!pocket_seq_playing(), "stop stops");
	}

	/* two ports: the first keeps the clock while it is sending */
	pocket_seq_init(44118, 64, emit_nothing);
	pocket_midi_clock_poll(100000); /* forget the last port's clock */
	uint32_t ms = 200000;
	pocket_midi_clock(1, start, 1, ms, 1);
	pocket_midi_clock(2, stop, 1, ms + 10, 1);
	CHECK(pocket_seq_playing(), "another port cannot stop it");
	pocket_midi_clock(1, resume, 1, ms + 20, 1);
	pocket_midi_clock(2, start, 1, ms + 5000, 1);
	CHECK(pocket_seq_playing() && pocket_seq_external(), "a port silent for seconds gives way");

	/* half a second of nothing: gone */
	pocket_midi_clock_poll(ms + 5400);
	CHECK(pocket_seq_external(), "still following after 400 ms");
	pocket_midi_clock_poll(ms + 5500);
	CHECK(!pocket_seq_external() && !pocket_seq_playing(), "stopped and on its own tempo after 500 ms");

	printf("midi: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
