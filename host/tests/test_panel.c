/*
 * test_panel.c - host tests of the Pocket panel (ui/pocket_panel.c) on the real engine, and a
 * screenshot of the panel as the FM-1 would show it (build/host/panel.ppm).
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pocket_lxr.h"
#include "pocket_params.h"
#include "pocket_panel.h"
#include "pocket_seq.h"
#include "pocket_gen.h"
#include "pocket_kit.h"
#include "pocket_store.h"
#include "pocket_history.h"
#include "fake_flash.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int32_t out[2 * 128];

static void poll(struct pocket_panel* p, uint32_t keys, uint32_t buttons, int select, int k1, int k2, int k3, int k4)
{
	struct pocket_panel_input in = { keys, buttons, { 0 } };
	in.encoders[POCKET_ENC_SELECT] = (int16_t)select;
	in.encoders[POCKET_ENC_KNOB1] = (int16_t)k1;
	in.encoders[POCKET_ENC_KNOB2] = (int16_t)k2;
	in.encoders[POCKET_ENC_KNOB3] = (int16_t)k3;
	in.encoders[POCKET_ENC_KNOB4] = (int16_t)k4;
	pocket_panel_apply(p, &in);
}
#define BUTTON(b) (1u << POCKET_BTN_##b)

/* A press and a release of one button. */
static void press(struct pocket_panel* p, uint32_t buttons)
{
	poll(p, 0, buttons, 0, 0, 0, 0, 0);
	poll(p, 0, 0, 0, 0, 0, 0, 0);
}

#define KEY(note) (1u << ((note) - POCKET_KEY_LOWEST))
#define KEY_PROGRAMMED KEY(60) /* in PITCH */
/* The first bank's key of each trigger. F3 is the kick, and the banks follow every nine keys. */
#define BANK_KEY(position, bank) (POCKET_KEY_LOWEST + (position) + 9 * (bank))
static const int firstKey[POCKET_LXR_TRIGGERS] = { BANK_KEY(0, 0), BANK_KEY(1, 0), BANK_KEY(2, 0), BANK_KEY(3, 0), BANK_KEY(4, 0), BANK_KEY(5, 0), BANK_KEY(6, 0) };
#define KEY_KICK KEY(BANK_KEY(0, 0))
#define KEY_SNARE KEY(BANK_KEY(3, 0))
#define KEY_HAT KEY(BANK_KEY(5, 0))
#define KEY_LOW_TOM KEY(BANK_KEY(1, 0))

static void tap_keys(struct pocket_panel* p, uint32_t keys)
{
	poll(p, keys, 0, 0, 0, 0, 0, 0);
	poll(p, 0, 0, 0, 0, 0, 0, 0);
}
/* SAVE or another button held, a second pressed, both released. */
static void press2(struct pocket_panel* p, uint32_t held, uint32_t button)
{
	poll(p, 0, held, 0, 0, 0, 0, 0);
	poll(p, 0, held | button, 0, 0, 0, 0, 0);
	poll(p, 0, held, 0, 0, 0, 0, 0);
	poll(p, 0, 0, 0, 0, 0, 0, 0);
}

static void hit_key(struct pocket_panel* p)
{
	poll(p, KEY_KICK, 0, 0, 0, 0, 0, 0);
	poll(p, 0, 0, 0, 0, 0, 0, 0);
}

static void start_engine(struct pocket_panel* p)
{
	pocket_lxr_init(1);
	pocket_seq_init(44100, POCKET_LXR_BLOCK, pocket_lxr_trigger_now);
	pocket_panel_init(p);
	pocket_panel_start_sequencer(p);
	pocket_lxr_set_block_hook(pocket_seq_block);
}

static int32_t render_peak(int blocks)
{
	int32_t peak = 0;
	for (int b = 0; b < blocks; b++)
	{
		pocket_lxr_render_q23(out, 128, 0);
		for (int i = 0; i < 256; i++) { const int32_t a = out[i] < 0 ? -out[i] : out[i]; if (a > peak) peak = a; }
	}
	return peak;
}

static void write_screen(const char* path, const struct pocket_panel* panel, const struct pocket_panel_status* status)
{
	static uint16_t band[POCKET_SCREEN_W * POCKET_BAND_ROWS];
	FILE* f = fopen(path, "wb");
	if (!f) return;
	fprintf(f, "P6\n%d %d\n255\n", POCKET_SCREEN_W, POCKET_SCREEN_H);
	for (unsigned b = 0; b < POCKET_BANDS; b++)
	{
		pocket_panel_draw_band(panel, status, b, band);
		for (int i = 0; i < POCKET_SCREEN_W * POCKET_BAND_ROWS; i++)
		{
			const uint16_t c = band[i];
			const unsigned char rgb[3] = { (unsigned char)((c >> 11) << 3), (unsigned char)(((c >> 5) & 63) << 2), (unsigned char)((c & 31) << 3) };
			fwrite(rgb, 3, 1, f);
		}
	}
	fclose(f);
}

static void turn(struct pocket_panel* p, int encoder, int by)
{
	struct pocket_panel_input in = { 0, 0, { 0 } };
	in.encoders[encoder] = (int16_t)by;
	pocket_panel_apply(p, &in);
}

/* The output's first sample at or above `level`, from `from`, over `frames`; -1 if none. */
static long first_at(const int32_t* samples, long from, long frames, int32_t level)
{
	for (long f = from; f < frames; f++)
		if (samples[2 * f] >= level || samples[2 * f] <= -level || samples[2 * f + 1] >= level || samples[2 * f + 1] <= -level) return f;
	return -1;
}

#define RENDER_FRAMES (44100 * 4)
static int32_t rendered[2 * RENDER_FRAMES];

static void render_seconds(long frames)
{
	for (long f = 0; f + 128 <= frames; f += 128) pocket_lxr_render_q23(rendered + 2 * f, 128, 0);
}

static void clear_pattern(void)
{
	struct pocket_pattern* p = pocket_seq_edit_begin();
	memset(p, 0, sizeof *p);
	pocket_seq_edit_commit();
}

/* What a step can hold, different on each track and page: off, a note, a variation, every condition. */
static uint32_t sample_cell(unsigned t, unsigned s, unsigned page)
{
	if ((t + s + page) % 5u == 4u) return 0;
	const unsigned tone = s % 3u == 0 ? 0 : s % 3u == 1 ? 36u + s + page : POCKET_TONE_VARIATION(1u + (t + page) % 2u);
	return POCKET_CELL_WITH_CONDITION(POCKET_CELL(1u + (t * 17u + s * 5u + page * 31u) % 127u, tone), (t + s + page) % POCKET_CONDITIONS);
}

/* The sequencer through the panel, on the real engine: what is heard, not what is stored. */
static void test_sequencer(void)
{
	struct pocket_panel panel;
	start_engine(&panel);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0); /* nothing held */
	press(&panel, BUTTON(SEQ));
	CHECK(panel.page == POCKET_PAGE_PATTERN, "SEQ opens PATTERN");

	/* a first pattern is there before anything is pressed, and it is silent until PLAY */
	int hits = 0;
	for (int t = 0; t < POCKET_SEQ_TRACKS; t++)
		for (int s = 0; s < POCKET_SEQ_STEPS; s++) hits += pocket_seq_pattern()->cell[t][s] != 0;
	CHECK(hits > 8, "a first pattern: %d hits", hits);
	CHECK(render_peak(50) == 0, "silent until PLAY");

	/* one kick on steps 1 and 9: the sound starts exactly where the clock says */
	clear_pattern();
	press(&panel, BUTTON(REC));
	tap_keys(&panel, KEY_KICK);
	CHECK(pocket_seq_pattern()->cell[0][0] == POCKET_CELL(127, 0), "REC and a key set the selected step: %x", pocket_seq_pattern()->cell[0][0]);
	turn(&panel, POCKET_ENC_KNOB4, 8);
	CHECK(panel.step == 8, "KNOB4 moves the step");
	tap_keys(&panel, KEY_KICK);
	press(&panel, BUTTON(REC));
	render_peak(600); /* the keys' own hits ring out */
	press(&panel, BUTTON(PLAY));
	CHECK(pocket_seq_playing(), "PLAY starts the sequencer");
	render_seconds(RENDER_FRAMES);
	const int32_t level = POCKET_Q23_MAX / 50;
	const long first = first_at(rendered, 0, RENDER_FRAMES, level);
	const long gap = 44100 * 15 / 120 * 8, second = first_at(rendered, first + gap - 200, RENDER_FRAMES, level);
	CHECK(first >= 0 && first < 128, "step 1 sounds in the first block: %ld", first);
	CHECK(second >= first + gap - 64 && second <= first + gap + 64, "step 9 half a bar later: %ld", second - first);
	pocket_panel_tick(&panel, 1);
	CHECK(panel.shown_position == pocket_seq_position(), "the screen follows the playhead");
	CHECK(panel.flash_ms[0] || pocket_seq_triggers() >= 4, "steps light the cells");

	/* STOP: the hit in hand rings out, nothing new starts */
	press(&panel, BUTTON(PLAY));
	CHECK(!pocket_seq_playing(), "PLAY stops the sequencer");
	const uint32_t triggers = pocket_seq_triggers();
	render_seconds(RENDER_FRAMES);
	CHECK(pocket_seq_triggers() == triggers, "no trigger after STOP");
	CHECK(pocket_seq_position() == POCKET_SEQ_NOT_PLAYING, "the playhead leaves");
	/* a stopped sequencer rings out to silence: no voice stuck on */
	int32_t tail = 0;
	for (long f = RENDER_FRAMES / 2; f < RENDER_FRAMES; f++)
	{
		const int32_t a = rendered[2 * f] < 0 ? -rendered[2 * f] : rendered[2 * f];
		if (a > tail) tail = a;
	}
	CHECK(tail < POCKET_Q23_MAX / 1000, "voices end after STOP: %d", tail);

	/* velocity reaches the sound */
	clear_pattern();
	panel.step = 0;
	pocket_seq_set_step(0, 0, 99, 0);
	turn(&panel, POCKET_ENC_KNOB1, 7);
	CHECK(pocket_seq_pattern()->cell[0][0] == 127, "KNOB1 sets the velocity: %d", pocket_seq_pattern()->cell[0][0]);
	for (int t = 0; t < POCKET_SEQ_TRACKS; t++)
	{
		int32_t peak[2];
		for (int soft = 0; soft < 2; soft++)
		{
			clear_pattern();
			pocket_seq_set_step(t, 0, soft ? 30 : 127, 0);
			pocket_seq_play(1);
			render_seconds(44100 / 4);
			pocket_seq_play(0);
			peak[soft] = 0;
			for (int i = 0; i < 44100 / 2; i++) { const int32_t a = rendered[i] < 0 ? -rendered[i] : rendered[i]; if (a > peak[soft]) peak[soft] = a; }
			render_seconds(RENDER_FRAMES); /* ring out */
		}
		CHECK(peak[1] < peak[0] * 2 / 3, "track %d: a soft step is quieter: %d, %d", t, peak[1], peak[0]);
		CHECK(peak[0] > POCKET_Q23_MAX / 100, "track %d sounds at all: %d", t, peak[0]);
	}
	clear_pattern();
	pocket_seq_set_step(0, 0, 100, 0);

	/* the knobs, the layer, the limits */
	turn(&panel, POCKET_ENC_KNOB2, 20);
	CHECK(pocket_seq_tempo() == 140, "KNOB2 sets the tempo: %d", pocket_seq_tempo());
	turn(&panel, POCKET_ENC_KNOB2, -9999);
	CHECK(pocket_seq_tempo() == POCKET_SEQ_BPM_MIN, "tempo floor");
	turn(&panel, POCKET_ENC_KNOB2, 90);
	turn(&panel, POCKET_ENC_KNOB3, 16);
	CHECK(pocket_seq_swing() == 66, "KNOB3 sets the swing: %d", pocket_seq_swing());
	/* GLOBAL holds what the knobs of PATTERN used to: the generator's settings */
	press(&panel, BUTTON(GLO));
	CHECK(panel.page == POCKET_PAGE_GLOBAL, "GLO opens GLOBAL");
	turn(&panel, POCKET_ENC_KNOB1, 13);
	turn(&panel, POCKET_ENC_KNOB2, -5);
	CHECK(panel.mutate_strength == POCKET_MUTATE_SMALL, "GLOBAL sets the mutation");
	turn(&panel, POCKET_ENC_KNOB1, -3);
	turn(&panel, POCKET_ENC_KNOB2, 25);
	CHECK(panel.density == 75, "GLOBAL sets the density: %d", panel.density);
	turn(&panel, POCKET_ENC_KNOB1, 1);
	turn(&panel, POCKET_ENC_KNOB2, 45);
	CHECK(panel.complexity == 75, "GLOBAL sets the complexity: %d", panel.complexity);
	turn(&panel, POCKET_ENC_KNOB1, 1);
	turn(&panel, POCKET_ENC_KNOB2, 41);
	CHECK(panel.seed == 42, "GLOBAL sets the seed: %d", panel.seed);
	CHECK(pocket_seq_tempo() == 120 && pocket_seq_swing() == 66, "not the tempo or swing");
	turn(&panel, POCKET_ENC_KNOB2, -43);
	CHECK(panel.seed == 9999, "the seed wraps: %d", panel.seed);
	turn(&panel, POCKET_ENC_KNOB2, 43);
	press(&panel, BUTTON(SEQ));

	/* ALGORITHM is the style, and the next GENERATE is the same seed in it */
	turn(&panel, POCKET_ENC_ALGORITHM, 1);
	CHECK(panel.style == 1, "ALGORITHM chooses the style");
	turn(&panel, POCKET_ENC_ALGORITHM, -1);
	CHECK(panel.style == 0 && panel.step == 0, "not the step");
	turn(&panel, POCKET_ENC_ALGORITHM, -POCKET_GEN_STYLES);
	CHECK(panel.style == 0, "the style wraps");

	/* GENERATE: a setting moved, that seed; pressed again, the next */
	press(&panel, BUTTON(ARP));
	CHECK(panel.seed == 42, "a changed setting keeps the seed: %d", panel.seed);
	struct pocket_pattern first_pattern = *pocket_seq_pattern(), expected;
	const struct pocket_gen_settings settings = { 42, 75, 75, 0, 0 };
	memset(&expected, 0, sizeof expected);
	pocket_gen_generate(&expected, &settings);
	CHECK(!memcmp(&first_pattern, &expected, sizeof expected), "GENERATE is the seed's pattern");
	press(&panel, BUTTON(ARP));
	CHECK(panel.seed == 43, "pressed again, the next seed");
	CHECK(memcmp(&first_pattern, pocket_seq_pattern(), sizeof expected), "another pattern");
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));
	CHECK(!memcmp(&first_pattern, pocket_seq_pattern(), sizeof expected), "SAVE+OCT- undoes it");
	CHECK(panel.locks == 0, "a chord does not lock");
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_UP));

	/* MUTATE changes the pattern and each press differs; a locked track stays */
	panel.trigger = 0;
	press2(&panel, BUTTON(SAVE), BUTTON(FX));
	CHECK(panel.locks == 1, "SAVE locks the track");
	const struct pocket_pattern base = *pocket_seq_pattern();
	int changed = 0;
	for (int i = 0; i < 20; i++)
	{
		const struct pocket_pattern was = *pocket_seq_pattern();
		press(&panel, BUTTON(LFO));
		changed += memcmp(&was, pocket_seq_pattern(), sizeof was) != 0;
	}
	CHECK(changed >= 19, "MUTATE changes the pattern: %d of 20", changed);
	CHECK(!memcmp(base.cell[0], pocket_seq_pattern()->cell[0], sizeof base.cell[0]), "MUTATE leaves a locked track");
	press(&panel, BUTTON(ARP));
	CHECK(!memcmp(base.cell[0], pocket_seq_pattern()->cell[0], sizeof base.cell[0]), "GENERATE leaves a locked track");

	/* MIDI and keys sound over the sequence */
	press(&panel, BUTTON(HOME));
	press(&panel, BUTTON(PLAY));
	const uint32_t hits_before = panel.hits;
	hit_key(&panel);
	render_seconds(44100);
	press(&panel, BUTTON(PLAY));
	CHECK(panel.hits == hits_before + 1, "keys still play over the sequence: %u", panel.hits - hits_before);
	struct pocket_lxr_stats stats;
	pocket_lxr_stats_read(&stats);
	CHECK(stats.events_dropped == 0, "nothing dropped");

	/* the longest fields, locked, on every page: no crash under test */
	static uint16_t band[POCKET_SCREEN_W * POCKET_BAND_ROWS];
	for (unsigned page = 0; page < POCKET_PAGES; page++)
		for (unsigned row = 0; row < 17; row++)
		{
			panel.page = (uint8_t)page;
			panel.global_row = (uint8_t)row;
			panel.locks = 0x7f;
			pocket_seq_set_tempo(POCKET_SEQ_BPM_MAX);
			panel.seed = 9999;
			panel.density = 100;
			panel.complexity = 100;
			panel.style = 7; /* INDUSTRIAL, the longest name */
			for (unsigned b = 0; b < POCKET_BANDS; b++) pocket_panel_draw_band(&panel, NULL, b, band);
		}
	checks++;
}

/* How often the output rises through zero: compares the pitch of two hits of one voice. */
static long crossings(long from, long frames)
{
	long n = 0;
	for (long f = from + 1; f < frames; f++) n += rendered[2 * (f - 1)] < 0 && rendered[2 * f] >= 0;
	return n;
}

/* The keyboard: which voice, what pitch, what stays alone. */
static void test_keyboard(void)
{
	struct pocket_panel panel;
	start_engine(&panel);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);

	/* DRUM: the first bank plays each instrument, at its programmed pitch */
	for (int t = 0; t < POCKET_LXR_TRIGGERS; t++)
	{
		start_engine(&panel);
		poll(&panel, 0, 0, 0, 0, 0, 0, 0);
		tap_keys(&panel, KEY(firstKey[t]));
		CHECK(panel.flash_ms[t] != 0, "key %d lit track %d", firstKey[t], t);
		CHECK(panel.trigger == t, "the selection follows the last key: %d", panel.trigger);
	}
	/* every key of the keyboard does something, the nine positions in the same order in each bank */
	start_engine(&panel);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	panel.follow = 0;
	static const int order[POCKET_BANK_KEYS] = { POCKET_TRIG_DRUM1, POCKET_TRIG_DRUM2, POCKET_TRIG_DRUM3, POCKET_TRIG_SNARE,
		POCKET_TRIG_CYMBAL, POCKET_TRIG_CLOSED_HAT, POCKET_TRIG_OPEN_HAT, POCKET_TRIG_DRUM3, POCKET_TRIG_SNARE };
	for (int k = 0; k < POCKET_KEYS; k++)
	{
		const uint32_t hits = panel.hits;
		memset(panel.flash_ms, 0, sizeof panel.flash_ms);
		tap_keys(&panel, 1u << k);
		render_peak(1);
		CHECK(panel.hits == hits + 1, "key %d is dead", k);
		CHECK(panel.flash_ms[order[k % POCKET_BANK_KEYS]] != 0, "key %d is position %d's track", k, k % POCKET_BANK_KEYS);
	}
	CHECK(panel.trigger == POCKET_TRIG_DRUM1, "with FOLLOW off the selection stays");

	/* SCL: the mode changes and nothing else */
	press(&panel, BUTTON(SEQ));
	pocket_seq_play(1);
	render_peak(4);
	const struct pocket_pattern before = *pocket_seq_pattern();
	const int position = pocket_seq_position();
	press(&panel, BUTTON(SCL));
	CHECK(panel.key_mode == POCKET_KEYS_PITCH, "SCL: PITCH");
	CHECK(!strcmp(panel.notice, "KEYS: PITCH") && panel.notice_ms, "SCL says so: %s", panel.notice);
	CHECK(pocket_seq_playing() && pocket_seq_position() == position && !memcmp(&before, pocket_seq_pattern(), sizeof before), "SCL leaves the playback and the pattern");
	CHECK(panel.page == POCKET_PAGE_PATTERN && panel.trigger == POCKET_TRIG_DRUM1, "and the page and the voice");
	pocket_seq_play(0);
	render_peak(4);

	/* PITCH: the selected voice, chromatically, an octave a click */
	start_engine(&panel);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	press(&panel, BUTTON(SCL));
	poll(&panel, 0, 0, POCKET_TRIG_DRUM2, 0, 0, 0, 0);
	const int coarse = pocket_lxr_get_param(POCKET_PAR_COARSE2);
	long at[3];
	const int notes[3] = { 60, 72, 67 };
	for (int i = 0; i < 3; i++)
	{
		render_seconds(44100);
		tap_keys(&panel, KEY(notes[i]));
		render_seconds(44100 / 2);
		at[i] = crossings(44100 / 8, 44100 / 2);
	}
	CHECK(panel.trigger == POCKET_TRIG_DRUM2, "PITCH keeps the selected voice");
	CHECK(at[1] > at[0] * 17 / 10, "an octave up is about twice the frequency: %ld, %ld", at[0], at[1]);
	CHECK(at[2] > at[0] * 12 / 10 && at[2] < at[1], "a fifth is between: %ld %ld %ld", at[0], at[2], at[1]);
	CHECK(pocket_lxr_get_param(POCKET_PAR_COARSE2) == coarse, "playing a pitch writes nothing to the patch");

	/* the note at which a voice is at home: no transposition can build up */
	render_seconds(44100);
	tap_keys(&panel, KEY(72));
	render_seconds(44100);
	tap_keys(&panel, KEY(72));
	render_seconds(44100);
	tap_keys(&panel, KEY(60));
	render_seconds(44100 / 2);
	const long home = crossings(44100 / 8, 44100 / 2);
	CHECK(home == at[0], "back on C4 the pitch is exactly the first: %ld, %ld", home, at[0]);

	/* OCT in PITCH: clamped, and it moves the notes */
	for (int i = 0; i < 9; i++) press(&panel, BUTTON(OCT_UP));
	CHECK(panel.octave == POCKET_OCTAVE_MAX, "the octave clamps: %d", panel.octave);
	for (int i = 0; i < 9; i++) press(&panel, BUTTON(OCT_DOWN));
	CHECK(panel.octave == POCKET_OCTAVE_MIN, "the octave clamps: %d", panel.octave);
	for (int i = 0; i < 3; i++) press(&panel, BUTTON(OCT_UP));
	press(&panel, BUTTON(OCT_UP));
	render_seconds(44100);
	tap_keys(&panel, KEY(60));
	render_seconds(44100 / 2);
	CHECK(crossings(44100 / 8, 44100 / 2) == at[1], "OCT+ is the same pitch as the key an octave up");

	/* OCT in DRUM: how hard the keys hit, heard */
	press(&panel, BUTTON(SCL));
	const int octave = panel.octave;
	press(&panel, BUTTON(OCT_DOWN));
	CHECK(panel.key_velocity == 100, "OCT- in DRUM: velocity 100: %d", panel.key_velocity);
	CHECK(panel.octave == octave, "and not the octave");
	press(&panel, BUTTON(OCT_DOWN));
	press(&panel, BUTTON(OCT_DOWN));
	press(&panel, BUTTON(OCT_DOWN));
	CHECK(panel.key_velocity == 40, "the velocity stops at 40: %d", panel.key_velocity);
	render_peak(800);
	tap_keys(&panel, KEY_SNARE);
	const int32_t soft = render_peak(30);
	for (int i = 0; i < 3; i++) press(&panel, BUTTON(OCT_UP));
	CHECK(panel.key_velocity == 127, "and at 127: %d", panel.key_velocity);
	render_peak(800);
	tap_keys(&panel, KEY_SNARE);
	const int32_t loud = render_peak(30);
	CHECK(soft < loud * 3 / 4, "velocity is heard from the keys: %d, %d", soft, loud);
}

/* REC, stopped and playing. */
static void test_recording(void)
{
	struct pocket_panel panel;
	start_engine(&panel);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	clear_pattern();

	/* off: the keys record nothing */
	tap_keys(&panel, KEY_KICK);
	CHECK(!pocket_seq_pattern()->cell[0][0], "REC off records nothing");
	press(&panel, BUTTON(SEQ));
	/* a toggle, shown, with a light */
	press(&panel, BUTTON(REC));
	CHECK(panel.recording && !strcmp(panel.notice, "REC ON"), "REC on: %s", panel.notice);
	CHECK(pocket_panel_leds(&panel) & BUTTON(REC), "REC lights");
	CHECK(!pocket_seq_pattern()->cell[0][0], "pressing REC writes no step");

	/* stopped: the selected step, the pressed key's track */
	panel.key_velocity = 88;
	turn(&panel, POCKET_ENC_KNOB4, 4);
	tap_keys(&panel, KEY_SNARE);
	CHECK(pocket_seq_pattern()->cell[POCKET_TRIG_SNARE][4] == POCKET_CELL(88, 0), "a snare on step 5, at the key velocity");
	tap_keys(&panel, KEY_KICK);
	CHECK(pocket_seq_pattern()->cell[POCKET_TRIG_DRUM1][4] == POCKET_CELL(88, 0), "another track on the same step");
	tap_keys(&panel, KEY_SNARE);
	CHECK(!pocket_seq_pattern()->cell[POCKET_TRIG_SNARE][4], "the same key removes it");
	CHECK(pocket_seq_pattern()->cell[POCKET_TRIG_DRUM1][4] != 0, "the other track stays");
	/* PITCH: the selected track, the pressed note */
	press(&panel, BUTTON(SCL));
	panel.trigger = POCKET_TRIG_DRUM2;
	tap_keys(&panel, KEY(62));
	CHECK(pocket_seq_pattern()->cell[POCKET_TRIG_DRUM2][4] == POCKET_CELL(88, 62), "a note on the selected track");
	CHECK(panel.dirty & (1u << 2), "the voice line, which shows the step's note, is redrawn");
	tap_keys(&panel, KEY(67));
	CHECK(pocket_seq_pattern()->cell[POCKET_TRIG_DRUM2][4] == POCKET_CELL(88, 67), "a different pitch replaces it");
	tap_keys(&panel, KEY(67));
	CHECK(!pocket_seq_pattern()->cell[POCKET_TRIG_DRUM2][4], "the same pitch removes it");
	tap_keys(&panel, KEY(60));
	CHECK(pocket_seq_pattern()->cell[POCKET_TRIG_DRUM2][4] == POCKET_CELL(88, 0), "C4 is the programmed pitch: no override");
	press(&panel, BUTTON(OCT_UP));
	tap_keys(&panel, KEY(60));
	CHECK(pocket_seq_pattern()->cell[POCKET_TRIG_DRUM2][4] == POCKET_CELL(88, 72), "the octave is recorded");
	press(&panel, BUTTON(OCT_DOWN));
	/* changing mode reinterprets nothing */
	const struct pocket_pattern kept = *pocket_seq_pattern();
	press(&panel, BUTTON(SCL));
	CHECK(!memcmp(&kept, pocket_seq_pattern(), sizeof kept), "switching modes keeps every note");
	/* and a DRUM key on a pitched step is another pitch: it replaces, then removes */
	tap_keys(&panel, KEY_LOW_TOM);
	CHECK(pocket_seq_pattern()->cell[POCKET_TRIG_DRUM2][4] == POCKET_CELL(88, 0), "a DRUM key replaces a pitched step");
	tap_keys(&panel, KEY_LOW_TOM);
	CHECK(!pocket_seq_pattern()->cell[POCKET_TRIG_DRUM2][4], "and then removes it");

	/* AUTO STEP moves on after an entry, not after a removal; off by default */
	panel.step = 0;
	panel.auto_step = 2;
	tap_keys(&panel, KEY_KICK);
	CHECK(panel.step == 2, "AUTO STEP: %d", panel.step);
	poll(&panel, KEY_KICK | KEY_SNARE, 0, 0, 0, 0, 0, 0);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	CHECK(panel.step == 4, "a chord is one entry: %d", panel.step);
	panel.step = 2;
	tap_keys(&panel, KEY_KICK);
	CHECK(panel.step == 2, "a removal stays: %d", panel.step);
	panel.auto_step = 0;

	/* playing: quantised, and it never removes */
	clear_pattern();
	pocket_seq_set_step(POCKET_TRIG_CYMBAL, 2, 77, 0);
	press(&panel, BUTTON(PLAY));
	render_peak(1);
	for (int s = 0; s < POCKET_SEQ_STEPS + 2; s++)
	{
		/* a little after each even step, the second time round too */
		while ((long)pocket_seq_position() != (s & 15)) pocket_lxr_render_q23(out, 64, 0);
		if (!(s & 1)) tap_keys(&panel, KEY_HAT);
	}
	int on = 0, off = 0;
	for (int s = 0; s < POCKET_SEQ_STEPS; s++)
	{
		const unsigned c = pocket_seq_pattern()->cell[POCKET_TRIG_CLOSED_HAT][s];
		if (s & 1) off += c != 0;
		else on += c != 0;
	}
	CHECK(on == 8 && !off, "hits land on their steps, and hitting a step again does not erase it: %d on, %d off", on, off);
	CHECK(pocket_seq_pattern()->cell[POCKET_TRIG_CYMBAL][2] == POCKET_CELL(77, 0), "other tracks are preserved");
	CHECK(pocket_seq_playing(), "recording does not interrupt playback");
	press(&panel, BUTTON(PLAY));
	render_peak(4);

	/* a hit late in a step goes to the next, and that step does not play it a second time */
	clear_pattern();
	press(&panel, BUTTON(PLAY));
	render_peak(1);
	while (pocket_seq_position() != 3) pocket_lxr_render_q23(out, 64, 0);
	for (int b = 0; b < 64; b++) pocket_lxr_render_q23(out, 64, 0); /* 3/4 of the step */
	const uint32_t triggers = pocket_seq_triggers();
	tap_keys(&panel, KEY_KICK);
	CHECK(pocket_seq_pattern()->cell[0][4] != 0 && !pocket_seq_pattern()->cell[0][3], "a late hit rounds up");
	while (pocket_seq_position() != 5) pocket_lxr_render_q23(out, 64, 0);
	CHECK(pocket_seq_triggers() == triggers, "no double trigger: %u", pocket_seq_triggers() - triggers);
	while (pocket_seq_position() != 3) pocket_lxr_render_q23(out, 64, 0);
	while (pocket_seq_position() != 5) pocket_lxr_render_q23(out, 64, 0);
	CHECK(pocket_seq_triggers() == triggers + 1, "the next time round it plays: %u", pocket_seq_triggers() - triggers);

	/* BEHIND keeps every hit on the step that was sounding */
	clear_pattern();
	panel.quantise = POCKET_QUANTISE_BEHIND;
	while (pocket_seq_position() != 7) pocket_lxr_render_q23(out, 64, 0);
	for (int b = 0; b < 64; b++) pocket_lxr_render_q23(out, 64, 0);
	tap_keys(&panel, KEY_KICK);
	CHECK(pocket_seq_pattern()->cell[0][7] != 0, "BEHIND stays on the step");
	press(&panel, BUTTON(PLAY));
	render_peak(4);

	/* pitched notes recorded live play at their pitch */
	clear_pattern();
	panel.quantise = POCKET_QUANTISE_NEAREST;
	press(&panel, BUTTON(SCL));
	panel.trigger = POCKET_TRIG_DRUM2;
	press(&panel, BUTTON(PLAY));
	render_peak(1);
	while (pocket_seq_position() != 1) pocket_lxr_render_q23(out, 64, 0);
	tap_keys(&panel, KEY(72));
	CHECK(POCKET_CELL_NOTE(pocket_seq_pattern()->cell[POCKET_TRIG_DRUM2][1]) == 72, "a live note keeps its pitch");
	press(&panel, BUTTON(PLAY));
	press(&panel, BUTTON(SCL));
	press(&panel, BUTTON(REC));
	CHECK(!panel.recording && !strcmp(panel.notice, "REC OFF"), "REC off: %s", panel.notice);
	render_seconds(44100);
}

static void settle(struct pocket_panel* p)
{
	for (int i = 0; i < 40; i++)
	{
		pocket_panel_tick(p, 1);
		pocket_lxr_render_q23(out, 128, 0);
	}
}

static void kit_now(uint8_t* kit)
{
	for (int p = 0; p < POCKET_SOUND_PARAMS; p++) kit[p] = (uint8_t)pocket_lxr_get_param(p);
}

/* PRESETS, ENV, SAVE+ENV, and the way back. The rhythm never moves. */
static void test_kits(void)
{
	struct pocket_panel panel;
	static uint8_t init[POCKET_SOUND_PARAMS], a[POCKET_SOUND_PARAMS], b[POCKET_SOUND_PARAMS], expected[POCKET_SOUND_PARAMS];
	start_engine(&panel);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	press(&panel, BUTTON(EDIT));
	render_peak(8);
	kit_now(init);
	const struct pocket_pattern rhythm = *pocket_seq_pattern();
	press(&panel, BUTTON(PLAY));

	/* PRESETS browses, applies a little at a time, and wraps */
	struct pocket_panel_input in = { 0, 0, { 0 } };
	in.encoders[POCKET_ENC_PRESETS] = 1;
	pocket_panel_apply(&panel, &in);
	CHECK(panel.kit == 1 && panel.kit_cursor, "PRESETS: the first kit: %d", panel.kit);
	CHECK(!strncmp(panel.notice, "KIT CLASSIC", 11), "named: %s", panel.notice);
	panel.dirty = 0;
	pocket_panel_tick(&panel, 1);
	struct pocket_lxr_stats stats;
	pocket_lxr_stats_read(&stats);
	settle(&panel);
	CHECK(!panel.kit_cursor, "the kit is applied within a few polls");
	CHECK(panel.dirty & (1u << 4), "SOUND redraws");
	kit_now(a);
	CHECK(memcmp(a, init, sizeof a), "the kit changed the sound");
	memcpy(expected, init, sizeof expected);
	pocket_kit_generate(expected, 0, 37, POCKET_KIT_WILD_DEFAULT);
	CHECK(!memcmp(a, expected, sizeof a), "the kit is its seed's, and the velocity switches stay on");
	turn(&panel, POCKET_ENC_PRESETS, POCKET_KITS - 1);
	settle(&panel);
	kit_now(a);
	CHECK(panel.kit == 0 && !memcmp(a, init, sizeof a), "PRESETS back to INIT restores the init kit exactly");
	CHECK(!memcmp(&rhythm, pocket_seq_pattern(), sizeof rhythm), "browsing leaves the rhythm");

	/* ENV: a new kit, the rhythm untouched, from the character PRESETS chose */
	turn(&panel, POCKET_ENC_PRESETS, 4); /* INDUSTRIAL */
	settle(&panel);
	const uint16_t seed = panel.kit_seed;
	press(&panel, BUTTON(ENV));
	CHECK(!strcmp(panel.notice, "GENERATING KIT"), "ENV says so: %s", panel.notice);
	CHECK(panel.kit == 4 && panel.kit_seed == seed + 1 && panel.kit_generated, "the next seed of the same character");
	settle(&panel);
	kit_now(a);
	memcpy(expected, init, sizeof expected);
	pocket_kit_generate(expected, 3, panel.kit_seed, POCKET_KIT_WILD_DEFAULT);
	CHECK(!memcmp(a, expected, sizeof a), "ENV's kit is deterministic");
	pocket_panel_tick(&panel, POCKET_GENERATING_MS);
	CHECK(!strncmp(panel.notice, "KIT INDUSTRIAL ", 15), "then the kit's name and seed: %s", panel.notice);
	CHECK(!memcmp(&rhythm, pocket_seq_pattern(), sizeof rhythm), "ENV leaves the rhythm");

	/* SAVE+ENV: one voice, and nothing else */
	panel.trigger = POCKET_TRIG_SNARE;
	press2(&panel, BUTTON(SAVE), BUTTON(ENV));
	settle(&panel);
	kit_now(b);
	CHECK(panel.locks == 0, "a chord does not lock");
	CHECK(b[POCKET_PAR_MIX1] != a[POCKET_PAR_MIX1] || b[POCKET_PAR_VELOD4] != a[POCKET_PAR_VELOD4] || b[POCKET_PAR_COARSE4] != a[POCKET_PAR_COARSE4], "the snare changed");
	static const uint8_t others[] = { POCKET_PAR_COARSE1, POCKET_PAR_COARSE2, POCKET_PAR_COARSE3, POCKET_PAR_COARSE5, POCKET_PAR_COARSE6,
		POCKET_PAR_VELOD1, POCKET_PAR_VELOD2, POCKET_PAR_VELOD3, POCKET_PAR_VELOD5, POCKET_PAR_VELOD6_CLOSED, POCKET_PAR_VELOD6_OPEN,
		POCKET_PAR_VOL1, POCKET_PAR_VOL2, POCKET_PAR_VOL3, POCKET_PAR_VOL5, POCKET_PAR_VOL6, POCKET_PAR_FILTER_FREQ_1, POCKET_PAR_FILTER_FREQ_6 };
	int moved = 0;
	for (unsigned i = 0; i < sizeof others; i++) moved += a[others[i]] != b[others[i]];
	CHECK(!moved, "the other voices stayed: %d moved", moved);
	CHECK(!memcmp(&rhythm, pocket_seq_pattern(), sizeof rhythm), "SAVE+ENV leaves the rhythm");
	/* the two hats are one voice: on either, the same parameters move */
	panel.trigger = POCKET_TRIG_OPEN_HAT;
	press2(&panel, BUTTON(SAVE), BUTTON(ENV));
	settle(&panel);
	kit_now(a);
	CHECK(a[POCKET_PAR_MOD_OSC_GAIN1] != b[POCKET_PAR_MOD_OSC_GAIN1] || a[POCKET_PAR_COARSE6] != b[POCKET_PAR_COARSE6], "the hat voice changed");
	CHECK(a[POCKET_PAR_COARSE4] == b[POCKET_PAR_COARSE4], "and not the snare");

	/* undo: back to the kit before, and forward again */
	kit_now(a);
	panel.trigger = POCKET_TRIG_DRUM1;
	press(&panel, BUTTON(ENV));
	settle(&panel);
	kit_now(b);
	CHECK(memcmp(a, b, sizeof a), "another kit");
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));
	settle(&panel);
	static uint8_t c[POCKET_SOUND_PARAMS];
	kit_now(c);
	CHECK(!memcmp(a, c, sizeof a), "undo restores the kit before, with its edits");
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_UP));
	settle(&panel);
	kit_now(c);
	CHECK(!memcmp(b, c, sizeof b), "and redo goes forward again");

	/* an unsaved kit is not lost by browsing away from it: a turn through the kits is one undo */
	press(&panel, BUTTON(ENV));
	settle(&panel);
	turn(&panel, POCKET_ENC_KNOB1, 3); /* a hand edit on SOUND */
	settle(&panel);
	CHECK(panel.kit_edited, "an edit marks the kit");
	kit_now(a);
	pocket_panel_tick(&panel, POCKET_MERGE_MS);
	const unsigned browsing = pocket_history_used();
	turn(&panel, POCKET_ENC_PRESETS, 1);
	settle(&panel);
	const unsigned one = pocket_history_used() - browsing;
	for (int i = 0; i < 16; i++)
	{
		turn(&panel, POCKET_ENC_PRESETS, 1);
		settle(&panel);
	}
	CHECK(pocket_history_used() - browsing < one + 200, "a browse costs one kit's changes, not one each: %u", pocket_history_used() - browsing);
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));
	settle(&panel);
	kit_now(c);
	CHECK(!memcmp(a, c, sizeof a), "browsing away keeps the edited kit");

	/* a whole kit loaded at full tilt while the pattern plays: nothing dropped, no clipping run away */
	for (int i = 0; i < 40; i++)
	{
		press(&panel, BUTTON(ENV));
		for (int p = 0; p < 6; p++)
		{
			pocket_panel_tick(&panel, 1);
			pocket_lxr_render_q23(out, 128, 0);
		}
	}
	settle(&panel);
	pocket_lxr_stats_read(&stats);
	CHECK(stats.events_dropped == 0, "kit changes dropped %u events", stats.events_dropped);
	CHECK(!memcmp(&rhythm, pocket_seq_pattern(), sizeof rhythm), "forty kits and the same rhythm");
	press(&panel, BUTTON(PLAY));
	render_seconds(44100);
}

static void test_one_track(void)
{
	struct pocket_panel panel;
	start_engine(&panel);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	press(&panel, BUTTON(SEQ));
	panel.trigger = POCKET_TRIG_SNARE;
	struct pocket_pattern before = *pocket_seq_pattern();
	press2(&panel, BUTTON(SAVE), BUTTON(ARP));
	CHECK(panel.locks == 0, "SAVE+ARP does not lock");
	CHECK(panel.seed == 1, "nor take a seed");
	CHECK(memcmp(before.cell[3], pocket_seq_pattern()->cell[3], sizeof before.cell[3]), "SAVE+ARP changed the snare");
	for (int t = 0; t < POCKET_SEQ_TRACKS; t++)
		if (t != 3) CHECK(!memcmp(before.cell[t], pocket_seq_pattern()->cell[t], sizeof before.cell[t]), "SAVE+ARP changed track %d", t);
	const struct pocket_pattern once = *pocket_seq_pattern();
	press2(&panel, BUTTON(SAVE), BUTTON(ARP));
	CHECK(memcmp(&once, pocket_seq_pattern(), sizeof once), "again: another rhythm");
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));
	CHECK(!memcmp(&once, pocket_seq_pattern(), sizeof once), "undone");
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_UP));

	/* SAVE+LFO mutates the selected track only */
	before = *pocket_seq_pattern();
	int changed = 0;
	for (int i = 0; i < 12; i++)
	{
		press2(&panel, BUTTON(SAVE), BUTTON(LFO));
		changed += memcmp(before.cell[3], pocket_seq_pattern()->cell[3], sizeof before.cell[3]) != 0;
		before.cell[3][0] = pocket_seq_pattern()->cell[3][0];
	}
	CHECK(changed > 0, "SAVE+LFO changes the track");
	for (int t = 0; t < POCKET_SEQ_TRACKS; t++)
		if (t != 3) CHECK(!memcmp(before.cell[t], pocket_seq_pattern()->cell[t], sizeof before.cell[t]), "SAVE+LFO changed track %d", t);

	/* a locked track refuses, and says why; SAVE+FX is the lock */
	press2(&panel, BUTTON(SAVE), BUTTON(FX));
	CHECK(panel.locks == 1u << 3, "SAVE+FX locks");
	CHECK(panel.page == POCKET_PAGE_PATTERN, "and is neither MIXER nor the save page");
	CHECK(pocket_panel_leds(&panel) & BUTTON(SAVE), "and lights");
	const struct pocket_pattern locked = *pocket_seq_pattern();
	press2(&panel, BUTTON(SAVE), BUTTON(ARP));
	press2(&panel, BUTTON(SAVE), BUTTON(LFO));
	CHECK(!memcmp(&locked, pocket_seq_pattern(), sizeof locked), "a locked track stays");
	CHECK(!strcmp(panel.notice, "LOCKED SNARE"), "the panel says so: %s", panel.notice);
	press2(&panel, BUTTON(SAVE), BUTTON(FX));
	CHECK(!panel.locks, "again unlocks");

	/* these work on every page: performance does not need PATTERN */
	press(&panel, BUTTON(HOME));
	const struct pocket_pattern main_before = *pocket_seq_pattern();
	press(&panel, BUTTON(ARP));
	CHECK(memcmp(&main_before, pocket_seq_pattern(), sizeof main_before), "ARP from MAIN");
	CHECK(panel.page == POCKET_PAGE_MAIN, "and stays there");
}

static void test_midi(void)
{
	struct pocket_panel panel;
	start_engine(&panel);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	press(&panel, BUTTON(SEQ));
	clear_pattern();

	/* DRUM: a different sound on every note, in the screen's order, or the GM map */
	static const int inOrder[] = { POCKET_TRIG_DRUM1, POCKET_TRIG_DRUM2, POCKET_TRIG_DRUM3, POCKET_TRIG_SNARE, POCKET_TRIG_CYMBAL,
		POCKET_TRIG_CLOSED_HAT, POCKET_TRIG_OPEN_HAT };
	for (int i = 0; i < 7; i++) CHECK(pocket_panel_midi(&panel, 0, 36 + (unsigned)i, 90) == 1 && panel.trigger == inOrder[i], "note %d: %d", 36 + i, panel.trigger);
	CHECK(pocket_panel_midi(&panel, 0, 36 + 9 + 3, 90) == 1 && panel.trigger == POCKET_TRIG_SNARE, "the second bank's snare");
	CHECK(pocket_panel_midi(&panel, 0, 36 + 27, 90) == 1 && panel.trigger == POCKET_TRIG_DRUM1, "and the map repeats past the keys");
	panel.midi_map = 1;
	CHECK(pocket_panel_midi(&panel, 0, 36, 90) == 1 && panel.trigger == POCKET_TRIG_DRUM1, "GM: note 36 is the kick");
	CHECK(pocket_panel_midi(&panel, 0, 46, 90) == 1 && panel.trigger == POCKET_TRIG_OPEN_HAT, "note 46 the open hat");
	CHECK(pocket_panel_midi(&panel, 0, 60, 90) == 0, "note 60 is no GM drum");
	panel.midi_map = 0;
	/* the channel */
	panel.midi_channel = 10;
	CHECK(pocket_panel_midi(&panel, 8, 36, 90) == 0, "another channel is ignored");
	CHECK(pocket_panel_midi(&panel, 9, 36, 90) == 1, "channel 10 plays");
	panel.midi_channel = 0;
	/* recording, with the note's own velocity */
	press(&panel, BUTTON(REC));
	pocket_panel_midi(&panel, 0, 39, 55);
	CHECK(pocket_seq_pattern()->cell[POCKET_TRIG_SNARE][0] == POCKET_CELL(55, 0), "MIDI is recorded with its velocity");
	/* PITCH: the selected voice at the note */
	press(&panel, BUTTON(SCL));
	panel.trigger = POCKET_TRIG_DRUM2;
	CHECK(pocket_panel_midi(&panel, 0, 70, 66) == 1, "PITCH plays any note");
	CHECK(panel.trigger == POCKET_TRIG_DRUM2, "the selection stays");
	CHECK(pocket_seq_pattern()->cell[POCKET_TRIG_DRUM2][0] == POCKET_CELL(66, 70), "and a pitched note is recorded");
}

static void test_global(void)
{
	struct pocket_panel panel;
	static uint16_t band[POCKET_SCREEN_W * POCKET_BAND_ROWS];
	start_engine(&panel);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	press(&panel, BUTTON(GLO));
	/* every row moves something, and stops at its ends */
	const struct { int row; const int8_t* value; int lo, hi; } rows[] = {
		{ 0, &panel.key_mode, 0, 1 }, { 1, &panel.octave, POCKET_OCTAVE_MIN, POCKET_OCTAVE_MAX }, { 2, &panel.key_velocity, 1, 127 },
		{ 3, &panel.follow, 0, 1 }, { 4, &panel.quantise, 0, 1 }, { 5, &panel.auto_step, 0, 4 }, { 9, &panel.style, 0, POCKET_GEN_STYLES - 1 },
		{ 10, &panel.density, 0, 100 }, { 11, &panel.complexity, 0, 100 }, { 13, &panel.mutate_strength, 0, 2 },
		{ 15, &panel.kit_wild, 0, 100 }, { 16, &panel.midi_channel, 0, 16 }, { 17, &panel.midi_map, 0, 1 },
	};
	for (unsigned i = 0; i < sizeof rows / sizeof *rows; i++)
	{
		turn(&panel, POCKET_ENC_KNOB1, rows[i].row - panel.global_row);
		turn(&panel, POCKET_ENC_KNOB2, -500);
		CHECK(*rows[i].value == rows[i].lo, "row %d floor: %d", rows[i].row, *rows[i].value);
		for (unsigned b = 4; b < 12; b++) pocket_panel_draw_band(&panel, NULL, b, band);
		turn(&panel, POCKET_ENC_KNOB2, 500);
		CHECK(*rows[i].value == rows[i].hi, "row %d ceiling: %d", rows[i].row, *rows[i].value);
		for (unsigned b = 4; b < 12; b++) pocket_panel_draw_band(&panel, NULL, b, band);
	}
	turn(&panel, POCKET_ENC_KNOB1, -100);
	CHECK(panel.global_row == 0, "the rows stop at the top");
	turn(&panel, POCKET_ENC_KNOB1, 100);
	CHECK(panel.global_row == 17, "and at the bottom");
	turn(&panel, POCKET_ENC_KNOB1, -100);
	/* TEMPO and SWING are the sequencer's */
	turn(&panel, POCKET_ENC_KNOB1, 6);
	turn(&panel, POCKET_ENC_KNOB2, 10);
	CHECK(pocket_seq_tempo() == 130, "GLOBAL's tempo: %d", pocket_seq_tempo());
	turn(&panel, POCKET_ENC_KNOB1, 1);
	turn(&panel, POCKET_ENC_KNOB2, 8);
	CHECK(pocket_seq_swing() == 58, "GLOBAL's swing: %d", pocket_seq_swing());
	/* LENGTH is the pattern's */
	turn(&panel, POCKET_ENC_KNOB1, 1);
	turn(&panel, POCKET_ENC_KNOB2, 2);
	CHECK(POCKET_PATTERN_STEPS(pocket_seq_pattern()) == 48, "GLOBAL's length: %u", POCKET_PATTERN_STEPS(pocket_seq_pattern()));
	turn(&panel, POCKET_ENC_KNOB2, 50);
	CHECK(POCKET_PATTERN_STEPS(pocket_seq_pattern()) == 64, "it stops at 64");
	turn(&panel, POCKET_ENC_KNOB2, -50);
	CHECK(POCKET_PATTERN_STEPS(pocket_seq_pattern()) == 16, "and at 16");
	/* the seed rows are what ARP and ENV use next */
	turn(&panel, POCKET_ENC_KNOB1, 4);
	turn(&panel, POCKET_ENC_KNOB2, 99);
	press(&panel, BUTTON(ARP));
	CHECK(panel.seed == 100, "the SEED row is the next pattern: %d", panel.seed);
	turn(&panel, POCKET_ENC_KNOB1, 2);
	turn(&panel, POCKET_ENC_KNOB2, 500);
	press(&panel, BUTTON(ENV));
	CHECK(panel.kit_seed == 500, "the KIT SEED row is the next kit: %d", panel.kit_seed);
	/* the knobs do nothing to the sound here */
	CHECK(!panel.kit_edited, "GLOBAL is not a sound edit");
	/* GLO goes on to the diagnostics, and back */
	press(&panel, BUTTON(GLO));
	CHECK(panel.page == POCKET_PAGE_SYSTEM, "GLO again: SYSTEM");
	press(&panel, BUTTON(GLO));
	CHECK(panel.page == POCKET_PAGE_GLOBAL, "and back");
	render_seconds(44100);
}

static void project_state(uint8_t* kit, struct pocket_pattern* pattern)
{
	kit_now(kit);
	*pattern = *pocket_seq_pattern();
}

/* Tap SAVE, a slot, tap SAVE, load: everything a project holds comes back. */
static void test_saving(void)
{
	struct pocket_panel panel;
	static uint8_t a[POCKET_SOUND_PARAMS], b[POCKET_SOUND_PARAMS];
	static struct pocket_pattern saved, now;
	fake_flash_reset();
	start_engine(&panel);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	press(&panel, BUTTON(ARP));
	press(&panel, BUTTON(ENV));
	settle(&panel);
	CHECK(!fake_flash_ops && !fake_flash_reads, "using the instrument touches no Flash: %ld %ld", fake_flash_ops, fake_flash_reads);

	/* a tap opens the page, and it writes nothing */
	press(&panel, BUTTON(SAVE));
	CHECK(panel.page == POCKET_PAGE_STORE, "a tap of SAVE opens PROJECTS");
	CHECK(!panel.locks, "and locks nothing");
	CHECK(!fake_flash_ops && fake_flash_reads, "opening it reads and does not write");

	/* a project: a long pattern, conditions, variations, notes, tempo, swing, generator, kit */
	press(&panel, BUTTON(SEQ));
	poll(&panel, 0, BUTTON(SEQ), 0, 0, 0, 3, 0);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	CHECK(pocket_seq_pattern()->last_page == 3, "SEQ+KNOB3: 64 steps");
	CHECK(panel.page == POCKET_PAGE_PATTERN, "and SEQ stays on PATTERN");
	{
		const int swing = pocket_seq_swing(), tempo_before_chord = pocket_seq_tempo(), step_before_chord = panel.step;
		poll(&panel, 0, BUTTON(SAVE), 0, 0, 0, -3, 0);
		poll(&panel, 0, 0, 0, 0, 0, 0, 0);
		CHECK(pocket_seq_pattern()->last_page == 3 && pocket_seq_swing() == swing, "SAVE+KNOB3 sets neither length nor swing");
		CHECK(panel.page == POCKET_PAGE_PATTERN, "and that is no tap");
		poll(&panel, 0, BUTTON(SEQ), 0, 0, 2, 0, 3);
		poll(&panel, 0, 0, 0, 0, 0, 0, 0);
		CHECK(pocket_seq_tempo() == tempo_before_chord && panel.step == step_before_chord, "SEQ held: the other knobs rest");
		press(&panel, BUTTON(HOME));
		poll(&panel, 0, BUTTON(SEQ), 0, 0, 0, -3, 0);
		poll(&panel, 0, 0, 0, 0, 0, 0, 0);
		CHECK(panel.page == POCKET_PAGE_PATTERN && pocket_seq_pattern()->last_page == 0, "from any page, SEQ opens PATTERN and KNOB3 is its length");
		poll(&panel, 0, BUTTON(SEQ), 0, 0, 0, 3, 0);
		poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	}
	press(&panel, BUTTON(ARP));
	pocket_seq_set_cell(1, 61, POCKET_CELL_WITH_CONDITION(POCKET_CELL(77, 72), POCKET_CONDITION_3_4));
	pocket_seq_set_cell(3, 40, POCKET_CELL_WITH_CONDITION(POCKET_CELL(127, POCKET_TONE_VARIATION(POCKET_VARIATION_ALT_B)), POCKET_CONDITION_75));
	pocket_seq_set_cell(6, 63, POCKET_CELL(1, POCKET_TONE_VARIATION(POCKET_VARIATION_B)));
	pocket_seq_set_tempo(133);
	pocket_seq_set_swing(61);
	panel.density = 66;
	panel.complexity = 44;
	panel.style = 5;
	panel.mutate_strength = POCKET_MUTATE_LARGE;
	panel.locks = 1u << 4;
	panel.kit_wild = 77;
	const uint16_t pattern_seed = panel.seed, kit_seed = panel.kit_seed;
	const uint8_t kit = panel.kit;
	project_state(a, &saved);
	press(&panel, BUTTON(SAVE));
	CHECK(panel.page == POCKET_PAGE_STORE, "from anywhere");
	poll(&panel, 0, 0, 2, 0, 0, 0, 0); /* SELECT chooses the slot */
	CHECK(panel.store_slot == 2, "SELECT moves the slot: %d", panel.store_slot);
	CHECK(panel.trigger == POCKET_TRIG_DRUM1, "not the voice, here");

	/* not while it plays: erasing silences audio */
	const long ops = fake_flash_ops;
	press(&panel, BUTTON(PLAY));
	press(&panel, BUTTON(SAVE));
	CHECK(!strcmp(panel.notice, "STOP TO SAVE") && fake_flash_ops == ops, "a playing pattern is not saved: %s", panel.notice);
	press(&panel, BUTTON(PLAY));
	press(&panel, BUTTON(SAVE));
	CHECK(fake_flash_ops > ops, "stopped, it is");
	CHECK(!strcmp(panel.notice, "SAVED 3"), "and says so: %s", panel.notice);

	/* a slot in use needs the same slot twice */
	const long once = fake_flash_ops;
	press(&panel, BUTTON(SAVE));
	CHECK(fake_flash_ops == once && !strcmp(panel.notice, "OVERWRITE: SAVE AGAIN"), "one press does not overwrite: %s", panel.notice);
	turn(&panel, POCKET_ENC_KNOB1, 1);
	turn(&panel, POCKET_ENC_KNOB1, -1);
	press(&panel, BUTTON(SAVE));
	CHECK(fake_flash_ops == once, "moving away disarms it");
	panel.kit_announce = 0;
	pocket_panel_tick(&panel, POCKET_QUESTION_MS);
	press(&panel, BUTTON(SAVE));
	CHECK(fake_flash_ops == once, "the question lapses");
	press(&panel, BUTTON(SAVE));
	CHECK(fake_flash_ops > once, "the second press does");

	/* everything changes, then it comes back, while playing */
	press(&panel, BUTTON(SEQ));
	press(&panel, BUTTON(ENV));
	settle(&panel);
	press(&panel, BUTTON(EDIT));
	turn(&panel, POCKET_ENC_KNOB1, 9);
	press(&panel, BUTTON(SEQ));
	poll(&panel, 0, BUTTON(SEQ), 0, 0, 0, -1, 0);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	press(&panel, BUTTON(ARP));
	panel.density = 10;
	panel.locks = 0;
	pocket_seq_set_tempo(90);
	pocket_seq_set_swing(50);
	project_state(b, &now);
	CHECK(memcmp(a, b, sizeof a) && memcmp(&saved, &now, sizeof now), "another kit and pattern");
	press(&panel, BUTTON(PLAY));
	press(&panel, BUTTON(SAVE));
	press(&panel, BUTTON(OCT_UP));
	CHECK(!strcmp(panel.notice, "LOADED 3"), "OCT- loads: %s", panel.notice);
	CHECK(panel.octave == 0 && panel.key_velocity == 127, "and is neither the octave nor the velocity here");
	settle(&panel);
	project_state(b, &now);
	CHECK(!memcmp(a, b, sizeof a), "the kit is back, every parameter, levels and pans too");
	CHECK(!memcmp(&saved, &now, sizeof now), "the pattern is back, 64 steps, notes, variations and conditions");
	CHECK(pocket_seq_tempo() == 133 && pocket_seq_swing() == 61, "and tempo and swing");
	CHECK(panel.density == 66 && panel.complexity == 44 && panel.style == 5 && panel.seed == pattern_seed && panel.locks == 1u << 4 && panel.mutate_strength == POCKET_MUTATE_LARGE, "and the generator");
	CHECK(panel.kit == kit && panel.kit_seed == kit_seed && panel.kit_generated && panel.kit_wild == 77, "and the kit's name and seed");
	CHECK(pocket_seq_playing(), "loading did not stop it");
	press(&panel, BUTTON(PLAY));

	/* a load is one undo, of all of it */
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));
	settle(&panel);
	project_state(b, &now);
	CHECK(memcmp(a, b, sizeof a) && memcmp(&saved, &now, sizeof now) && pocket_seq_tempo() == 90 && panel.density == 10 && !panel.locks, "a load is undone, kit, pattern and settings");
	CHECK(panel.page == POCKET_PAGE_STORE, "from the page too");
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_UP));
	settle(&panel);
	project_state(b, &now);
	CHECK(!memcmp(a, b, sizeof a) && !memcmp(&saved, &now, sizeof now) && pocket_seq_tempo() == 133, "and redone");

	/* an empty slot: nothing to load */
	turn(&panel, POCKET_ENC_KNOB1, 3);
	project_state(b, &now);
	press(&panel, BUTTON(OCT_UP));
	CHECK(!memcmp(&now, pocket_seq_pattern(), sizeof now), "an empty slot loads nothing");

	/* every slot, in any order, and none disturbs another */
	for (int slot = 8; slot >= 0; slot--)
	{
		panel.store_slot = (uint8_t)slot;
		pocket_seq_set_tempo(100 + slot);
		if (slot == 2) continue;
		press(&panel, BUTTON(SAVE));
		CHECK(!strncmp(panel.notice, "SAVED", 5), "slot %d saves: %s", slot, panel.notice);
		pocket_panel_tick(&panel, POCKET_NOTICE_MS);
	}
	for (int slot = 0; slot < POCKET_STORE_PROJECTS; slot++)
	{
		panel.store_slot = (uint8_t)slot;
		press(&panel, BUTTON(OCT_UP));
		CHECK(pocket_seq_tempo() == (slot == 2 ? 133 : 100 + slot), "slot %d holds its own: %d", slot, pocket_seq_tempo());
	}

	/* a restart: everything in RAM goes, Flash stays, and so does nothing of the history */
	start_engine(&panel);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));
	CHECK(!strcmp(panel.notice, "NOTHING TO UNDO"), "history does not survive: %s", panel.notice);
	press(&panel, BUTTON(SAVE));
	turn(&panel, POCKET_ENC_KNOB1, 2);
	press(&panel, BUTTON(OCT_UP));
	settle(&panel);
	project_state(b, &now);
	CHECK(!memcmp(a, b, sizeof a) && !memcmp(&saved, &now, sizeof now) && pocket_seq_tempo() == 133, "the project survives a restart");

	/* every saved value is range-checked as it comes in: a good CRC is not a promise */
	uint8_t* record = pocket_store_project(0);
	memset(record, 0xff, POCKET_STORE_PROJECT_BYTES);
	CHECK(pocket_store_commit() == POCKET_STORE_OK, "a record of nonsense, with a good CRC");
	press(&panel, BUTTON(HOME));
	press(&panel, BUTTON(SAVE));
	panel.store_slot = 0;
	static uint16_t band[POCKET_SCREEN_W * POCKET_BAND_ROWS];
	for (unsigned line = 4; line < 12; line++) pocket_panel_draw_band(&panel, NULL, line, band); /* no crash on bad records */
	press(&panel, BUTTON(OCT_UP));
	CHECK(panel.style < POCKET_GEN_STYLES && panel.density <= 100 && panel.complexity <= 100 && panel.mutate_strength < POCKET_MUTATE_STRENGTHS && panel.seed < 10000, "out-of-range values are not believed");
	CHECK(!(panel.locks & 0x80) && pocket_seq_tempo() <= POCKET_SEQ_BPM_MAX && pocket_seq_swing() <= POCKET_SEQ_SWING_MAX, "nor the rest");
	int stray = 0;
	for (int t = 0; t < POCKET_SEQ_TRACKS; t++)
		for (int i = 0; i < POCKET_SEQ_MAX_STEPS; i++)
		{
			const uint32_t c = pocket_seq_pattern()->cell[t][i];
			stray += POCKET_CELL_VELOCITY(c) > 127 || (c >> 20) != 0 || POCKET_CELL_CONDITION(c) >= POCKET_CONDITIONS || POCKET_CELL_VARIATION(c) > POCKET_VARIATION_LAST(t);
		}
	CHECK(!stray, "nor a step out of range: %d", stray);
	settle(&panel);
	render_peak(4); /* the worst of it plays */

	/* what another firmware left in our sectors: erased without asking, never while playing */
	fake_flash_reset();
	memset(fake_flash + 430080, 0x12, 8192);
	press(&panel, BUTTON(HOME));
	press(&panel, BUTTON(PLAY));
	const long before = fake_flash_ops;
	press(&panel, BUTTON(SAVE));
	CHECK(panel.store_state == POCKET_STORE_OCCUPIED, "foreign data, playing: %d", panel.store_state);
	press(&panel, BUTTON(SAVE));
	press(&panel, BUTTON(SAVE));
	press(&panel, BUTTON(OCT_UP));
	CHECK(fake_flash_ops == before && panel.store_state == POCKET_STORE_OCCUPIED, "not while playing, nor a load");
	for (unsigned line = 1; line < 12; line++) pocket_panel_draw_band(&panel, NULL, line, band);
	press(&panel, BUTTON(PLAY));
	press(&panel, BUTTON(SAVE));
	CHECK(panel.store_state == POCKET_STORE_OK && fake_flash_ops > before + 2, "stopped, one SAVE erases what was in the way and saves: %ld", fake_flash_ops - before);
	CHECK(strstr((char*)panel.notice, "SAVED"), "and says it saved: %s", panel.notice);
	press(&panel, BUTTON(HOME));
	fake_flash_reset();
	memset(fake_flash + 430080, 0x12, 8192);
	const long opened = fake_flash_ops;
	press(&panel, BUTTON(SAVE));
	CHECK(panel.store_state == POCKET_STORE_OK && fake_flash_ops == opened + 2, "stopped, opening PROJECTS erases those sectors: %ld", fake_flash_ops - opened);
	uint8_t kept = 0;
	pocket_store_peek(0, 0, &kept, 1);
	CHECK(!kept, "and saves nothing");
	for (unsigned line = 1; line < 12; line++) pocket_panel_draw_band(&panel, NULL, line, band);
	press(&panel, BUTTON(SAVE));
	CHECK(strstr((char*)panel.notice, "SAVED"), "the next press saves: %s", panel.notice);
	press(&panel, BUTTON(HOME));

	/* a failed save says so, and the last good one stands */
	fake_flash_reset();
	start_engine(&panel);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	press(&panel, BUTTON(SAVE));
	press(&panel, BUTTON(SAVE));
	press(&panel, BUTTON(SAVE));
	press(&panel, BUTTON(SAVE));
	press(&panel, BUTTON(SAVE));
	fake_flash_fail_at = fake_flash_ops;
	press(&panel, BUTTON(SAVE));
	CHECK(!strcmp(panel.notice, "SAVE FAILED"), "a failed save says so: %s", panel.notice);
	fake_flash_fail_at = -1;
	uint8_t used = 0;
	pocket_store_peek(0, 0, &used, 1);
	CHECK(panel.store_state >= 0 && used, "and the last one is still there");
}

/* GLOBAL's eight playing settings survive a power-off: they go with every save, a tap of SAVE on
 * GLOBAL stores them alone, and start-up reads them back, writing nothing. */
static void set_global(struct pocket_panel* p, int keys, int octave, int velocity, int follow, int quantise, int step, int channel, int map)
{
	p->key_mode = (int8_t)keys, p->octave = (int8_t)octave, p->key_velocity = (int8_t)velocity, p->follow = (int8_t)follow;
	p->quantise = (int8_t)quantise, p->auto_step = (int8_t)step, p->midi_channel = (int8_t)channel, p->midi_map = (int8_t)map;
}
static int global_is(const struct pocket_panel* p, int keys, int octave, int velocity, int follow, int quantise, int step, int channel, int map)
{
	return p->key_mode == keys && p->octave == octave && p->key_velocity == velocity && p->follow == follow &&
		p->quantise == quantise && p->auto_step == step && p->midi_channel == channel && p->midi_map == map;
}
static void power_on(struct pocket_panel* p)
{
	start_engine(p);
	poll(p, 0, 0, 0, 0, 0, 0, 0);
}
static void test_settings(void)
{
	struct pocket_panel panel;
	fake_flash_reset();
	power_on(&panel);
	const long reads = fake_flash_reads;
	pocket_panel_restore_settings(&panel);
	CHECK(global_is(&panel, POCKET_KEYS_DRUM, 0, 127, 1, POCKET_QUANTISE_NEAREST, 0, 0, 0), "a blank chip: the defaults");
	CHECK(!fake_flash_ops && fake_flash_reads > reads, "start-up reads and writes nothing");

	/* with a project save */
	set_global(&panel, POCKET_KEYS_PITCH, -2, 90, 0, POCKET_QUANTISE_BEHIND, 3, 10, 1);
	press(&panel, BUTTON(SAVE));
	press(&panel, BUTTON(SAVE));
	CHECK(!strcmp(panel.notice, "SAVED 1"), "a project: %s", panel.notice);
	power_on(&panel);
	CHECK(global_is(&panel, POCKET_KEYS_DRUM, 0, 127, 1, POCKET_QUANTISE_NEAREST, 0, 0, 0), "not before they are read");
	pocket_panel_restore_settings(&panel);
	CHECK(global_is(&panel, POCKET_KEYS_PITCH, -2, 90, 0, POCKET_QUANTISE_BEHIND, 3, 10, 1), "a save carries them");

	/* alone, from GLOBAL: stopped, one erase at most, and the projects stay */
	press(&panel, BUTTON(GLO));
	set_global(&panel, POCKET_KEYS_DRUM, 3, 1, 1, POCKET_QUANTISE_NEAREST, 4, 16, 0);
	press(&panel, BUTTON(PLAY));
	long ops = fake_flash_ops;
	press(&panel, BUTTON(SAVE));
	CHECK(!strcmp(panel.notice, "STOP TO SAVE") && fake_flash_ops == ops, "not while playing: %s", panel.notice);
	press(&panel, BUTTON(PLAY));
	for (int i = 0; i < 3; i++)
	{
		ops = fake_flash_ops;
		const long erased = fake_flash_erases;
		press(&panel, BUTTON(SAVE));
		CHECK(!strcmp(panel.notice, "SETTINGS SAVED") && panel.page == POCKET_PAGE_GLOBAL, "a tap of SAVE on GLOBAL: %s", panel.notice);
		CHECK(fake_flash_erases - erased <= 1 && fake_flash_ops > ops, "one erase at most: %ld", fake_flash_erases - erased);
	}
	uint8_t used = 0;
	pocket_store_open();
	pocket_store_peek(0, 0, &used, 1);
	CHECK(used, "the project is still there");
	power_on(&panel);
	pocket_panel_restore_settings(&panel);
	CHECK(global_is(&panel, POCKET_KEYS_DRUM, 3, 1, 1, POCKET_QUANTISE_NEAREST, 4, 16, 0), "and they come back");

	/* a sector from before: no settings, and the defaults */
	pocket_store_set_settings(NULL);
	pocket_store_open();
	pocket_store_project(0);
	CHECK(pocket_store_commit() == POCKET_STORE_OK, "a save as 0.9.1 made it");
	power_on(&panel);
	pocket_panel_restore_settings(&panel);
	CHECK(global_is(&panel, POCKET_KEYS_DRUM, 0, 127, 1, POCKET_QUANTISE_NEAREST, 0, 0, 0), "0.9.1's save: the defaults");

	/* out of range: not believed, one by one */
	static const uint8_t wild[POCKET_STORE_SETTINGS] = { 7, 0x80, 0, 9, 0, 200, 17, 1 };
	pocket_store_set_settings(wild);
	pocket_store_open();
	pocket_store_project(0);
	pocket_store_commit();
	power_on(&panel);
	pocket_panel_restore_settings(&panel);
	CHECK(global_is(&panel, POCKET_KEYS_DRUM, 0, 127, 1, POCKET_QUANTISE_BEHIND, 0, 0, 1), "out-of-range values are left at the default");

	/* the newest good sector: a damaged one is passed over */
	set_global(&panel, POCKET_KEYS_PITCH, 1, 64, 0, POCKET_QUANTISE_NEAREST, 2, 5, 0);
	press(&panel, BUTTON(GLO));
	press(&panel, BUTTON(SAVE));
	set_global(&panel, POCKET_KEYS_PITCH, 2, 70, 0, POCKET_QUANTISE_NEAREST, 2, 6, 0);
	press(&panel, BUTTON(SAVE));
	unsigned newest = 0;
	uint32_t best = 0;
	for (unsigned sct = 0; sct < 4; sct++)
	{
		uint32_t g;
		memcpy(&g, fake_flash + 430080 + sct * 4096 + 16, 4);
		if (fake_flash[430080 + sct * 4096 + 4] == 0 && g >= best && g != ~0u) best = g, newest = sct;
	}
	fake_flash[430080 + newest * 4096 + 40] ^= 1;
	power_on(&panel);
	pocket_panel_restore_settings(&panel);
	CHECK(global_is(&panel, POCKET_KEYS_PITCH, 1, 64, 0, POCKET_QUANTISE_NEAREST, 2, 5, 0), "a damaged newest sector: the one before");

	/* foreign data: nothing read as settings, nothing written by SAVE on GLOBAL */
	fake_flash_reset();
	memset(fake_flash + 430080, 0x12, 4 * 4096);
	power_on(&panel);
	pocket_panel_restore_settings(&panel);
	CHECK(global_is(&panel, POCKET_KEYS_DRUM, 0, 127, 1, POCKET_QUANTISE_NEAREST, 0, 0, 0), "foreign data: the defaults");
	press(&panel, BUTTON(GLO));
	press(&panel, BUTTON(SAVE));
	CHECK(!strcmp(panel.notice, "SAVING OFF: SEE PROJECTS") && !fake_flash_ops, "and SAVE leaves it alone: %s", panel.notice);
	fake_flash_size = 4096;
	pocket_panel_restore_settings(&panel);
	press(&panel, BUTTON(SAVE));
	CHECK(!strcmp(panel.notice, "SAVING OFF: SEE PROJECTS") && !fake_flash_ops, "the wrong partition: %s", panel.notice);
	fake_flash_reset();
}

#define NAME_AT 1332 /* in a record: the rhythm 12, the sound 256, the cells 1,064 */
/* A project's name: KNOB 2 the letter's place, KNOB 3 the letter, saved with it, back with a load. */
static void test_names(void)
{
	struct pocket_panel panel;
	static uint16_t band[POCKET_SCREEN_W * POCKET_BAND_ROWS];
	fake_flash_reset();
	power_on(&panel);
	press(&panel, BUTTON(SAVE));
	CHECK(!memcmp(panel.name, "\0\0\0\0\0\0\0\0", 8), "no name to start with");
	turn(&panel, POCKET_ENC_KNOB3, 1);
	turn(&panel, POCKET_ENC_KNOB2, 1);
	turn(&panel, POCKET_ENC_KNOB3, 2);
	turn(&panel, POCKET_ENC_KNOB2, 1);
	turn(&panel, POCKET_ENC_KNOB3, -1);
	turn(&panel, POCKET_ENC_KNOB2, 20);
	CHECK(panel.name_at == 7, "KNOB 2 stops at the last letter: %d", panel.name_at);
	turn(&panel, POCKET_ENC_KNOB3, 27);
	CHECK(!memcmp(panel.name, "AB-\0\0\0\0" "0", 8), "KNOB 3 the letters: %.8s", panel.name);
	CHECK(panel.store_slot == 0 && !fake_flash_ops, "and nothing else moves, or is written");
	for (unsigned b = 0; b < POCKET_BANDS; b++) pocket_panel_draw_band(&panel, NULL, b, band);
	press(&panel, BUTTON(SAVE));
	CHECK(!strcmp(panel.notice, "SAVED 1"), "saved: %s", panel.notice);
	char name[8];
	pocket_store_peek(0, NAME_AT, name, 8);
	CHECK(!memcmp(name, "AB-\0\0\0\0" "0", 8), "in the record's spare bytes: %.8s", name);
	for (unsigned b = 0; b < POCKET_BANDS; b++) pocket_panel_draw_band(&panel, NULL, b, band);

	/* a restart has no name; a load brings it back, and a slot saved without one has none */
	power_on(&panel);
	press(&panel, BUTTON(SAVE));
	turn(&panel, POCKET_ENC_KNOB1, 1);
	press(&panel, BUTTON(SAVE));
	press(&panel, BUTTON(OCT_UP));
	CHECK(!memcmp(panel.name, "        ", 8), "slot 2 has no name: %.8s", panel.name);
	turn(&panel, POCKET_ENC_KNOB1, -1);
	press(&panel, BUTTON(OCT_UP));
	CHECK(!memcmp(panel.name, "AB-    0", 8), "slot 1 has its own: %.8s", panel.name);

	/* what a record holds is not believed: anything but a letter is a space */
	uint8_t* record = pocket_store_project(0);
	memset(record + NAME_AT, 0xff, 8);
	record[NAME_AT + 1] = 'Z';
	pocket_store_commit();
	press(&panel, BUTTON(OCT_UP));
	CHECK(!memcmp(panel.name, " Z      ", 8), "a bad name is cleaned: %.8s", panel.name);
	for (unsigned b = 0; b < POCKET_BANDS; b++) pocket_panel_draw_band(&panel, NULL, b, band);
	fake_flash_reset();
}

/* A save from before 0.9.2 can hold the LXR's FX parameters. It loads: every other parameter as
 * it was, the FX ones not at all, and a save from it holds 0 where they were. */
#define RECORD_PARAMS 20 /* a record: the rhythm (12 bytes), the sound's head (6), wild, reserved */
static void test_saved_fx(void)
{
	struct pocket_panel panel;
	static uint8_t before[POCKET_SOUND_PARAMS], after[POCKET_SOUND_PARAMS], engine[POCKET_SOUND_PARAMS];
	fake_flash_reset();
	start_engine(&panel);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	press(&panel, BUTTON(ENV));
	settle(&panel);
	kit_now(engine);
	press(&panel, BUTTON(SAVE));
	press(&panel, BUTTON(SAVE));
	CHECK(!strcmp(panel.notice, "SAVED 1"), "a project: %s", panel.notice);
	uint8_t* record = pocket_store_project(0);
	int layout = 0;
	for (int p = 1; p < POCKET_PAR_FX_TYPE; p++) layout += record[RECORD_PARAMS + p] == engine[p];
	CHECK(layout == POCKET_PAR_FX_TYPE - 1, "the record's parameters are where this test writes: %d", layout);
	for (int p = 1; p < POCKET_SOUND_PARAMS; p++) before[p] = record[RECORD_PARAMS + p];
	for (int p = POCKET_PAR_FX_TYPE; p <= POCKET_PAR_FX_DELAY_TYPE; p++)
	{
		CHECK(!record[RECORD_PARAMS + p], "FX parameter %d is saved as 0", p);
		record[RECORD_PARAMS + p] = (uint8_t)(1 + 9 * (p - POCKET_PAR_FX_TYPE));
	}
	CHECK(pocket_store_commit() == POCKET_STORE_OK, "as an older firmware left it");

	press(&panel, BUTTON(HOME));
	press(&panel, BUTTON(ENV));
	settle(&panel);
	press(&panel, BUTTON(SAVE));
	press(&panel, BUTTON(OCT_UP));
	CHECK(!strcmp(panel.notice, "LOADED 1"), "it loads: %s", panel.notice);
	settle(&panel);
	kit_now(after);
	int same = 0;
	for (int p = 1; p < POCKET_SOUND_PARAMS; p++)
		if (!(p >= POCKET_PAR_FX_TYPE && p <= POCKET_PAR_FX_DELAY_TYPE)) same += after[p] == before[p];
	CHECK(same == POCKET_SOUND_PARAMS - 1 - 12, "every other parameter as it was saved: %d", same);
	for (int p = POCKET_PAR_FX_TYPE; p <= POCKET_PAR_FX_DELAY_TYPE; p++)
		CHECK(after[p] == engine[p], "FX parameter %d does not reach the engine: %d", p, after[p]);

	panel.store_slot = 1;
	press(&panel, BUTTON(SAVE));
	record = pocket_store_project(1);
	int zero = 0;
	for (int p = POCKET_PAR_FX_TYPE; p <= POCKET_PAR_FX_DELAY_TYPE; p++) zero += !record[RECORD_PARAMS + p];
	CHECK(zero == 12, "saved again, they are 0: %d", zero);
	render_peak(4);
}

/* What a sound is, independent of how loud: how it moves, how long it lasts, how bright it is. */
#define SHAPE_WINDOW 128
#define SHAPE_WINDOWS 320
struct shape { double envelope[SHAPE_WINDOWS], energy, length, brightness; };

static void shape_of(struct shape* s, const int32_t* samples)
{
	double peak = 0, crossings = 0;
	s->energy = 0;
	for (int w = 0; w < SHAPE_WINDOWS; w++)
	{
		double sum = 0;
		int cross = 0;
		for (int i = 0; i < SHAPE_WINDOW; i++)
		{
			const int32_t x = samples[2 * (w * SHAPE_WINDOW + i)];
			sum += (double)x * x;
			if ((i || w) && (x < 0) != (samples[2 * (w * SHAPE_WINDOW + i - 1)] < 0)) cross++;
		}
		s->envelope[w] = sum;
		s->energy += sum;
		crossings += sum * cross;
		if (sum > peak) peak = sum;
	}
	s->brightness = s->energy > 0 ? crossings / s->energy : 0;
	s->length = 1;
	for (int w = 0; w < SHAPE_WINDOWS; w++)
	{
		if (peak > 0 && s->envelope[w] > peak / 100) s->length = w + 1; /* 20 dB down */
		s->envelope[w] = peak > 0 ? s->envelope[w] / peak : 0;
	}
}

/* 0 for the same sound. The largest of: the ratio of lengths, the ratio of brightness, and how much
 * of the envelope differs, so 0.14 is about 15 % longer, or two semitones, or a seventh of its shape. */
static double shape_distance(const struct shape* a, const struct shape* b)
{
	double moved = 0, area = 0;
	for (int w = 0; w < SHAPE_WINDOWS; w++)
	{
		moved += fabs(a->envelope[w] - b->envelope[w]);
		area += a->envelope[w] + b->envelope[w];
	}
	const double length = fabs(log(a->length / b->length));
	const double bright = a->brightness > 0 && b->brightness > 0 ? fabs(log(a->brightness / b->brightness)) : 9;
	double d = area > 0 ? moved / area : 0;
	if (length > d) d = length;
	return bright > d ? bright : d;
}

#define SOUND_FRAMES (SHAPE_WINDOWS * SHAPE_WINDOW)
static int32_t sound[2 * SOUND_FRAMES];

/* Key k on a fresh engine, kit `kit` of PRESETS. */
static void render_key(struct pocket_panel* panel, int kit, int k)
{
	start_engine(panel);
	poll(panel, 0, 0, 0, 0, 0, 0, 0);
	if (kit)
	{
		turn(panel, POCKET_ENC_PRESETS, kit);
		settle(panel);
		pocket_panel_tick(panel, 2000);
		for (int i = 0; i < 40; i++) pocket_lxr_render_q23(out, 128, 0);
	}
	tap_keys(panel, 1u << k);
	for (long f = 0; f + 128 <= SOUND_FRAMES; f += 128) pocket_lxr_render_q23(sound + 2 * f, 128, 0);
}

#define CLOSE 0.14

static void test_variations(void)
{
	struct pocket_panel panel;
	static struct shape shapes[POCKET_KEYS];
	int close = 0, silent = 0, total = 0;
	double weakest = 1e9;
	for (int kit = 0; kit < POCKET_KITS; kit++)
	{
		for (int k = 0; k < POCKET_KEYS; k++)
		{
			render_key(&panel, kit, k);
			shape_of(&shapes[k], sound);
			silent += shapes[k].energy == 0;
		}
		if (getenv("VARIATIONS"))
		{
			printf("kit %2d:", kit);
			for (int position = 0; position < POCKET_BANK_KEYS; position++)
				printf(" %s%.2f/%.2f/%.2f", shapes[position].energy == 0 || shapes[position + 9].energy == 0 || shapes[position + 18].energy == 0 ? "!" : "",
				       shape_distance(&shapes[position], &shapes[position + 9]), shape_distance(&shapes[position], &shapes[position + 18]), shape_distance(&shapes[position + 9], &shapes[position + 18]));
			printf("  perc %.2f clap %.2f\n", shape_distance(&shapes[5], &shapes[7]), shape_distance(&shapes[1], &shapes[8]));
		}
		/* bank against bank, position by position, and the two sounds sharing a voice */
		for (int position = 0; position < POCKET_BANK_KEYS; position++)
			for (int bank = 0; bank < 3; bank++)
				for (int other = bank + 1; other < 3; other++)
				{
					const double d = shape_distance(&shapes[position + 9 * bank], &shapes[position + 9 * other]);
					total++;
					close += d < CLOSE;
					if (d < weakest) weakest = d;
				}
		for (int bank = 0; bank < 3; bank++)
		{
			total += 2;
			close += shape_distance(&shapes[2 + 9 * bank], &shapes[7 + 9 * bank]) < CLOSE;
			close += shape_distance(&shapes[3 + 9 * bank], &shapes[8 + 9 * bank]) < CLOSE;
		}
	}
	printf("  variations: %d of %d comparisons close, weakest %.2f, across %d kits\n", close, total, weakest, POCKET_KITS);
	CHECK(!silent, "no sound of 27 is silent on any kit: %d", silent);
	CHECK(close * 10 < total, "variations are different sounds, level aside: %d of %d too close", close, total);

	/* per hit: the kit is not touched, and the next plain hit is the plain sound, to the bit:
	   compared with plain hits at the same moments, which use the noise source the same way */
	static uint8_t before[POCKET_SOUND_PARAMS], after[POCKET_SOUND_PARAMS];
	static int32_t plain[2 * SOUND_FRAMES];
	for (int position = 0; position < POCKET_BANK_KEYS; position++)
	{
		for (int run = 0; run < 2; run++)
		{
			start_engine(&panel);
			poll(&panel, 0, 0, 0, 0, 0, 0, 0);
			kit_now(before);
			for (int bank = 1; bank < 3; bank++)
			{
				tap_keys(&panel, 1u << (position + (run ? 9 * bank : 0)));
				render_seconds(4 * 44100 - 128);
				render_seconds(4 * 44100 - 128);
			}
			kit_now(after);
			CHECK(!memcmp(before, after, sizeof before), "position %d: variations leave the kit", position);
			tap_keys(&panel, 1u << position);
			for (long f = 0; f + 128 <= SOUND_FRAMES; f += 128) pocket_lxr_render_q23((run ? sound : plain) + 2 * f, 128, 0);
		}
		/* the tonal voices restart exactly; noise and free-running oscillators differ every hit anyway */
		int64_t worst = 0, peak = 0;
		for (long i = 0; i < 2 * SOUND_FRAMES; i++)
		{
			const int64_t d = llabs((int64_t)sound[i] - plain[i]);
			if (d > worst) worst = d;
			if (llabs((int64_t)plain[i]) > peak) peak = llabs((int64_t)plain[i]);
		}
		struct shape was, is;
		shape_of(&was, plain);
		shape_of(&is, sound);
		if (position <= 2) CHECK(worst * 1000 < peak, "position %d: after both variations the plain hit is the plain sound: %lld of %lld", position, (long long)worst, (long long)peak);
		CHECK(shape_distance(&was, &is) < 0.08, "position %d: and sounds as it did: %.3f", position, shape_distance(&was, &is));
	}
	/* the hats share a voice: a variation on one does not outlive a hit on the other */
	for (int run = 0; run < 2; run++)
	{
		start_engine(&panel);
		poll(&panel, 0, 0, 0, 0, 0, 0, 0);
		tap_keys(&panel, 1u << (run ? 2 + 18 : 2)); /* the closed hat, or its B */
		render_seconds(4 * 44100 - 128);
		tap_keys(&panel, 1u << 3);
		for (long f = 0; f + 128 <= SOUND_FRAMES; f += 128) pocket_lxr_render_q23((run ? sound : plain) + 2 * f, 128, 0);
	}
	{
		struct shape was, is;
		shape_of(&was, plain);
		shape_of(&is, sound);
		CHECK(shape_distance(&was, &is) < 0.08, "a hat's variation ends at the other hat: %.3f", shape_distance(&was, &is));
	}

	/* recorded: stopped and live, it is that variation, and played back it is the key's sound */
	start_engine(&panel);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	clear_pattern();
	press(&panel, BUTTON(REC));
	tap_keys(&panel, 1u << (3 + 9));
	CHECK(pocket_seq_pattern()->cell[POCKET_TRIG_SNARE][0] == POCKET_CELL(127, POCKET_TONE_VARIATION(POCKET_VARIATION_A)), "a variation is recorded as one");
	tap_keys(&panel, 1u << 3);
	CHECK(pocket_seq_pattern()->cell[POCKET_TRIG_SNARE][0] == POCKET_CELL(127, 0), "the plain key replaces it");
	tap_keys(&panel, 1u << 3);
	CHECK(!pocket_seq_pattern()->cell[POCKET_TRIG_SNARE][0], "and then removes it");
	tap_keys(&panel, 1u << (8 + 18));
	CHECK(POCKET_CELL_VARIATION(pocket_seq_pattern()->cell[POCKET_TRIG_SNARE][0]) == POCKET_VARIATION_ALT_B, "the clap's B is its own");
	tap_keys(&panel, 1u << (8 + 18));
	CHECK(!pocket_seq_pattern()->cell[POCKET_TRIG_SNARE][0], "and the same key removes it");
	CHECK(panel.last_key == 8 + 18 + 1, "the screen is told which sound: %d", panel.last_key);
	static uint16_t band[POCKET_SCREEN_W * POCKET_BAND_ROWS];
	pocket_panel_draw_band(&panel, NULL, 2, band);
	press(&panel, BUTTON(PLAY));
	render_peak(1);
	while (pocket_seq_position() != 2) pocket_lxr_render_q23(out, 64, 0);
	tap_keys(&panel, 1u << (0 + 18));
	const int step = pocket_seq_position();
	CHECK(POCKET_CELL_VARIATION(pocket_seq_pattern()->cell[POCKET_TRIG_DRUM1][step]) == POCKET_VARIATION_B, "live too");
	press(&panel, BUTTON(PLAY));
	render_peak(4);

	static int32_t keyed[2 * 128 * 60], stepped[2 * 128 * 60];
	for (int k = 9; k < POCKET_KEYS; k++)
	{
		render_key(&panel, 0, k);
		memcpy(keyed, sound, sizeof keyed);
		start_engine(&panel);
		clear_pattern();
		poll(&panel, 0, 0, 0, 0, 0, 0, 0);
		panel.recording = 1;
		panel.follow = 0;
		tap_keys(&panel, 1u << k);
		panel.recording = 0;
		/* what recording it sounded like has gone; the step plays from the same state */
		start_engine(&panel);
		const uint32_t cell = POCKET_CELL(127, POCKET_TONE_VARIATION(k / 9 + (k % 9 >= 7 ? POCKET_VARIATION_ALT : 0)));
		static const int track[9] = { 0, 1, 2, 3, 4, 5, 6, 2, 3 };
		clear_pattern();
		pocket_seq_set_cell(track[k % 9], 0, cell);
		pocket_seq_play(1);
		for (int b = 0; b < 60; b++) pocket_lxr_render_q23(stepped + 256 * b, 128, 0);
		pocket_seq_play(0);
		CHECK(!memcmp(keyed, stepped, 256 * sizeof(int32_t) * 20), "key %d: played back it is what the key played", k);
	}

	/* PITCH is untouched by any of it */
	start_engine(&panel);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	press(&panel, BUTTON(SCL));
	clear_pattern();
	press(&panel, BUTTON(REC));
	panel.trigger = POCKET_TRIG_DRUM2;
	tap_keys(&panel, KEY(67));
	CHECK(pocket_seq_pattern()->cell[POCKET_TRIG_DRUM2][0] == POCKET_CELL(127, 67), "PITCH records a note, on the selected voice");
	CHECK(!POCKET_CELL_VARIATION(pocket_seq_pattern()->cell[POCKET_TRIG_DRUM2][0]), "and no variation");
}

/* The keyboard as the pattern. Keys are named by their notes here, worked out from the piano and not
 * from the panel's table, so that the two have to agree. */
#define NOTE_KEY(n) ((n) - POCKET_KEY_LOWEST)
static int nth_key(int n, int black)
{
	static const uint8_t blackKeys[12] = { 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0 };
	for (int k = 0; k < POCKET_KEYS; k++)
		if (blackKeys[(POCKET_KEY_LOWEST + k) % 12] == black && n-- == 0) return k;
	return -1;
}
#define WHITE(n) (1u << nth_key(n, 0))
#define BLACK(n) (1u << nth_key(n, 1))
#define HOLD(keys, k1, k2, k3, k4) poll(&panel, keys, 0, 0, k1, k2, k3, k4)
#define OFF() poll(&panel, 0, 0, 0, 0, 0, 0, 0)
#define pattern pocket_seq_pattern() /* which of the two is playing changes with every commit */

static void test_step_keys(void)
{
	struct pocket_panel panel;
	start_engine(&panel);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	clear_pattern();
	CHECK(nth_key(15, 0) == 26 && nth_key(16, 0) == -1, "16 white keys, the last G5");
	CHECK(nth_key(0, 1) == NOTE_KEY(54) && nth_key(1, 1) == NOTE_KEY(56) && nth_key(2, 1) == NOTE_KEY(58) && nth_key(3, 1) == NOTE_KEY(61), "F#3 G#3 A#3 C#4");
	CHECK(!pocket_panel_key_leds(&panel), "no key lamps on MAIN");
	CHECK(!(pocket_panel_leds(&panel) & BUTTON(SEQ)), "nor SEQ's");

	press(&panel, BUTTON(SEQ));
	CHECK(pocket_panel_leds(&panel) & BUTTON(SEQ), "SEQ is lit on PATTERN");

	/* every white key, its own step, and nothing sounds */
	panel.key_velocity = 99;
	const uint32_t hits = panel.hits, triggers = pocket_seq_triggers();
	for (int s = 0; s < POCKET_SEQ_STEPS; s++)
	{
		tap_keys(&panel, WHITE(s));
		for (int i = 0; i < POCKET_SEQ_STEPS; i++)
			CHECK(pattern->cell[POCKET_TRIG_DRUM1][i] == (i <= s ? POCKET_CELL(99, 0) : 0u), "white key %d, step %d: %x", s + 1, i + 1, pattern->cell[POCKET_TRIG_DRUM1][i]);
		CHECK(panel.step == s, "and it is selected");
		CHECK(pocket_panel_key_leds(&panel) & WHITE(s), "and its lamp is lit");
	}
	CHECK(panel.hits == hits && pocket_seq_triggers() == triggers && !panel.flash_ms[0], "stepping is silent");
	CHECK(pocket_lxr_get_param(POCKET_PAR_VELOD1) >= 0 && render_peak(200) == 0, "really: the output is silent");
	for (int s = POCKET_SEQ_STEPS - 1; s >= 0; s--) tap_keys(&panel, WHITE(s));
	for (int s = 0; s < POCKET_SEQ_STEPS; s++) CHECK(!pattern->cell[POCKET_TRIG_DRUM1][s], "tapped again: off");
	CHECK(!(pocket_panel_key_leds(&panel) & WHITE(3)), "and unlit");

	/* the keys that are neither do nothing */
	const uint32_t others = BLACK(4) | BLACK(5) | BLACK(6) | BLACK(7) | BLACK(8) | BLACK(9) | BLACK(10);
	const unsigned step = panel.step;
	tap_keys(&panel, others);
	CHECK(panel.step == step && panel.hits == hits, "the other black keys do nothing");

	/* a tap and a hold: a slow press, and a knob, are not taps, however short the knob's turn */
	HOLD(WHITE(2), 0, 0, 0, 0);
	CHECK(!pattern->cell[POCKET_TRIG_DRUM1][2], "the press itself changes no step");
	CHECK(pocket_panel_key_leds(&panel) & WHITE(2), "but its lamp shows what the tap will do, at once");
	pocket_panel_tick(&panel, POCKET_STEP_HOLD_MS - 1);
	OFF();
	CHECK(pattern->cell[POCKET_TRIG_DRUM1][2] == POCKET_CELL(99, 0), "a quick release is a tap");
	HOLD(WHITE(2), 0, 0, 0, 0);
	pocket_panel_tick(&panel, POCKET_STEP_HOLD_MS);
	HOLD(WHITE(2), 0, 0, 0, 0);
	CHECK(pocket_panel_key_leds(&panel) & WHITE(2), "a hold: the lamp is the step again");
	OFF();
	CHECK(pattern->cell[POCKET_TRIG_DRUM1][2] == POCKET_CELL(99, 0), "a hold does not toggle");

	/* overlapping taps, as fast playing makes them: each is its own */
	HOLD(WHITE(5), 0, 0, 0, 0);
	pocket_panel_tick(&panel, 200);
	HOLD(WHITE(5) | WHITE(6), 0, 0, 0, 0);
	pocket_panel_tick(&panel, 90);
	HOLD(WHITE(6), 0, 0, 0, 0);
	pocket_panel_tick(&panel, 200);
	OFF();
	CHECK(pattern->cell[POCKET_TRIG_DRUM1][5] && pattern->cell[POCKET_TRIG_DRUM1][6], "overlapping taps both land");
	tap_keys(&panel, WHITE(5));
	tap_keys(&panel, WHITE(6));

	/* held: the four knobs, on that step, all of them one undo, whichever way and however slowly */
	HOLD(WHITE(2), 0, 0, 0, 0);
	HOLD(WHITE(2), -2, 0, 0, 0);
	CHECK(POCKET_CELL_VELOCITY(pattern->cell[POCKET_TRIG_DRUM1][2]) == 91, "KNOB1: velocity");
	pocket_panel_tick(&panel, POCKET_MERGE_MS + 1);
	HOLD(WHITE(2), 0, 3, 0, 0);
	CHECK(POCKET_CELL_NOTE(pattern->cell[POCKET_TRIG_DRUM1][2]) == 63, "KNOB2: pitch");
	HOLD(WHITE(2), 0, 0, 1, 0);
	CHECK(POCKET_CELL_CONDITION(pattern->cell[POCKET_TRIG_DRUM1][2]) == POCKET_CONDITION_1_2, "KNOB3: condition");
	HOLD(WHITE(2), 0, 0, 3, 0);
	CHECK(POCKET_CELL_CONDITION(pattern->cell[POCKET_TRIG_DRUM1][2]) == POCKET_CONDITION_2_3, "the conditions in their order");
	HOLD(WHITE(2), 0, 0, 0, 1);
	CHECK(POCKET_CELL_VARIATION(pattern->cell[POCKET_TRIG_DRUM1][2]) == POCKET_VARIATION_A && !POCKET_CELL_NOTE(pattern->cell[POCKET_TRIG_DRUM1][2]), "KNOB4: a variation, in place of the note");
	HOLD(WHITE(2), 0, 0, 0, 40);
	CHECK(POCKET_CELL_VARIATION(pattern->cell[POCKET_TRIG_DRUM1][2]) == POCKET_VARIATION_LAST(POCKET_TRIG_DRUM1), "as far as the track has them");
	HOLD(WHITE(2), 0, -1, 0, 0);
	CHECK(POCKET_CELL_NOTE(pattern->cell[POCKET_TRIG_DRUM1][2]) == 59 && !POCKET_CELL_VARIATION(pattern->cell[POCKET_TRIG_DRUM1][2]), "a note in place of the variation");
	HOLD(WHITE(2), 0, 1, 0, 0);
	CHECK(!POCKET_CELL_TONE(pattern->cell[POCKET_TRIG_DRUM1][2]), "and at C4, the programmed pitch again");
	HOLD(WHITE(2), -200, 0, 200, 0);
	CHECK(POCKET_CELL_VELOCITY(pattern->cell[POCKET_TRIG_DRUM1][2]) == 1 && POCKET_CELL_CONDITION(pattern->cell[POCKET_TRIG_DRUM1][2]) == POCKET_CONDITIONS - 1, "velocity stops at 1: a knob never turns a step off");
	OFF();
	CHECK(pattern->cell[POCKET_TRIG_DRUM1][2], "released, it stays on");
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));
	CHECK(pattern->cell[POCKET_TRIG_DRUM1][2] == POCKET_CELL(99, 0), "one undo takes the whole hold back: %x", pattern->cell[POCKET_TRIG_DRUM1][2]);
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_UP));
	CHECK(POCKET_CELL_VELOCITY(pattern->cell[POCKET_TRIG_DRUM1][2]) == 1, "one redo brings it all");
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));

	/* held again: that is a new edit, and a step that is off stays off */
	HOLD(WHITE(2), 1, 0, 0, 0);
	OFF();
	HOLD(WHITE(2), 1, 0, 0, 0);
	OFF();
	CHECK(POCKET_CELL_VELOCITY(pattern->cell[POCKET_TRIG_DRUM1][2]) == 107, "held twice");
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));
	CHECK(POCKET_CELL_VELOCITY(pattern->cell[POCKET_TRIG_DRUM1][2]) == 103, "two holds are two undos");
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));
	HOLD(WHITE(9), 0, 0, 0, 0);
	HOLD(WHITE(9), 5, 5, 5, 5);
	OFF();
	CHECK(!pattern->cell[POCKET_TRIG_DRUM1][9], "a knob neither edits nor enables a step that is off");

	/* each toggle is an undo of its own */
	tap_keys(&panel, WHITE(10));
	tap_keys(&panel, WHITE(11));
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));
	CHECK(pattern->cell[POCKET_TRIG_DRUM1][10] && !pattern->cell[POCKET_TRIG_DRUM1][11], "undo: the last toggle");
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));
	CHECK(!pattern->cell[POCKET_TRIG_DRUM1][10], "undo: the one before");
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_UP));
	CHECK(pattern->cell[POCKET_TRIG_DRUM1][10], "redo");
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));

	/* the knobs are otherwise as they were */
	const int tempo = pocket_seq_tempo();
	turn(&panel, POCKET_ENC_KNOB2, 2);
	CHECK(pocket_seq_tempo() == tempo + 2, "KNOB2 alone: the tempo");
	HOLD(WHITE(2), 0, 1, 0, 0);
	OFF();
	CHECK(pocket_seq_tempo() == tempo + 2, "held, it is not");
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));

	/* tracks: SELECT, in place, and only that track's steps */
	panel.step = 2;
	turn(&panel, POCKET_ENC_SELECT, POCKET_TRIG_SNARE);
	CHECK(panel.page == POCKET_PAGE_PATTERN && panel.step == 2, "SELECT stays on the step");
	const uint32_t lit = pocket_panel_key_leds(&panel);
	CHECK(!(lit & WHITE(2)), "the lamps are the snare's");
	tap_keys(&panel, WHITE(4));
	CHECK(pattern->cell[POCKET_TRIG_SNARE][4] && !pattern->cell[POCKET_TRIG_DRUM1][4], "the selected track's step and no other");
	turn(&panel, POCKET_ENC_SELECT, -POCKET_TRIG_SNARE);
	CHECK((pocket_panel_key_leds(&panel) & WHITE(2)) && !(pocket_panel_key_leds(&panel) & WHITE(4)), "and back: the kick's");
	HOLD(WHITE(2), 0, 0, 0, 0);
	poll(&panel, WHITE(2), 0, 1, 0, 0, 0, 0);
	OFF();
	CHECK(pattern->cell[POCKET_TRIG_DRUM1][2] && !pattern->cell[POCKET_TRIG_DRUM2][2], "SELECT during a press cancels its tap");
	turn(&panel, POCKET_ENC_SELECT, -1);
	clear_pattern();

	/* pages, at every length: a page past the end cannot be reached, or the pattern lengthened */
	for (int last = 0; last < POCKET_SEQ_PAGES; last++)
	{
		pocket_seq_set_last_page((unsigned)last);
		panel.step = 0;
		for (int p = 0; p < POCKET_SEQ_PAGES; p++)
		{
			panel.dirty = 0;
			tap_keys(&panel, BLACK(p));
			const int expected = p <= last ? p : last;
			if (p > last)
			{
				CHECK(strstr(panel.notice, "PAST THE END") != NULL, "said so: %s", panel.notice);
				CHECK(panel.step / POCKET_SEQ_STEPS == expected, "length %d: stays on page %d: %d", last + 1, expected, panel.step);
				CHECK(pattern->last_page == last, "and is not lengthened");
				continue;
			}
			CHECK(panel.step / POCKET_SEQ_STEPS == p, "black key %d: page %d", p + 1, panel.step / POCKET_SEQ_STEPS);
			CHECK(panel.dirty & (1u << 4), "and the screen follows");
			uint32_t leds = pocket_panel_key_leds(&panel);
			CHECK(leds & BLACK(p), "its lamp");
			for (int q = 0; q < POCKET_SEQ_PAGES; q++) CHECK(q == p || !(leds & BLACK(q)), "only its");
			/* the first and last key of the page: the boundary */
			tap_keys(&panel, WHITE(0));
			tap_keys(&panel, WHITE(15));
			CHECK(pattern->cell[0][p * POCKET_SEQ_STEPS] && pattern->cell[0][p * POCKET_SEQ_STEPS + 15], "page %d: steps %d and %d", p + 1, p * 16 + 1, p * 16 + 16);
			CHECK((pocket_panel_key_leds(&panel) & (WHITE(0) | WHITE(15) | WHITE(1))) == (WHITE(0) | WHITE(15)), "their lamps");
			tap_keys(&panel, WHITE(0));
			tap_keys(&panel, WHITE(15));
		}
		for (int s = 0; s < POCKET_SEQ_MAX_STEPS; s++) CHECK(!pattern->cell[0][s], "nothing left behind");
	}
	/* OCT: the next page, round the pattern's own length, and the velocity is left alone */
	pocket_seq_set_last_page(1);
	panel.step = 3;
	const int velocity = panel.key_velocity;
	press(&panel, BUTTON(OCT_UP));
	CHECK(panel.step == 19 && panel.key_velocity == velocity, "OCT+: the next page");
	press(&panel, BUTTON(OCT_UP));
	CHECK(panel.step == 3 && pattern->last_page == 1, "and round, not longer");
	press(&panel, BUTTON(OCT_UP));
	CHECK(panel.step == 19, "OCT-");
	/* a page turned with a key down: that key has done nothing */
	HOLD(WHITE(4), 0, 0, 0, 0);
	HOLD(WHITE(4) | BLACK(0), 0, 0, 0, 0);
	HOLD(WHITE(4), 0, 0, 0, 0);
	OFF();
	CHECK(!pattern->cell[0][4] && !pattern->cell[0][20], "a press across a page change toggles nothing");
	/* a pattern shortened under the keys: they come with it */
	panel.step = 18;
	poll(&panel, 0, BUTTON(SEQ), 0, 0, 0, -1, 0);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	CHECK(pattern->last_page == 0 && panel.step == 2, "SEQ+KNOB3 shortens, the keys on the page left: %d", panel.step);
	poll(&panel, 0, BUTTON(SEQ), 0, 0, 0, 3, 0);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	CHECK(pattern->last_page == 3, "and it is how a pattern is lengthened");

	/* the playhead, on the step: its lamp the other way round, every step, every page */
	clear_pattern();
	pocket_seq_set_last_page(1);
	pocket_seq_set_tempo(240);
	panel.step = 0;
	tap_keys(&panel, WHITE(1));
	press(&panel, BUTTON(PLAY));
	int seen = 0, flashed = 0, shownPlayhead = 0;
	for (int i = 0; i < 2500; i++)
	{
		render_peak(1);
		const int position = pocket_seq_position();
		if (position == POCKET_SEQ_NOT_PLAYING) continue;
		const uint32_t leds = pocket_panel_key_leds(&panel);
		for (int s = 0; s < POCKET_SEQ_STEPS; s++)
		{
			const int on = (s == 1) ^ (s == position);
			CHECK(!!(leds & WHITE(s)) == on, "position %d, key %d: %x", position, s, leds);
		}
		seen |= 1 << (position / POCKET_SEQ_STEPS);
		shownPlayhead += position < POCKET_SEQ_STEPS;
		CHECK(leds & BLACK(0), "the selected page stays lit");
		flashed |= position >= POCKET_SEQ_STEPS ? 1 << !!(leds & BLACK(1)) : 0;
	}
	CHECK(seen == 3 && shownPlayhead, "both pages played");
	CHECK(flashed == 3, "the page playing, not the one on the keys, flashes");
	/* a toggle while playing, and heard to change: no stopping */
	tap_keys(&panel, WHITE(5));
	CHECK(pattern->cell[0][5], "a toggle while it plays");
	tap_keys(&panel, BLACK(1));
	CHECK(panel.step / POCKET_SEQ_STEPS == 1 && (pocket_panel_key_leds(&panel) & BLACK(1)), "a page while it plays");
	press(&panel, BUTTON(PLAY));
	pocket_seq_set_tempo(tempo);
	clear_pattern();
	pocket_seq_set_last_page(0);
	panel.step = 0;

	/* REC gives the keys back: they play, record, and light nothing */
	press(&panel, BUTTON(REC));
	CHECK(!pocket_panel_key_leds(&panel), "with REC the lamps are off");
	const uint32_t played = panel.hits;
	tap_keys(&panel, KEY_KICK);
	CHECK(panel.hits == played + 1 && pattern->cell[POCKET_TRIG_DRUM1][0], "with REC, a key plays and records");
	tap_keys(&panel, BLACK(1));
	CHECK(panel.step == 0, "and a black key is no page");
	press(&panel, BUTTON(REC));
	CHECK(pocket_panel_key_leds(&panel) & BLACK(0), "without it, the keys are steps again");
	clear_pattern();

	/* changing mode with keys down: nothing sticks, toggles or plays when they come up */
	HOLD(WHITE(3), 0, 0, 0, 0);
	poll(&panel, WHITE(3), BUTTON(HOME), 0, 0, 0, 0, 0);
	const uint32_t before = panel.hits;
	poll(&panel, WHITE(3), 0, 0, 0, 0, 0, 0);
	CHECK(panel.hits == before, "a key held out of SEQ does not play");
	OFF();
	CHECK(!pattern->cell[0][3] && !panel.step_held, "nor toggle when it comes up");
	CHECK(!pocket_panel_key_leds(&panel), "the lamps go out with SEQ");
	/* and the other way */
	poll(&panel, WHITE(3), 0, 0, 0, 0, 0, 0);
	CHECK(panel.hits == before + 1, "a key on MAIN plays");
	poll(&panel, WHITE(3), BUTTON(SEQ), 0, 0, 0, 0, 0);
	poll(&panel, WHITE(3), 0, 0, 2, 0, 0, 0);
	OFF();
	CHECK(!pattern->cell[0][3] && !panel.step_held && panel.hits == before + 1, "held into SEQ it is no step");
	press(&panel, BUTTON(SEQ));
	CHECK(pocket_seq_tempo() == tempo, "");
	/* a hold left by the page: its undo is over */
	tap_keys(&panel, WHITE(7));
	HOLD(WHITE(7), -1, 0, 0, 0);
	poll(&panel, WHITE(7), BUTTON(EDIT), 0, 0, 0, 0, 0);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	CHECK(panel.page == POCKET_PAGE_SOUND && POCKET_CELL_VELOCITY(pattern->cell[panel.trigger][7]) == 95, "leaving during a hold keeps the edit");

	/* outside SEQ, the keyboard is the instrument it was */
	press(&panel, BUTTON(HOME));
	const uint32_t hit = panel.hits;
	tap_keys(&panel, WHITE(0));
	tap_keys(&panel, BLACK(0));
	CHECK(panel.hits == hit + 2, "both colours play");
	CHECK(pattern->cell[0][0] == 0u, "and write no step");
	press(&panel, BUTTON(SCL));
	CHECK(panel.key_mode == POCKET_KEYS_PITCH, "SCL: PITCH");
	const int octave = panel.octave;
	press(&panel, BUTTON(OCT_UP));
	CHECK(panel.octave == octave + 1, "OCT is the octave");
	press(&panel, BUTTON(OCT_UP));
	press(&panel, BUTTON(SEQ));
	tap_keys(&panel, WHITE(8));
	CHECK(pattern->cell[panel.trigger][8] && !POCKET_CELL_NOTE(pattern->cell[panel.trigger][8]), "in PITCH too the keys are steps on PATTERN");
	press(&panel, BUTTON(SCL));
}
#undef pattern

/* Long patterns by the panel's controls. */
static void test_long_patterns(void)
{
	struct pocket_panel panel;
	static uint16_t band[POCKET_SCREEN_W * POCKET_BAND_ROWS];
	struct pocket_panel_status status = { 0 };
	status.state = POCKET_STATE_READY;
	start_engine(&panel);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	press(&panel, BUTTON(SEQ));
	clear_pattern();
	/* KNOB4 stays inside the pattern until it is longer */
	turn(&panel, POCKET_ENC_KNOB4, 15);
	CHECK(panel.step == 15, "the last step of the page");
	turn(&panel, POCKET_ENC_KNOB4, 1);
	CHECK(panel.step == 0, "16 steps: round to the first");
	poll(&panel, 0, BUTTON(SEQ), 0, 0, 0, 1, 0);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	CHECK(POCKET_PATTERN_STEPS(pocket_seq_pattern()) == 32, "SEQ+KNOB3: a page longer");
	turn(&panel, POCKET_ENC_KNOB4, 20);
	CHECK(panel.step == 20, "the cursor reaches the second page");
	poll(&panel, 0, BUTTON(SEQ), 0, 0, 0, 2, 0);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	CHECK(POCKET_PATTERN_STEPS(pocket_seq_pattern()) == 64, "64");
	poll(&panel, 0, BUTTON(SAVE), 0, 0, 0, 0, 2);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	CHECK(panel.step == 52, "SAVE+KNOB4: two pages on, the same place: %d", panel.step);
	CHECK(panel.page == POCKET_PAGE_PATTERN, "all chords: no save page");
	/* stopped entry on every page */
	press(&panel, BUTTON(REC));
	for (int p = 0; p < 4; p++)
	{
		panel.step = (uint8_t)(16 * p + 3);
		tap_keys(&panel, KEY_KICK);
		CHECK(POCKET_CELL_VELOCITY(pocket_seq_pattern()->cell[0][16 * p + 3]) == 127, "entry on page %d", p + 1);
		for (unsigned b = 0; b < POCKET_BANDS; b++) pocket_panel_draw_band(&panel, &status, b, band);
	}
	/* a condition, on the selected step, shown */
	panel.trigger = POCKET_TRIG_DRUM1;
	panel.step = 51;
	poll(&panel, 0, BUTTON(SAVE), 0, 3, 0, 0, 0);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	CHECK(POCKET_CELL_CONDITION(pocket_seq_pattern()->cell[0][51]) == POCKET_CONDITION_1_3, "SAVE+KNOB1: the step's condition");
	CHECK(panel.page == POCKET_PAGE_PATTERN, "no save page");
	poll(&panel, 0, BUTTON(SAVE), 0, 99, 0, 0, 0);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	CHECK(POCKET_CELL_CONDITION(pocket_seq_pattern()->cell[0][51]) == POCKET_CONDITION_75, "it stops at the last");
	for (unsigned b = 0; b < POCKET_BANDS; b++) pocket_panel_draw_band(&panel, &status, b, band);
	poll(&panel, 0, BUTTON(SAVE), 0, -99, 0, 0, 0);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	CHECK(POCKET_CELL_CONDITION(pocket_seq_pattern()->cell[0][51]) == POCKET_CONDITION_ALWAYS, "and at ALWAYS");
	poll(&panel, 0, BUTTON(SAVE), 0, 5, 0, 0, 0);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	/* a condition survives a new velocity and a new tone on the step */
	turn(&panel, POCKET_ENC_KNOB1, -3);
	const uint32_t conditioned = pocket_seq_pattern()->cell[0][51];
	CHECK(POCKET_CELL_VELOCITY(conditioned) == 115 && POCKET_CELL_CONDITION(conditioned) == 5, "velocity keeps the condition");
	tap_keys(&panel, 1u << 9);
	CHECK(POCKET_CELL_CONDITION(pocket_seq_pattern()->cell[0][51]) == 5 && POCKET_CELL_VARIATION(pocket_seq_pattern()->cell[0][51]) == POCKET_VARIATION_A, "so does a new sound");
	/* live recording on page 3, seen on page 3 */
	clear_pattern();
	pocket_seq_set_last_page(3);
	press(&panel, BUTTON(PLAY));
	render_peak(1);
	while (pocket_seq_position() != 36) pocket_lxr_render_q23(out, 64, 0);
	pocket_panel_tick(&panel, 1);
	CHECK(panel.dirty & (1u << 6), "the grid follows a recording to its page");
	tap_keys(&panel, KEY_SNARE);
	CHECK(pocket_seq_pattern()->cell[POCKET_TRIG_SNARE][36] != 0 || pocket_seq_pattern()->cell[POCKET_TRIG_SNARE][37] != 0, "a hit lands on page 3");
	/* and wraps at the end of the fourth */
	while (pocket_seq_position() != 63) pocket_lxr_render_q23(out, 64, 0);
	for (int b = 0; b < 72; b++) pocket_lxr_render_q23(out, 64, 0);
	const int position = pocket_seq_position();
	tap_keys(&panel, KEY_HAT);
	if (position == 63) CHECK(pocket_seq_pattern()->cell[POCKET_TRIG_CLOSED_HAT][0] != 0, "a late hit on the last step rounds to the first");
	press(&panel, BUTTON(PLAY));
	press(&panel, BUTTON(REC));
	/* shortening keeps the steps, and the cursor comes in */
	panel.step = 60;
	pocket_seq_set_step(1, 60, 90, 0);
	poll(&panel, 0, BUTTON(SEQ), 0, 0, 0, -3, 0);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	CHECK(POCKET_PATTERN_STEPS(pocket_seq_pattern()) == 16 && panel.step < 16, "shorter: the cursor is inside: %d", panel.step);
	CHECK(pocket_seq_pattern()->cell[1][60] != 0, "and the steps of the pages left out stay");
	/* ARP and LFO work to the length */
	poll(&panel, 0, BUTTON(SEQ), 0, 0, 0, 3, 0);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	press(&panel, BUTTON(ARP));
	press(&panel, BUTTON(LFO));
	int last = 0;
	for (int t = 0; t < POCKET_SEQ_TRACKS; t++)
		for (int i = 48; i < 64; i++) last += pocket_seq_pattern()->cell[t][i] != 0;
	CHECK(last > 0, "ARP fills the fourth page: %d", last);
	/* MAIN shows where the music is, and every page draws with a long pattern playing */
	press(&panel, BUTTON(PLAY));
	for (int s = 0; s < 40; s++)
	{
		for (int b = 0; b < 24; b++) pocket_lxr_render_q23(out, 64, 0);
		pocket_panel_tick(&panel, 1);
		panel.page = (uint8_t)(s % POCKET_PAGE_STORE);
		for (unsigned b = 0; b < POCKET_BANDS; b++) pocket_panel_draw_band(&panel, &status, b, band);
	}
	press(&panel, BUTTON(PLAY));
	render_seconds(44100);
}

/* One history: kits and patterns, in the order they happened. */
static void test_history(void)
{
	struct pocket_panel panel;
	static uint8_t kits[6][POCKET_SOUND_PARAMS], kit[POCKET_SOUND_PARAMS];
	static struct pocket_pattern patterns[6];
	int seeds[6];
#define MARK(n) do { settle(&panel); kit_now(kits[n]); patterns[n] = *pocket_seq_pattern(); seeds[n] = panel.seed; } while (0)
#define IS(n, what) do { settle(&panel); kit_now(kit); CHECK(!memcmp(kit, kits[n], sizeof kit), "%s: the kit", what); \
	CHECK(!memcmp(pocket_seq_pattern(), &patterns[n], sizeof patterns[n]), "%s: the pattern", what); } while (0)
	fake_flash_reset();
	start_engine(&panel);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));
	CHECK(!strcmp(panel.notice, "NOTHING TO UNDO"), "at first: %s", panel.notice);
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_UP));
	CHECK(!strcmp(panel.notice, "NOTHING TO REDO"), "at first: %s", panel.notice);
	CHECK(panel.page == POCKET_PAGE_MAIN && panel.octave == 0 && panel.key_velocity == 127, "the chords open nothing, and change neither octave nor velocity");

	/* your order: kit, pattern, sound, then back through them, and forward */
	MARK(0);
	press(&panel, BUTTON(ENV));
	MARK(1);
	press(&panel, BUTTON(ARP));
	MARK(2);
	press(&panel, BUTTON(EDIT));
	turn(&panel, POCKET_ENC_KNOB2, 6);
	turn(&panel, POCKET_ENC_KNOB2, 6);
	MARK(3);
	CHECK(kits[3][POCKET_PAR_VELOD1] == kits[2][POCKET_PAR_VELOD1] + 12, "the edit");
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));
	CHECK(!strcmp(panel.notice, "UNDO"), "says UNDO: %s", panel.notice);
	IS(2, "undo the sound");
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));
	IS(1, "undo the pattern");
	CHECK(panel.seed == seeds[1], "the generator's seed goes with it");
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));
	IS(0, "undo the kit");
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));
	CHECK(!strcmp(panel.notice, "NOTHING TO UNDO"), "exhausted: %s", panel.notice);
	IS(0, "and nothing moves");
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_UP));
	CHECK(!strcmp(panel.notice, "REDO"), "says REDO: %s", panel.notice);
	IS(1, "redo the kit");
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_UP));
	IS(2, "redo the pattern");
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_UP));
	IS(3, "redo the sound");
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_UP));
	CHECK(!strcmp(panel.notice, "NOTHING TO REDO"), "exhausted: %s", panel.notice);
	IS(3, "and nothing moves");
	/* and round again, more than once: exact each time, with nothing regenerated */
	for (int round = 0; round < 5; round++)
	{
		for (int n = 2; n >= 0; n--)
		{
			press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));
			IS(n, "down again");
		}
		for (int n = 1; n <= 3; n++)
		{
			press2(&panel, BUTTON(SAVE), BUTTON(OCT_UP));
			IS(n, "up again");
		}
	}
	/* a new edit after an undo forgets the redo */
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));
	press(&panel, BUTTON(LFO));
	MARK(4);
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_UP));
	CHECK(!strcmp(panel.notice, "NOTHING TO REDO"), "redo is gone: %s", panel.notice);
	IS(4, "and what was done stays");
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));
	IS(2, "undo goes back through the new edit");

	/* a turn of a knob is one undo */
	start_engine(&panel);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	press(&panel, BUTTON(EDIT));
	MARK(0);
	for (int i = 0; i < 20; i++) turn(&panel, POCKET_ENC_KNOB1, 1);
	CHECK(pocket_history_used() < 6, "a turn costs a word or two, not a kit: %u", pocket_history_used());
	pocket_panel_tick(&panel, POCKET_MERGE_MS);
	MARK(1);
	turn(&panel, POCKET_ENC_KNOB1, 3);
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));
	IS(1, "a turn after a pause is its own");
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));
	IS(0, "and the turn before it one undo");

	/* steps: a velocity, a pitch, a condition, each undone */
	press(&panel, BUTTON(SEQ));
	press(&panel, BUTTON(ARP));
	panel.trigger = POCKET_TRIG_DRUM1;
	panel.step = 0;
	MARK(0);
	turn(&panel, POCKET_ENC_KNOB1, -2);
	turn(&panel, POCKET_ENC_KNOB1, -2);
	CHECK(pocket_seq_pattern()->cell[0][0] != patterns[0].cell[0][0], "the velocity moved");
	pocket_panel_tick(&panel, POCKET_MERGE_MS);
	MARK(1);
	poll(&panel, 0, BUTTON(SAVE), 0, 4, 0, 0, 0);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	pocket_panel_tick(&panel, POCKET_MERGE_MS);
	MARK(2);
	press(&panel, BUTTON(REC));
	press(&panel, BUTTON(SCL));
	tap_keys(&panel, KEY(72));
	press(&panel, BUTTON(SCL));
	press(&panel, BUTTON(REC));
	MARK(3);
	CHECK(POCKET_CELL_NOTE(patterns[3].cell[0][0]) == 72 && POCKET_CELL_CONDITION(patterns[3].cell[0][0]) == 4, "a note on a conditioned step");
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));
	IS(2, "undo the note");
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));
	IS(1, "undo the condition");
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));
	IS(0, "undo the velocity");
	for (int n = 1; n <= 3; n++)
	{
		press2(&panel, BUTTON(SAVE), BUTTON(OCT_UP));
		IS(n, "redo the step");
	}
	/* a chord entered stopped is one undo; a take while playing is one */
	press(&panel, BUTTON(REC));
	panel.step = 8;
	clear_pattern();
	pocket_history_reset();
	MARK(0);
	poll(&panel, KEY_KICK | KEY_SNARE, 0, 0, 0, 0, 0, 0);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	MARK(1);
	CHECK(patterns[1].cell[0][8] && patterns[1].cell[POCKET_TRIG_SNARE][8], "a chord");
	tap_keys(&panel, KEY_HAT);
	MARK(2);
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));
	IS(1, "the next entry is its own");
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));
	IS(0, "and the chord one undo");
	press(&panel, BUTTON(PLAY));
	render_peak(1);
	MARK(0);
	for (int h = 0; h < 12; h++)
	{
		for (int b = 0; b < 40; b++) pocket_lxr_render_q23(out, 64, 0);
		tap_keys(&panel, KEY_HAT);
		pocket_panel_tick(&panel, 100);
	}
	MARK(1);
	press(&panel, BUTTON(PLAY));
	int take = 0;
	for (int i = 0; i < 16; i++) take += pocket_seq_pattern()->cell[POCKET_TRIG_CLOSED_HAT][i] != 0;
	CHECK(take >= 4, "a take of hits: %d", take);
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));
	IS(0, "a take is one undo");
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_UP));
	IS(1, "and one redo");
	/* a pause in a take starts another */
	press(&panel, BUTTON(PLAY));
	render_peak(1);
	for (int b = 0; b < 40; b++) pocket_lxr_render_q23(out, 64, 0);
	tap_keys(&panel, KEY_KICK);
	pocket_panel_tick(&panel, POCKET_TAKE_MS);
	for (int b = 0; b < 200; b++) pocket_lxr_render_q23(out, 64, 0);
	tap_keys(&panel, KEY_SNARE);
	press(&panel, BUTTON(PLAY));
	const struct pocket_pattern taken = *pocket_seq_pattern();
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));
	CHECK(memcmp(&taken, pocket_seq_pattern(), sizeof taken) && memcmp(&patterns[1], pocket_seq_pattern(), sizeof patterns[1]), "only the second take goes");
	press(&panel, BUTTON(REC));
	render_peak(4);

	/* long patterns with conditions and length, through generation, and a project load */
	fake_flash_reset();
	start_engine(&panel);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	press(&panel, BUTTON(SEQ));
	MARK(0);
	poll(&panel, 0, BUTTON(SEQ), 0, 0, 0, 3, 0);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	pocket_panel_tick(&panel, POCKET_MERGE_MS);
	MARK(1);
	press(&panel, BUTTON(ARP));
	MARK(2);
	pocket_seq_set_tempo(150);
	press(&panel, BUTTON(SAVE));
	press(&panel, BUTTON(SAVE));
	press(&panel, BUTTON(SEQ));
	press(&panel, BUTTON(ENV));
	press(&panel, BUTTON(ARP));
	press(&panel, BUTTON(LFO));
	MARK(3);
	pocket_seq_set_tempo(90);
	press(&panel, BUTTON(SAVE));
	press(&panel, BUTTON(OCT_UP));
	CHECK(pocket_seq_tempo() == 150, "the load's tempo");
	MARK(4);
	IS(4, "a project is loaded");
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));
	IS(3, "undo the load");
	CHECK(pocket_seq_tempo() == 90, "tempo too: %d", pocket_seq_tempo());
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_UP));
	IS(4, "redo the load");
	CHECK(pocket_seq_tempo() == 150, "tempo too: %d", pocket_seq_tempo());
	const long ops = fake_flash_ops;
	for (int i = 0; i < 6; i++) press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));
	settle(&panel);
	CHECK(pocket_seq_pattern()->last_page == 0, "back to one page, by undo");
	CHECK(fake_flash_ops == ops, "undo writes nothing to Flash");
	for (int i = 0; i < 6; i++) press2(&panel, BUTTON(SAVE), BUTTON(OCT_UP));
	IS(4, "and all the way forward");
	CHECK(pocket_seq_pattern()->last_page == 3 && fake_flash_ops == ops, "and redo neither");

	/* levels: what the history holds of the largest thing there is */
	start_engine(&panel);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	press(&panel, BUTTON(SEQ));
	poll(&panel, 0, BUTTON(SEQ), 0, 0, 0, 3, 0);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	panel.density = 100;
	panel.complexity = 100;
	unsigned most = 0;
	for (int i = 0; i < 40; i++)
	{
		const unsigned before = pocket_history_used();
		press(&panel, BUTTON(ARP));
		press(&panel, BUTTON(ENV));
		settle(&panel);
		if (pocket_history_used() > before && pocket_history_used() - before > most) most = pocket_history_used() - before;
	}
	int levels = 0;
	while (press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN)), strcmp(panel.notice, "NOTHING TO UNDO")) levels++;
	printf("  history: %d undo levels of dense 64-step patterns and new kits, %u words the pair at most\n", levels, most);
	CHECK(levels >= 8, "at least eight undo levels at the worst the generators do: %d", levels);
	settle(&panel);
#undef MARK
#undef IS
}

/* SAVE alone is a tap; SAVE with anything else is never one. */
static void test_save_chords(void)
{
	struct pocket_panel panel;
	static const uint32_t others[] = { BUTTON(OCT_DOWN), BUTTON(OCT_UP), BUTTON(ENV), BUTTON(ARP), BUTTON(LFO), BUTTON(FX), BUTTON(SEQ),
		BUTTON(EDIT), BUTTON(HOME), BUTTON(GLO), BUTTON(SCL), BUTTON(PLAY), BUTTON(REC) };
	fake_flash_reset();
	for (unsigned o = 0; o < sizeof others / sizeof *others; o++)
	{
		start_engine(&panel);
		poll(&panel, 0, 0, 0, 0, 0, 0, 0);
		press2(&panel, BUTTON(SAVE), others[o]);
		CHECK(panel.page != POCKET_PAGE_STORE, "SAVE and button %u opens no save page", o);
		CHECK(panel.octave == 0 && panel.key_velocity == 127, "and changes neither octave nor velocity");
		pocket_seq_play(0);
	}
	/* the order of release: the other button let go first or second, the same */
	start_engine(&panel);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	press(&panel, BUTTON(ARP));
	poll(&panel, 0, BUTTON(SAVE), 0, 0, 0, 0, 0);
	poll(&panel, 0, BUTTON(SAVE) | BUTTON(OCT_DOWN), 0, 0, 0, 0, 0);
	poll(&panel, 0, BUTTON(OCT_DOWN), 0, 0, 0, 0, 0);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	CHECK(panel.page == POCKET_PAGE_MAIN && !strcmp(panel.notice, "UNDO"), "SAVE let go first: still the undo: %s", panel.notice);
	CHECK(panel.key_velocity == 127, "and OCT- then does nothing of its own");
	/* a knob or a key under SAVE is no tap either */
	poll(&panel, 0, BUTTON(SAVE), 0, 0, 0, 0, 0);
	poll(&panel, 0, BUTTON(SAVE), 1, 0, 0, 0, 0);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	CHECK(panel.page == POCKET_PAGE_MAIN, "a knob under SAVE");
	poll(&panel, 0, BUTTON(SAVE), 0, 0, 0, 0, 0);
	poll(&panel, KEY_KICK, BUTTON(SAVE), 0, 0, 0, 0, 0);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	CHECK(panel.page == POCKET_PAGE_MAIN, "a key under SAVE");
	/* SAVE held and let go is a tap however long, and only then does anything */
	poll(&panel, 0, BUTTON(SAVE), 0, 0, 0, 0, 0);
	CHECK(panel.page == POCKET_PAGE_MAIN, "nothing on the press");
	for (int i = 0; i < 50; i++) poll(&panel, 0, BUTTON(SAVE), 0, 0, 0, 0, 0);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	CHECK(panel.page == POCKET_PAGE_STORE, "the release opens it");
	/* on the page too: a chord stores nothing */
	const long ops = fake_flash_ops;
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_UP));
	press2(&panel, BUTTON(SAVE), BUTTON(LFO));
	CHECK(fake_flash_ops == ops, "chords store nothing");
	/* a SAVE held since before the panel started is not a press */
	struct pocket_panel fresh;
	pocket_panel_init(&fresh);
	poll(&fresh, 0, BUTTON(SAVE), 0, 0, 0, 0, 0);
	poll(&fresh, 0, 0, 0, 0, 0, 0, 0);
	CHECK(fresh.page == POCKET_PAGE_MAIN, "a button held at the start does not tap");
}

/* The keyboard's pages and steps: ui/pocket_panel.c's stepKeys, as bits of the keys. */
static uint32_t page_key(unsigned page)
{
	static const uint8_t keys[POCKET_SEQ_PAGES] = { 1, 3, 5, 8 };
	return 1u << keys[page];
}

static uint32_t white_key(unsigned step)
{
	static const uint8_t keys[POCKET_SEQ_STEPS] = { 0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26 };
	return 1u << keys[step];
}

#define PAGE_COPY(p) press2((p), BUTTON(SEQ), BUTTON(OCT_DOWN))
#define PAGE_PASTE(p) press2((p), BUTTON(SEQ), BUTTON(OCT_UP))

static void pattern_of_pages(void)
{
	struct pocket_pattern* p = pocket_seq_edit_begin();
	for (unsigned t = 0; t < POCKET_SEQ_TRACKS; t++)
		for (unsigned s = 0; s < POCKET_SEQ_MAX_STEPS; s++) p->cell[t][s] = sample_cell(t, s % POCKET_SEQ_STEPS, s / POCKET_SEQ_STEPS);
	p->last_page = POCKET_SEQ_PAGES - 1;
	pocket_seq_edit_commit();
	pocket_history_reset();
}

static void test_page_copy(void)
{
	struct pocket_panel panel;
	static struct pocket_pattern before, expected;
	char want[32];
	start_engine(&panel);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	press(&panel, BUTTON(SEQ));

	/* lengthening is not copying: a generated page 1, and nothing after it */
	before = *pocket_seq_pattern();
	CHECK(before.last_page == 0, "16 steps to begin with");
	for (int pages = 2; pages <= POCKET_SEQ_PAGES; pages++)
	{
		poll(&panel, 0, BUTTON(SEQ), 0, 0, 0, 1, 0);
		poll(&panel, 0, 0, 0, 0, 0, 0, 0);
		CHECK((int)POCKET_PATTERN_STEPS(pocket_seq_pattern()) == pages * POCKET_SEQ_STEPS, "%d pages", pages);
		CHECK(!memcmp(before.cell, pocket_seq_pattern()->cell, sizeof before.cell), "and the steps are as they were: new pages are empty");
	}

	/* nothing yet to paste: says so, changes nothing */
	PAGE_PASTE(&panel);
	CHECK(!strcmp(panel.notice, "NOTHING COPIED"), "%s", panel.notice);
	CHECK(!memcmp(before.cell, pocket_seq_pattern()->cell, sizeof before.cell), "and pastes nothing");

	/* every page to every page, every track and everything on a step, and each is one undo */
	for (unsigned from = 0; from < POCKET_SEQ_PAGES; from++)
		for (unsigned to = 0; to < POCKET_SEQ_PAGES; to++)
		{
			pattern_of_pages();
			pocket_seq_set_tempo(133);
			pocket_seq_set_swing(61);
			const unsigned trigger = (from + to) % POCKET_SEQ_TRACKS;
			panel.trigger = (uint8_t)trigger;
			before = *pocket_seq_pattern();
			tap_keys(&panel, page_key(from));
			const unsigned step = panel.step;
			PAGE_COPY(&panel);
			snprintf(want, sizeof want, "PAGE %u COPIED", from + 1);
			CHECK(!strcmp(panel.notice, want), "%s", panel.notice);
			CHECK(!memcmp(&before, pocket_seq_pattern(), sizeof before), "a copy changes nothing");
			CHECK(!pocket_history_used(), "and is no undo");
			CHECK(panel.step == step, "and stays on the page");
			tap_keys(&panel, page_key(to));
			PAGE_PASTE(&panel);
			snprintf(want, sizeof want, "PASTED TO PAGE %u", to + 1);
			CHECK(!strcmp(panel.notice, want), "%s", panel.notice);
			expected = before;
			for (unsigned t = 0; t < POCKET_SEQ_TRACKS; t++)
				memcpy(&expected.cell[t][to * POCKET_SEQ_STEPS], &before.cell[t][from * POCKET_SEQ_STEPS], POCKET_SEQ_STEPS * sizeof(uint32_t));
			CHECK(!memcmp(&expected, pocket_seq_pattern(), sizeof expected), "page %u to %u: all of it, and no other page", from + 1, to + 1);
			CHECK(panel.step / POCKET_SEQ_STEPS == to && panel.trigger == trigger, "still on the page and the track");
			CHECK(pocket_seq_tempo() == 133 && pocket_seq_swing() == 61, "tempo and swing are left alone");
			const unsigned used = pocket_history_used();
			if (from == to) CHECK(!used, "a page onto itself leaves no undo");
			else
			{
				CHECK(used >= 2 && used <= 1 + POCKET_SEQ_TRACKS * POCKET_SEQ_STEPS, "a word a step that differed, and a mark: %u", used);
				press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));
				CHECK(!memcmp(&before, pocket_seq_pattern(), sizeof before), "one undo: the page as it was, exactly");
				press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));
				CHECK(!strcmp(panel.notice, "NOTHING TO UNDO"), "one undo, not one a step: %s", panel.notice);
				press2(&panel, BUTTON(SAVE), BUTTON(OCT_UP));
				CHECK(!memcmp(&expected, pocket_seq_pattern(), sizeof expected), "redo: the page pasted");
				press2(&panel, BUTTON(SAVE), BUTTON(OCT_UP));
				CHECK(!strcmp(panel.notice, "NOTHING TO REDO"), "%s", panel.notice);
			}
		}

	/* it overwrites: a page's rests are pasted too */
	pattern_of_pages();
	clear_pattern();
	pocket_seq_set_last_page(POCKET_SEQ_PAGES - 1);
	pocket_seq_set_cell(0, 0, POCKET_CELL(100, 0));
	pocket_seq_set_cell(3, 16 + 5, POCKET_CELL(90, 0));
	tap_keys(&panel, page_key(0));
	PAGE_COPY(&panel);
	tap_keys(&panel, page_key(1));
	PAGE_PASTE(&panel);
	CHECK(pocket_seq_pattern()->cell[0][16] && !pocket_seq_pattern()->cell[3][16 + 5], "a paste replaces, it does not merge");

	/* one copy, many pastes, from what was copied: the page it came from can change */
	pattern_of_pages();
	before = *pocket_seq_pattern();
	tap_keys(&panel, page_key(0));
	PAGE_COPY(&panel);
	pocket_seq_set_cell(2, 3, POCKET_CELL(11, 0));
	for (unsigned to = 1; to < POCKET_SEQ_PAGES; to++)
	{
		tap_keys(&panel, page_key(to));
		PAGE_PASTE(&panel);
		for (unsigned t = 0; t < POCKET_SEQ_TRACKS; t++)
			CHECK(!memcmp(&pocket_seq_pattern()->cell[t][to * POCKET_SEQ_STEPS], before.cell[t], POCKET_SEQ_STEPS * sizeof(uint32_t)), "page %u: what was copied", to + 1);
	}
	const unsigned words = pocket_history_used();
	PAGE_PASTE(&panel);
	CHECK(pocket_history_used() == words, "the same again changes nothing, and leaves nothing to undo");
	/* until the next copy */
	tap_keys(&panel, page_key(2));
	pocket_seq_set_cell(1, 2 * POCKET_SEQ_STEPS + 1, POCKET_CELL(22, 0));
	PAGE_COPY(&panel);
	tap_keys(&panel, page_key(3));
	PAGE_PASTE(&panel);
	CHECK(pocket_seq_pattern()->cell[1][3 * POCKET_SEQ_STEPS + 1] == POCKET_CELL(22, 0), "a new copy replaces the old");

	/* the chords are told apart */
	pattern_of_pages();
	panel.step = 20;
	press(&panel, BUTTON(OCT_UP));
	CHECK(panel.step == 36, "OCT+ alone still turns the page: %d", panel.step);
	press(&panel, BUTTON(OCT_DOWN));
	CHECK(panel.step == 20, "and OCT-");
	PAGE_COPY(&panel);
	CHECK(panel.step == 20, "under SEQ it does not");
	tap_keys(&panel, page_key(2));
	PAGE_PASTE(&panel);
	before = *pocket_seq_pattern();
	poll(&panel, 0, BUTTON(SAVE) | BUTTON(SEQ), 0, 0, 0, 0, 0);
	poll(&panel, 0, BUTTON(SAVE) | BUTTON(SEQ) | BUTTON(OCT_DOWN), 0, 0, 0, 0, 0);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	CHECK(!strcmp(panel.notice, "UNDO"), "SAVE wins: %s", panel.notice);
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_UP));
	CHECK(!memcmp(&before, pocket_seq_pattern(), sizeof before), "redo");

	/* from another page, SEQ opens PATTERN and the chord still works; OCT means nothing else there */
	press(&panel, BUTTON(EDIT));
	PAGE_COPY(&panel);
	CHECK(panel.page == POCKET_PAGE_PATTERN && !strcmp(panel.notice, "PAGE 3 COPIED"), "%s", panel.notice);

	/* with REC on the keys play, and the chord is the same; PITCH's octave is left alone */
	press(&panel, BUTTON(SCL));
	press(&panel, BUTTON(REC));
	PAGE_COPY(&panel);
	CHECK(panel.octave == 0 && !strcmp(panel.notice, "PAGE 3 COPIED"), "with REC on");
	press(&panel, BUTTON(REC));
	press(&panel, BUTTON(SCL));

	/* a key down under the chord: its release is no tap */
	const uint32_t c = pocket_seq_pattern()->cell[panel.trigger][2 * POCKET_SEQ_STEPS + 1];
	poll(&panel, white_key(1), 0, 0, 0, 0, 0, 0);
	poll(&panel, white_key(1), BUTTON(SEQ), 0, 0, 0, 0, 0);
	poll(&panel, white_key(1), BUTTON(SEQ) | BUTTON(OCT_UP), 0, 0, 0, 0, 0);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	CHECK(pocket_seq_pattern()->cell[panel.trigger][2 * POCKET_SEQ_STEPS + 1] == c, "no toggle");

	/* while it plays: the position goes on, and the paste is heard from the next step */
	pattern_of_pages();
	pocket_seq_set_tempo(300);
	tap_keys(&panel, page_key(0));
	PAGE_COPY(&panel);
	tap_keys(&panel, page_key(1));
	press(&panel, BUTTON(PLAY));
	render_peak(200);
	const uint32_t played = pocket_seq_steps_played();
	PAGE_PASTE(&panel);
	render_peak(200);
	CHECK(pocket_seq_steps_played() > played && pocket_seq_playing(), "it does not stop the music");
	press(&panel, BUTTON(PLAY));
	render_peak(4);
}

/* The whole way: 16 steps, 32, copied, edited, a condition, heard to change, saved, restarted. */
/* MIDI clock has the tempo: the knob does nothing, and the screen says so */
static void test_project_buttons(void)
{
	static struct pocket_panel panel;
	fake_flash_reset();
	start_engine(&panel);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	press(&panel, BUTTON(SAVE));
	press(&panel, BUTTON(SAVE));
	CHECK(!strcmp(panel.notice, "SAVED 1"), "SAVE saves: %s", panel.notice);

	/* overwriting: the question stays up long enough to read and answer */
	press(&panel, BUTTON(SAVE));
	CHECK(!strcmp(panel.notice, "OVERWRITE: SAVE AGAIN"), "it asks: %s", panel.notice);
	pocket_panel_tick(&panel, 2500);
	long ops = fake_flash_ops;
	press(&panel, BUTTON(SAVE));
	CHECK(fake_flash_ops > ops && !strcmp(panel.notice, "SAVED 1"), "and 2.5 s later the answer is taken: %s", panel.notice);

	/* OCT+ loads, OCT- erases */
	const int tempo = pocket_seq_tempo();
	pocket_seq_set_tempo(tempo + 9);
	ops = fake_flash_ops;
	press(&panel, BUTTON(OCT_UP));
	CHECK(pocket_seq_tempo() == tempo && fake_flash_ops == ops, "OCT+ loads and writes nothing: %d", pocket_seq_tempo());
	press(&panel, BUTTON(OCT_DOWN));
	CHECK(fake_flash_ops == ops && !strcmp(panel.notice, "ERASE: OCT- AGAIN"), "OCT- asks: %s", panel.notice);
	press(&panel, BUTTON(SAVE));
	CHECK(!strcmp(panel.notice, "OVERWRITE: SAVE AGAIN"), "SAVE does not answer it: %s", panel.notice);
	CHECK(fake_flash_ops == ops, "nothing yet");
	press(&panel, BUTTON(OCT_DOWN));
	CHECK(fake_flash_ops == ops && !strcmp(panel.notice, "ERASE: OCT- AGAIN"), "nor the other way round: %s", panel.notice);
	press(&panel, BUTTON(PLAY));
	press(&panel, BUTTON(OCT_DOWN));
	CHECK(fake_flash_ops == ops && !strcmp(panel.notice, "STOP TO SAVE"), "not while playing: %s", panel.notice);
	press(&panel, BUTTON(PLAY));
	press(&panel, BUTTON(OCT_DOWN));
	press(&panel, BUTTON(OCT_DOWN));
	CHECK(fake_flash_ops > ops && !strcmp(panel.notice, "ERASED 1"), "OCT- again erases: %s", panel.notice);
	pocket_seq_set_tempo(tempo + 9);
	press(&panel, BUTTON(OCT_UP));
	CHECK(pocket_seq_tempo() == tempo + 9, "the slot is empty: nothing to load");
	ops = fake_flash_ops;
	press(&panel, BUTTON(OCT_DOWN));
	press(&panel, BUTTON(OCT_DOWN));
	CHECK(fake_flash_ops == ops, "and nothing to erase");
	press(&panel, BUTTON(SAVE));
	CHECK(!strcmp(panel.notice, "SAVED 1"), "and it saves at the first press: %s", panel.notice);

	/* only that slot goes, and it stays gone over a restart */
	turn(&panel, POCKET_ENC_SELECT, 1);
	press(&panel, BUTTON(SAVE));
	CHECK(!strcmp(panel.notice, "SAVED 2"), "another slot: %s", panel.notice);
	turn(&panel, POCKET_ENC_SELECT, -1);
	press(&panel, BUTTON(OCT_DOWN));
	press(&panel, BUTTON(OCT_DOWN));
	press(&panel, BUTTON(HOME));
	start_engine(&panel);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	press(&panel, BUTTON(SAVE));
	uint8_t record[4];
	pocket_store_peek(0, 0, record, sizeof record);
	CHECK(!record[0], "slot 1 is gone");
	pocket_store_peek(1, 0, record, sizeof record);
	CHECK(record[0], "slot 2 is not");
}

static void test_clock_tempo(void)
{
	static struct pocket_panel panel;
	start_engine(&panel);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	press(&panel, BUTTON(SEQ));
	const int tempo = pocket_seq_tempo();
	pocket_seq_set_external(1);
	pocket_panel_clock(&panel, 0);
	CHECK(panel.clock_shown && (panel.dirty & (1u << 1)), "following shows at once");
	panel.dirty = 0;
	turn(&panel, POCKET_ENC_KNOB2, 7);
	CHECK(pocket_seq_tempo() == tempo, "the tempo knob does nothing while following: %d", pocket_seq_tempo());
	pocket_panel_clock(&panel, 174);
	CHECK(pocket_seq_tempo() == 174 && panel.dirty, "the tempo is the clock's: %d", pocket_seq_tempo());
	panel.dirty = 0;
	pocket_panel_clock(&panel, 174);
	pocket_panel_clock(&panel, 0);
	CHECK(!panel.dirty, "nothing new, nothing to draw");
	pocket_seq_set_external(0);
	pocket_panel_clock(&panel, 0);
	CHECK(!panel.clock_shown && pocket_seq_tempo() == 174, "gone: the last tempo stays");
	turn(&panel, POCKET_ENC_KNOB2, 2);
	CHECK(pocket_seq_tempo() != 174, "and the knob works again: %d", pocket_seq_tempo());
}

static void test_page_to_project(void)
{
	struct pocket_panel panel;
	static struct pocket_pattern saved;
	fake_flash_reset();
	start_engine(&panel);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	press(&panel, BUTTON(SEQ));
	clear_pattern();
	pocket_history_reset();
	for (unsigned s = 0; s < POCKET_SEQ_STEPS; s += 4) tap_keys(&panel, white_key(s));
	CHECK(pocket_seq_pattern()->cell[POCKET_TRIG_DRUM1][4], "four on the floor, from the keys");
	poll(&panel, 0, BUTTON(SEQ), 0, 0, 0, 1, 0);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	PAGE_COPY(&panel);
	tap_keys(&panel, page_key(1));
	PAGE_PASTE(&panel);
	for (unsigned s = 0; s < POCKET_SEQ_STEPS; s++)
		CHECK(pocket_seq_pattern()->cell[0][16 + s] == pocket_seq_pattern()->cell[0][s], "step %u", s + 17);

	/* an edit on page 2 alone */
	tap_keys(&panel, white_key(2));
	CHECK(pocket_seq_pattern()->cell[0][18] && !pocket_seq_pattern()->cell[0][2], "a step added to page 2 is not on page 1");

	/* a condition on step 21, from a held key, turned twice: 2:2. A hold, not a toggle */
	poll(&panel, white_key(4), 0, 0, 0, 0, 0, 0);
	poll(&panel, white_key(4), 0, 0, 0, 0, 2, 0);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	uint32_t c = pocket_seq_pattern()->cell[0][20];
	CHECK(POCKET_CELL_VELOCITY(c) && POCKET_CELL_CONDITION(c) == POCKET_CONDITION_2_2, "a condition, and it stays on");
	CHECK(POCKET_CELL_VELOCITY(c) == POCKET_CELL_VELOCITY(pocket_seq_pattern()->cell[0][4]) && !POCKET_CELL_TONE(c), "velocity and tone as they were");
	CHECK(!POCKET_CELL_CONDITION(pocket_seq_pattern()->cell[0][4]), "page 1 has none");
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));
	CHECK(!POCKET_CELL_CONDITION(pocket_seq_pattern()->cell[0][20]) && POCKET_CELL_VELOCITY(pocket_seq_pattern()->cell[0][20]), "undone: no condition, still on");
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_UP));
	CHECK(pocket_seq_pattern()->cell[0][20] == c, "redone");

	/* heard: passes 2 and 4 play step 21, 1 and 3 do not, and step 5 plays on all */
	pocket_seq_set_tempo(300);
	press(&panel, BUTTON(PLAY));
	unsigned passesWith21 = 0, passesWith5 = 0, seen = 0, last = 0xffffffffu;
	for (int b = 0; b < 8000 && seen < 4u; b++)
	{
		render_peak(1);
		const uint32_t played = pocket_seq_steps_played();
		if (played == last) continue;
		last = played;
		const int at = pocket_seq_position();
		if (at == 4 && pocket_seq_last_tracks() & 1u) passesWith5++;
		if (at == 20 && pocket_seq_last_tracks() & 1u) passesWith21 |= 1u << pocket_seq_pass();
		if (at == 31) seen++;
	}
	CHECK(seen == 4u, "four passes of 32 steps: %u", seen);
	CHECK(passesWith5 == 4u, "step 5 on all: %u", passesWith5);
	CHECK(passesWith21 == 0xau, "step 21 on the second of two, and only then: %x", passesWith21);
	press(&panel, BUTTON(PLAY));
	render_peak(4);

	/* saved, wrecked, and back; then again after a restart */
	saved = *pocket_seq_pattern();
	press(&panel, BUTTON(SAVE));
	press(&panel, BUTTON(SAVE));
	CHECK(!strcmp(panel.notice, "SAVED 1"), "%s", panel.notice);
	press(&panel, BUTTON(SEQ));
	clear_pattern();
	press(&panel, BUTTON(SAVE));
	press(&panel, BUTTON(OCT_UP));
	CHECK(!strcmp(panel.notice, "LOADED 1"), "%s", panel.notice);
	CHECK(!memcmp(&saved, pocket_seq_pattern(), sizeof saved), "32 steps and the condition, loaded");
	CHECK(pocket_seq_pattern()->last_page == 1, "two pages");
	start_engine(&panel);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	press(&panel, BUTTON(SAVE));
	press(&panel, BUTTON(OCT_UP));
	CHECK(!memcmp(&saved, pocket_seq_pattern(), sizeof saved), "and after a restart");
}

int main(int argc, char** argv)
{
	struct pocket_panel panel;
	start_engine(&panel);

	/* buttons held at start do nothing */
	panel.previous_buttons = BUTTON(HOME) | BUTTON(ARP);
	poll(&panel, 0, BUTTON(HOME) | BUTTON(ARP), 0, 0, 0, 0, 0);
	CHECK(panel.page == POCKET_PAGE_MAIN, "starts on MAIN");
	CHECK(panel.key_mode == POCKET_KEYS_DRUM, "starts in DRUM");
	CHECK(!(panel.settings_changed || panel.mutations), "a held button acted");
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	CHECK(render_peak(10) == 0, "panel made sound without a hit");

	/* the programmed-pitch key triggers the selected voice (drum 1) */
	poll(&panel, KEY_KICK, 0, 0, 0, 0, 0, 0);
	CHECK(render_peak(20) > POCKET_Q23_MAX / 20, "the key did not trigger");
	poll(&panel, KEY_KICK, 0, 0, 0, 0, 0, 0); /* held: no retrigger */
	CHECK(panel.hits == 1, "a held key retriggered (%u hits)", panel.hits);
	CHECK(panel.flash_ms[POCKET_TRIG_DRUM1] == POCKET_FLASH_MS, "a hit does not light its activity cell");
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);

	/* navigation: direct buttons, OCT-/OCT+ wrapping, HOME back to MAIN */
	static const struct { uint32_t button; int page; } direct[] = {
		{ BUTTON(SEQ), POCKET_PAGE_PATTERN }, { BUTTON(EDIT), POCKET_PAGE_SOUND },
		{ BUTTON(FX), POCKET_PAGE_MIXER }, { BUTTON(GLO), POCKET_PAGE_GLOBAL }, { BUTTON(GLO), POCKET_PAGE_SYSTEM },
		{ BUTTON(GLO), POCKET_PAGE_GLOBAL }, { BUTTON(HOME), POCKET_PAGE_MAIN }
	};
	for (unsigned i = 0; i < sizeof direct / sizeof direct[0]; i++)
	{
		panel.dirty = 0;
		press(&panel, direct[i].button);
		CHECK(panel.page == direct[i].page, "button %u opened page %d", i, panel.page);
		CHECK(panel.dirty == (1u << POCKET_BANDS) - 1u, "a page change does not redraw the whole screen");
	}
	/* OCT no longer pages */
	press(&panel, BUTTON(OCT_UP));
	press(&panel, BUTTON(OCT_UP));
	CHECK(panel.page == POCKET_PAGE_MAIN, "OCT changed the page (%d)", panel.page);
	press(&panel, BUTTON(SEQ));
	/* sound knobs act only on their page: on PATTERN and MAIN they move no sound parameter */
	{
		const int v = pocket_lxr_get_param(POCKET_PAR_COARSE1);
		poll(&panel, 0, 0, 0, 5, 5, 5, 5);
		press(&panel, BUTTON(HOME));
		poll(&panel, 0, 0, 0, 5, 5, 5, 5);
		CHECK(pocket_lxr_get_param(POCKET_PAR_COARSE1) == v, "a knob edited outside SOUND and MIXER");
	}

	/* SOUND: the four knobs step pitch, decay, filter, drive of the selected voice, clamped */
	press(&panel, BUTTON(EDIT));
	{
		const int pitch = pocket_lxr_get_param(POCKET_PAR_COARSE1), decay = pocket_lxr_get_param(POCKET_PAR_VELOD1);
		const int filter = pocket_lxr_get_param(POCKET_PAR_FILTER_FREQ_1);
		poll(&panel, 0, 0, 0, 5, -3, -200, 300);
		CHECK(pocket_lxr_get_param(POCKET_PAR_COARSE1) == pitch + 5, "KNOB1 does not set pitch");
		CHECK(pocket_lxr_get_param(POCKET_PAR_VELOD1) == decay - 3, "KNOB2 does not set decay");
		CHECK(pocket_lxr_get_param(POCKET_PAR_FILTER_FREQ_1) == 0 && filter > 0, "KNOB3 does not clamp filter at 0");
		CHECK(pocket_lxr_get_param(POCKET_PAR_DRIVE1) == 127, "KNOB4 does not clamp drive at 127");
	}

	/* SELECT picks the voice on every page and wraps */
	pocket_lxr_load_init_kit();
	render_peak(600);
	poll(&panel, 0, 0, 3, 0, 0, 0, 0);
	CHECK(panel.trigger == POCKET_TRIG_SNARE, "SELECT picked trigger %d", panel.trigger);
	CHECK(pocket_panel_param(&panel, POCKET_CTL_DECAY) == POCKET_PAR_VELOD4, "snare decay parameter");
	poll(&panel, 0, 0, -4, 0, 0, 0, 0);
	CHECK(panel.trigger == POCKET_TRIG_OPEN_HAT, "SELECT does not wrap (%d)", panel.trigger);
	CHECK(pocket_panel_param(&panel, POCKET_CTL_DECAY) == POCKET_PAR_VELOD6_OPEN, "open hat decay parameter");
	poll(&panel, 0, 0, 1, 0, 0, 0, 0);
	panel.follow = 0;
	{
		const uint32_t hits = panel.hits;
		poll(&panel, (1u << 0) | (1u << 12) | (1u << 26), 0, 0, 0, 0, 0, 0);
		CHECK(panel.hits == hits + 3, "three keys gave %u hits", panel.hits - hits);
		poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	}
	render_peak(600);

	/* no fake controls: on every voice, moving every control changes the rendered output */
	for (int t = 0; t < POCKET_LXR_TRIGGERS; t++)
		for (int c = 0; c < POCKET_CTLS; c++)
		{
			static int32_t first[2 * 128 * 40], second[2 * 128 * 40];
			pocket_lxr_init(1);
			pocket_panel_init(&panel);
			panel.trigger = (uint8_t)t;
			pocket_lxr_trigger(t, 127, 60);
			for (int b = 0; b < 40; b++) pocket_lxr_render_q23(first + 256 * b, 128, 0);
			pocket_lxr_init(1);
			const int p = pocket_panel_param(&panel, c), v = pocket_lxr_get_param(p);
			pocket_lxr_set_param(p, v > 63 ? v - 50 : v + 50);
			pocket_lxr_trigger(t, 127, 60);
			for (int b = 0; b < 40; b++) pocket_lxr_render_q23(second + 256 * b, 128, 0);
			CHECK(memcmp(first, second, sizeof first) != 0, "control %d of voice %d changes nothing", c, t);
		}
	pocket_lxr_init(1);

	/* MIXER: KNOB1/2 are the selected voice's level and pan, and the pan moves the voice */
	pocket_panel_init(&panel);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0);
	press(&panel, BUTTON(FX));
	CHECK(panel.page == POCKET_PAGE_MIXER, "FX does not open MIXER");
	poll(&panel, 0, 0, 0, -200, 0, 0, 0);
	CHECK(pocket_lxr_get_param(POCKET_PAR_VOL1) == 0, "KNOB1 does not set level");
	hit_key(&panel);
	CHECK(render_peak(20) == 0, "level 0 still sounds");
	poll(&panel, 0, 0, 0, 100, -200, 0, 0);
	CHECK(pocket_lxr_get_param(POCKET_PAR_PAN1) == 0, "KNOB2 does not set pan");
	{
		float left = 0, right = 0;
		hit_key(&panel);
		render_peak(20);
		pocket_lxr_voice_block(0, &left, &right);
		CHECK(left > right, "pan 0 is not left (%f, %f)", left, right);
		poll(&panel, 0, 0, 0, 0, 300, 0, 0);
		hit_key(&panel);
		render_peak(20);
		pocket_lxr_voice_block(0, &left, &right);
		CHECK(right > left, "pan 127 is not right (%f, %f)", left, right);
	}

	/* activity: a MIDI hit names its voice for a moment; time ends it and redraws */
	pocket_panel_init(&panel);
	panel.dirty = 0;
	CHECK(pocket_panel_midi(&panel, 9, 39, 100) == 1, "MIDI note 39 plays");
	CHECK(panel.midi_trigger == POCKET_TRIG_SNARE && panel.flash_ms[POCKET_TRIG_SNARE], "MIDI hit not shown");
	CHECK(panel.dirty & (1u << 3), "MIDI hit does not redraw the MIDI line");
	panel.midi_map = 1;
	CHECK(pocket_panel_midi(&panel, 9, 99, 100) == 0 && panel.midi_trigger == POCKET_TRIG_SNARE, "an unmapped GM note played");
	panel.midi_map = 0;
	pocket_panel_tick(&panel, 1000);
	pocket_panel_midi(&panel, 9, 39, 100);
	panel.dirty = 0;
	pocket_panel_tick(&panel, POCKET_FLASH_MS - 1);
	CHECK(panel.flash_ms[POCKET_TRIG_SNARE] == 1 && !panel.dirty, "the activity cell ended early");
	pocket_panel_tick(&panel, 1000);
	CHECK(!panel.flash_ms[POCKET_TRIG_SNARE] && !panel.midi_flash_ms && panel.dirty, "activity does not end and redraw");

	/* an audio overrun raises a warning that shows, then goes */
	{
		static uint16_t a[POCKET_SCREEN_W * POCKET_BAND_ROWS], b[POCKET_SCREEN_W * POCKET_BAND_ROWS];
		struct pocket_panel_status now = { 0 }, next = { 0 };
		now.state = next.state = POCKET_STATE_READY;
		pocket_panel_init(&panel);
		pocket_panel_draw_band(&panel, &now, 1, a);
		next.overruns = 3;
		panel.dirty = 0;
		pocket_panel_update_status(&panel, &now, &next);
		CHECK(panel.alert_ms == POCKET_ALERT_MS && panel.alert_overruns == 3 && (panel.dirty & 2), "overrun raises no warning");
		CHECK(now.overruns == 3, "status not stored");
		pocket_panel_draw_band(&panel, &now, 1, b);
		CHECK(memcmp(a, b, sizeof a) != 0, "the warning does not change the state line");
		pocket_panel_tick(&panel, POCKET_ALERT_MS);
		pocket_panel_draw_band(&panel, &now, 1, b);
		CHECK(memcmp(a, b, sizeof a) == 0, "the warning does not go away");
		panel.dirty = 0;
		pocket_panel_update_status(&panel, &now, &next);
		CHECK(!panel.alert_ms && !panel.dirty, "no change still redraws");
		/* a level change redraws the meter, SYSTEM numbers redraw only on SYSTEM */
		next.level = 50;
		next.cpu_permille = 100;
		pocket_panel_update_status(&panel, &now, &next);
		CHECK(panel.dirty == (1u << 13), "level redraw %x", panel.dirty);
		panel.page = POCKET_PAGE_SYSTEM;
		panel.dirty = 0;
		next.cpu_permille = 200;
		pocket_panel_update_status(&panel, &now, &next);
		CHECK(panel.dirty && !(panel.dirty & 1), "SYSTEM numbers do not redraw");
		/* the MIDI line shows activity; the pre-READY screens name the firmware */
		static uint16_t c[POCKET_SCREEN_W * POCKET_BAND_ROWS], d[POCKET_SCREEN_W * POCKET_BAND_ROWS];
		pocket_panel_init(&panel);
		pocket_panel_draw_band(&panel, &now, 3, c);
		now.midi_messages = 1;
		pocket_panel_draw_band(&panel, &now, 3, d);
		CHECK(memcmp(c, d, sizeof c) != 0, "MIDI messages do not change the MIDI line");
		pocket_panel_midi(&panel, 9, 39, 100);
		pocket_panel_draw_band(&panel, &now, 3, c); /* at once, before the next status period */
		CHECK(memcmp(c, d, sizeof c) != 0, "a MIDI hit does not change the MIDI line");
	}

	/* every band of every page in every state, at extreme values, stays inside its band
	   buffer (guard words after it); the start screen's state line differs per state */
	{
		static uint16_t guarded[POCKET_SCREEN_W * POCKET_BAND_ROWS + 64];
		static uint16_t first[POCKET_SCREEN_W * POCKET_BAND_ROWS];
		struct pocket_panel_status worst = { 0 };
		worst.cpu_permille = worst.cpu_peak_permille = 65535;
		worst.overruns = worst.dropped = worst.midi_messages = worst.midi_hits = 0xffffffffu;
		worst.level = 255;
		worst.master = 255;
		memcpy(worst.build, "FFFFFFFF", 9);
		pocket_panel_init(&panel);
		panel.hits = 0xffffffffu;
		panel.alert_ms = 1;
		panel.alert_overruns = 0xffffffffu;
		panel.midi_trigger = POCKET_TRIG_DRUM3;
		panel.midi_note = 255;
		panel.midi_flash_ms = 1;
		panel.footer = "HOLD HOME NOW FOR SAFE MODE";
		int intact = 1, distinct = 1;
		for (unsigned page = 0; page < POCKET_PAGES; page++)
			for (unsigned st = 0; st < 6; st++)
			{
				panel.page = (uint8_t)page;
				worst.state = (uint8_t)st; /* 4 and 5 are out of range: shown as an error */
				for (unsigned b = 0; b < POCKET_BANDS; b++)
				{
					for (int i = 0; i < 64; i++) guarded[POCKET_SCREEN_W * POCKET_BAND_ROWS + i] = 0x5a5a;
					pocket_panel_draw_band(&panel, &worst, b, guarded);
					for (int i = 0; i < 64; i++) intact &= guarded[POCKET_SCREEN_W * POCKET_BAND_ROWS + i] == 0x5a5a;
					if (b == 8 && st == 0) memcpy(first, guarded, sizeof first);
					if (b == 8 && (st == POCKET_STATE_SAFE || st == POCKET_STATE_ERROR)) distinct &= memcmp(first, guarded, sizeof first) != 0;
				}
			}
		CHECK(intact, "a band wrote past its buffer");
		CHECK(distinct, "the start screen's state line does not change with the state");
		pocket_panel_draw_band(&panel, NULL, 1, guarded); /* no status: the start screen, no crash */
	}

	test_sequencer();
	test_keyboard();
	test_recording();
	test_kits();
	test_one_track();
	test_midi();
	test_global();
	test_saving();
	test_saved_fx();
	test_settings();
	test_names();
	test_variations();
	test_long_patterns();
	test_step_keys();
	test_history();
	test_save_chords();
	test_page_copy();
	test_page_to_project();
	test_clock_tempo();
	test_project_buttons();

	/* screenshots: each screen as the FM-1 would show it */
	start_engine(&panel);
	panel.hits = 42;
	struct pocket_panel_status status = { 0 };
	status.cpu_permille = 183;
	status.cpu_peak_permille = 241;
	status.level = 46;
	status.midi_messages = 120;
	status.midi_hits = 57;
	status.master = 100;
	memcpy(status.build, "1FE05C9D", 9);
	const char* path = argc > 1 ? argv[1] : "build/host/panel.ppm";
	for (int screen = -1; screen < POCKET_PAGES; screen++)
	{
		char name[256];
		status.state = screen < 0 ? POCKET_STATE_STARTING : POCKET_STATE_READY;
		panel.page = (uint8_t)(screen < 0 ? 0 : screen);
		panel.footer = "HOLD HOME NOW FOR SAFE MODE";
		if (screen == POCKET_PAGE_SOUND) panel.flash_ms[0] = 1;
		snprintf(name, sizeof name, "%s.%d", path, screen + 1);
		write_screen(screen == POCKET_PAGE_MAIN ? path : name, &panel, &status);
	}
	printf("  panel screenshots: %s and %s.N\n", path, path);
	printf("panel: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
