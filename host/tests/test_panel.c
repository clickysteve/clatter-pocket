/*
 * test_panel.c - host tests of the Pocket panel (ui/pocket_panel.c) on the real engine, and a
 * screenshot of the panel as the FM-1 would show it (build/host/panel.ppm).
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdio.h>
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

static void poll(struct pocket_panel* p, uint32_t keys, uint32_t buttons, int select, int k1, int k2, int k3, int k4, int* exited)
{
	struct pocket_panel_input in = { keys, buttons, { 0 } };
	in.encoders[POCKET_ENC_SELECT] = (int16_t)select;
	in.encoders[POCKET_ENC_KNOB1] = (int16_t)k1;
	in.encoders[POCKET_ENC_KNOB2] = (int16_t)k2;
	in.encoders[POCKET_ENC_KNOB3] = (int16_t)k3;
	in.encoders[POCKET_ENC_KNOB4] = (int16_t)k4;
	const int r = pocket_panel_apply(p, &in);
	if (exited) *exited = r;
}
#define BUTTON(b) (1u << POCKET_BTN_##b)

/* A press and a release of one button. */
static void press(struct pocket_panel* p, uint32_t buttons)
{
	poll(p, 0, buttons, 0, 0, 0, 0, 0, NULL);
	poll(p, 0, 0, 0, 0, 0, 0, 0, NULL);
}

#define KEY(note) (1u << ((note) - POCKET_KEY_LOWEST))
#define KEY_PROGRAMMED KEY(60) /* in PITCH */
/* The first bank's key of each trigger. F3 is the kick, and the banks follow every nine keys. */
#define BANK_KEY(position, bank) (POCKET_KEY_LOWEST + (position) + 9 * (bank))
static const int firstKey[POCKET_LXR_TRIGGERS] = { BANK_KEY(0, 0), BANK_KEY(4, 0), BANK_KEY(5, 0), BANK_KEY(1, 0), BANK_KEY(6, 0), BANK_KEY(2, 0), BANK_KEY(3, 0) };
#define KEY_KICK KEY(BANK_KEY(0, 0))
#define KEY_SNARE KEY(BANK_KEY(1, 0))
#define KEY_HAT KEY(BANK_KEY(2, 0))
#define KEY_LOW_TOM KEY(BANK_KEY(4, 0))

static void tap_keys(struct pocket_panel* p, uint32_t keys)
{
	poll(p, keys, 0, 0, 0, 0, 0, 0, NULL);
	poll(p, 0, 0, 0, 0, 0, 0, 0, NULL);
}
/* SAVE or another button held, a second pressed, both released. */
static void press2(struct pocket_panel* p, uint32_t held, uint32_t button)
{
	poll(p, 0, held, 0, 0, 0, 0, 0, NULL);
	poll(p, 0, held | button, 0, 0, 0, 0, 0, NULL);
	poll(p, 0, held, 0, 0, 0, 0, 0, NULL);
	poll(p, 0, 0, 0, 0, 0, 0, 0, NULL);
}

static void hit_key(struct pocket_panel* p)
{
	poll(p, KEY_KICK, 0, 0, 0, 0, 0, 0, NULL);
	poll(p, 0, 0, 0, 0, 0, 0, 0, NULL);
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

/* The sequencer through the panel, on the real engine: what is heard, not what is stored. */
static void test_sequencer(void)
{
	struct pocket_panel panel;
	start_engine(&panel);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL); /* nothing held */
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
	turn(&panel, POCKET_ENC_KNOB1, 12);
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
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));

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
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);

	/* DRUM: the first bank plays each instrument, at its programmed pitch */
	for (int t = 0; t < POCKET_LXR_TRIGGERS; t++)
	{
		start_engine(&panel);
		poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
		tap_keys(&panel, KEY(firstKey[t]));
		CHECK(panel.flash_ms[t] != 0, "key %d lit track %d", firstKey[t], t);
		CHECK(panel.trigger == t, "the selection follows the last key: %d", panel.trigger);
	}
	/* every key of the keyboard does something, the nine positions in the same order in each bank */
	start_engine(&panel);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
	panel.follow = 0;
	static const int order[POCKET_BANK_KEYS] = { POCKET_TRIG_DRUM1, POCKET_TRIG_SNARE, POCKET_TRIG_CLOSED_HAT, POCKET_TRIG_OPEN_HAT,
		POCKET_TRIG_DRUM2, POCKET_TRIG_DRUM3, POCKET_TRIG_CYMBAL, POCKET_TRIG_DRUM3, POCKET_TRIG_SNARE };
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
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
	press(&panel, BUTTON(SCL));
	poll(&panel, 0, 0, POCKET_TRIG_DRUM2, 0, 0, 0, 0, NULL);
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
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
	press(&panel, BUTTON(SEQ));
	clear_pattern();

	/* off: the keys record nothing */
	tap_keys(&panel, KEY_KICK);
	CHECK(!pocket_seq_pattern()->cell[0][0], "REC off records nothing");
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
	poll(&panel, KEY_KICK | KEY_SNARE, 0, 0, 0, 0, 0, 0, NULL);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
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
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
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
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
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
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
	press(&panel, BUTTON(SEQ));
	clear_pattern();

	/* DRUM: the GM map, or the keys' map */
	CHECK(pocket_panel_midi(&panel, 0, 36, 90) == 1 && panel.trigger == POCKET_TRIG_DRUM1, "note 36 is the kick");
	CHECK(pocket_panel_midi(&panel, 0, 46, 90) == 1 && panel.trigger == POCKET_TRIG_OPEN_HAT, "note 46 the open hat");
	CHECK(pocket_panel_midi(&panel, 0, 60, 90) == 0, "note 60 is no GM drum");
	panel.midi_map = 1;
	CHECK(pocket_panel_midi(&panel, 0, 53, 90) == 1 && panel.trigger == POCKET_TRIG_DRUM1, "as the keys: F3 is the kick");
	CHECK(pocket_panel_midi(&panel, 0, 53 + 9 + 1, 90) == 1 && panel.trigger == POCKET_TRIG_SNARE, "the second bank's snare");
	CHECK(pocket_panel_midi(&panel, 0, 53 + 27, 90) == 1 && panel.trigger == POCKET_TRIG_DRUM1, "and the map repeats past the keys");
	panel.midi_map = 0;
	/* the channel */
	panel.midi_channel = 10;
	CHECK(pocket_panel_midi(&panel, 8, 36, 90) == 0, "another channel is ignored");
	CHECK(pocket_panel_midi(&panel, 9, 36, 90) == 1, "channel 10 plays");
	panel.midi_channel = 0;
	/* recording, with the note's own velocity */
	press(&panel, BUTTON(REC));
	pocket_panel_midi(&panel, 0, 38, 55);
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
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
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

/* A bank of kits or patterns as firmware before projects wrote it, on the chip. */
static void old_bank(unsigned kind, unsigned copy, uint32_t generation, const uint8_t slots[12][256])
{
	static uint8_t sector[4096];
	const uint32_t magic = 0x31535043u, commit = 0, format = 1u << 8 | kind;
	memset(sector, 0xff, sizeof sector);
	memset(sector + 20, 0, 12);
	memcpy(sector, &magic, 4);
	memcpy(sector + 4, &commit, 4);
	memcpy(sector + 12, &format, 4);
	memcpy(sector + 16, &generation, 4);
	memcpy(sector + 32, slots, 12 * 256);
	const uint32_t crc = pocket_crc32(0, sector + 12, 3104 - 12);
	memcpy(sector + 8, &crc, 4);
	memcpy(fake_flash + 430080 + (2 * kind + copy) * 4096, sector, sizeof sector);
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
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
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
	poll(&panel, 0, BUTTON(SAVE), 0, 0, 0, 3, 0, NULL);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
	CHECK(pocket_seq_pattern()->last_page == 3, "SAVE+KNOB3: 64 steps");
	CHECK(panel.page == POCKET_PAGE_PATTERN, "and that is no tap");
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
	poll(&panel, 0, 0, 2, 0, 0, 0, 0, NULL); /* SELECT chooses the slot */
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
	pocket_panel_tick(&panel, POCKET_NOTICE_MS);
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
	poll(&panel, 0, BUTTON(SAVE), 0, 0, 0, -1, 0, NULL);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
	press(&panel, BUTTON(ARP));
	panel.density = 10;
	panel.locks = 0;
	pocket_seq_set_tempo(90);
	pocket_seq_set_swing(50);
	project_state(b, &now);
	CHECK(memcmp(a, b, sizeof a) && memcmp(&saved, &now, sizeof now), "another kit and pattern");
	press(&panel, BUTTON(PLAY));
	press(&panel, BUTTON(SAVE));
	press(&panel, BUTTON(OCT_DOWN));
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
	press(&panel, BUTTON(OCT_DOWN));
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
		press(&panel, BUTTON(OCT_DOWN));
		CHECK(pocket_seq_tempo() == (slot == 2 ? 133 : 100 + slot), "slot %d holds its own: %d", slot, pocket_seq_tempo());
	}

	/* a restart: everything in RAM goes, Flash stays, and so does nothing of the history */
	start_engine(&panel);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));
	CHECK(!strcmp(panel.notice, "NOTHING TO UNDO"), "history does not survive: %s", panel.notice);
	press(&panel, BUTTON(SAVE));
	turn(&panel, POCKET_ENC_KNOB1, 2);
	press(&panel, BUTTON(OCT_DOWN));
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
	press(&panel, BUTTON(OCT_DOWN));
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

	/* what the page says when it cannot save, and that nothing is then written */
	fake_flash_reset();
	memset(fake_flash + 430080, 0x12, 8192);
	press(&panel, BUTTON(HOME));
	press(&panel, BUTTON(SAVE));
	CHECK(panel.store_state == POCKET_STORE_OCCUPIED, "foreign data: %d", panel.store_state);
	const long before = fake_flash_ops;
	press(&panel, BUTTON(SAVE));
	press(&panel, BUTTON(OCT_DOWN));
	CHECK(fake_flash_ops == before, "the page cannot write then");
	for (unsigned line = 4; line < 12; line++) pocket_panel_draw_band(&panel, NULL, line, band);
	press(&panel, BUTTON(HOME));

	/* a failed save says so, and the last good one stands */
	fake_flash_reset();
	start_engine(&panel);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
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
	pocket_store_peek(0, &used, 1);
	CHECK(panel.store_state >= 0 && used, "and the last one is still there");
}

/* Saves from before projects: kept, listed, loaded, and given up only when asked. */
static void test_old_saves(void)
{
	struct pocket_panel panel;
	static uint8_t kits[12][256], patterns[12][256], k[POCKET_SOUND_PARAMS];
	fake_flash_reset();
	/* a kit, as the old firmware laid it out: used, kit, generated, edited, seed, wild, reserved, params */
	kits[1][0] = 1;
	kits[1][1] = 4;
	kits[1][2] = 1;
	kits[1][3] = 1;
	kits[1][4] = 149;
	kits[1][6] = 66;
	pocket_kit_generate(kits[1] + 8, 3, 149, 50);
	/* a pattern: used, swing, density, complexity, style, mutate, locks, reserved, seed, tempo, cells */
	static const uint8_t head[12] = { 1, 58, 70, 20, 2, 2, 1 << 3, 0, 9, 0, 140, 0 };
	memcpy(patterns[4], head, sizeof head);
	const uint16_t cell = POCKET_CELL(99, 72);
	memcpy(patterns[4] + 12 + 3 * 32 + 5 * 2, &cell, 2);
	old_bank(0, 1, 7, kits);
	old_bank(1, 0, 3, patterns);
	static uint8_t chip[16384];
	memcpy(chip, fake_flash + 430080, sizeof chip);
	start_engine(&panel);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
	const long ops = fake_flash_ops;

	press(&panel, BUTTON(SAVE));
	CHECK(panel.store_state == POCKET_STORE_OK, "old banks open: %d", panel.store_state);
	CHECK(pocket_store_room() == 3, "and hold back all but a sector of projects: %u", pocket_store_room());
	static uint16_t band[POCKET_SCREEN_W * POCKET_BAND_ROWS];
	for (int row = 0; row < 40; row++)
	{
		turn(&panel, POCKET_ENC_KNOB1, 1);
		for (unsigned line = 4; line < 12; line++) pocket_panel_draw_band(&panel, NULL, line, band);
	}
	CHECK(panel.store_slot == 33, "the rows end at ERASE OLD SAVES: %d", panel.store_slot);
	/* the old kit loads */
	panel.store_slot = 10;
	press(&panel, BUTTON(OCT_DOWN));
	settle(&panel);
	kit_now(k);
	CHECK(!memcmp(k + 1, kits[1] + 9, POCKET_SOUND_PARAMS - 1 - 6 * 0) || k[POCKET_PAR_COARSE1] == kits[1][8 + POCKET_PAR_COARSE1], "an old kit loads");
	CHECK(panel.kit == 4 && panel.kit_seed == 149 && panel.kit_edited, "with its name");
	CHECK(pocket_seq_tempo() == 120, "a kit alone leaves the tempo");
	/* the old pattern loads, as one page */
	panel.store_slot = 21 + 4;
	press(&panel, BUTTON(OCT_DOWN));
	CHECK(pocket_seq_pattern()->cell[3][5] == POCKET_CELL(99, 72), "an old pattern loads");
	CHECK(pocket_seq_pattern()->last_page == 0 && pocket_seq_tempo() == 140 && pocket_seq_swing() == 58 && panel.density == 70 && panel.locks == 1u << 3, "with its settings");
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_DOWN));
	CHECK(pocket_seq_tempo() == 120, "and is undone like anything else");
	press2(&panel, BUTTON(SAVE), BUTTON(OCT_UP));
	/* none of that wrote anything, and SAVE on them does not */
	press(&panel, BUTTON(SAVE));
	CHECK(!strcmp(panel.notice, "OLD SAVES ONLY LOAD"), "old rows only load: %s", panel.notice);
	panel.store_slot = 4;
	press(&panel, BUTTON(SAVE));
	CHECK(!strcmp(panel.notice, "HELD BY OLD SAVES"), "a project their sectors hold: %s", panel.notice);
	CHECK(fake_flash_ops == ops, "nothing written");

	/* a new project goes beside them, and they are untouched */
	panel.store_slot = 0;
	press(&panel, BUTTON(SAVE));
	CHECK(!strcmp(panel.notice, "SAVED 1"), "a project beside old banks: %s", panel.notice);
	CHECK(!memcmp(chip + 4096, fake_flash + 430080 + 4096, 4096) && !memcmp(chip + 8192, fake_flash + 430080 + 8192, 4096), "the old banks are byte for byte as they were");
	for (int i = 1; i < 6; i++)
	{
		pocket_panel_tick(&panel, POCKET_NOTICE_MS);
		panel.store_slot = 0;
		press(&panel, BUTTON(SAVE));
		press(&panel, BUTTON(SAVE));
		CHECK(!strncmp(panel.notice, "SAVED", 5), "again: %s", panel.notice);
		CHECK(!memcmp(chip + 4096, fake_flash + 430080 + 4096, 4096) && !memcmp(chip + 8192, fake_flash + 430080 + 8192, 4096), "and again");
	}
	press(&panel, BUTTON(HOME));
	press(&panel, BUTTON(SAVE));
	CHECK(pocket_store_old(0) && pocket_store_old(1), "still found after a reopen");

	/* erasing them is asked for, twice */
	panel.store_slot = 33;
	const long asked = fake_flash_ops;
	press(&panel, BUTTON(SAVE));
	CHECK(fake_flash_ops == asked && !strcmp(panel.notice, "ERASE OLD: SAVE AGAIN"), "one press erases nothing: %s", panel.notice);
	press(&panel, BUTTON(SAVE));
	CHECK(!strcmp(panel.notice, "OLD SAVES ERASED"), "the second does: %s", panel.notice);
	press(&panel, BUTTON(HOME));
	press(&panel, BUTTON(SAVE));
	CHECK(pocket_store_room() == POCKET_STORE_PROJECTS && !pocket_store_old(0) && !pocket_store_old(1), "then every project is free: %u", pocket_store_room());
	panel.store_slot = 8;
	press(&panel, BUTTON(SAVE));
	CHECK(!strcmp(panel.notice, "SAVED 9"), "the last one too: %s", panel.notice);
	uint8_t used = 0;
	pocket_store_peek(1, &used, 1);
	CHECK(used, "and the projects saved beside them are there");
}

/* A note of what a sound is, independent of how loud: how it decays and how bright it is. */
#define SHAPE_WINDOWS 40
struct shape { float envelope[SHAPE_WINDOWS], crossings[SHAPE_WINDOWS]; double energy; };

static void shape_of(struct shape* s, const int32_t* samples)
{
	double peak = 0;
	s->energy = 0;
	for (int w = 0; w < SHAPE_WINDOWS; w++)
	{
		double sum = 0;
		int cross = 0;
		for (int i = 0; i < 1024; i++)
		{
			const int32_t x = samples[2 * (w * 1024 + i)];
			sum += (double)x * x;
			if (i && (x < 0) != (samples[2 * (w * 1024 + i - 1)] < 0)) cross++;
		}
		s->envelope[w] = (float)sum;
		s->crossings[w] = (float)cross;
		s->energy += sum;
		if (sum > peak) peak = sum;
	}
	for (int w = 0; w < SHAPE_WINDOWS; w++) s->envelope[w] = peak > 0 ? (float)(s->envelope[w] / peak) : 0;
}

/* 0 for the same sound: differences of decay and of brightness, level left out. */
static float shape_distance(const struct shape* a, const struct shape* b)
{
	float d = 0;
	for (int w = 0; w < SHAPE_WINDOWS; w++)
	{
		const float e = a->envelope[w] - b->envelope[w];
		d += e < 0 ? -e : e;
		const float c = a->crossings[w] * a->envelope[w] - b->crossings[w] * b->envelope[w];
		d += (c < 0 ? -c : c) / 64.f;
	}
	return d;
}

#define SOUND_FRAMES (SHAPE_WINDOWS * 1024)
static int32_t sound[2 * SOUND_FRAMES];

/* Key k on a fresh engine, kit `kit` of PRESETS. */
static void render_key(struct pocket_panel* panel, int kit, int k)
{
	start_engine(panel);
	poll(panel, 0, 0, 0, 0, 0, 0, 0, NULL);
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

static void test_variations(void)
{
	struct pocket_panel panel;
	static struct shape shapes[POCKET_KEYS];
	int close = 0, silent = 0, total = 0;
	float weakest = 1e9f;
	for (int kit = 0; kit < POCKET_KITS; kit++)
	{
		for (int k = 0; k < POCKET_KEYS; k++)
		{
			render_key(&panel, kit, k);
			shape_of(&shapes[k], sound);
			silent += shapes[k].energy == 0;
		}
		/* bank against bank, position by position, and the two sounds sharing a voice */
		for (int position = 0; position < POCKET_BANK_KEYS; position++)
			for (int bank = 0; bank < 3; bank++)
				for (int other = bank + 1; other < 3; other++)
				{
					const float d = shape_distance(&shapes[position + 9 * bank], &shapes[position + 9 * other]);
					total++;
					close += d < 1.f;
					if (d < weakest) weakest = d;
				}
		for (int bank = 0; bank < 3; bank++)
		{
			total += 2;
			close += shape_distance(&shapes[5 + 9 * bank], &shapes[7 + 9 * bank]) < 1.f;
			close += shape_distance(&shapes[1 + 9 * bank], &shapes[8 + 9 * bank]) < 1.f;
		}
	}
	printf("  variations: %d of %d comparisons close, weakest %.2f, across %d kits\n", close, total, weakest, POCKET_KITS);
	CHECK(!silent, "no sound of 27 is silent on any kit: %d", silent);
	CHECK(close * 10 < total, "variations are different sounds, level aside: %d of %d too close", close, total);

	/* per hit: the kit is not touched, and the next plain hit is the plain sound */
	static uint8_t before[POCKET_SOUND_PARAMS], after[POCKET_SOUND_PARAMS];
	for (int position = 0; position < POCKET_BANK_KEYS; position++)
	{
		struct shape plain, again;
		render_key(&panel, 0, position);
		shape_of(&plain, sound);
		start_engine(&panel);
		poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
		kit_now(before);
		for (int bank = 1; bank < 3; bank++)
		{
			tap_keys(&panel, 1u << (position + 9 * bank));
			render_seconds(3 * 44100);
		}
		kit_now(after);
		CHECK(!memcmp(before, after, sizeof before), "position %d: variations leave the kit", position);
		tap_keys(&panel, 1u << position);
		for (long f = 0; f + 128 <= SOUND_FRAMES; f += 128) pocket_lxr_render_q23(sound + 2 * f, 128, 0);
		shape_of(&again, sound);
		const float d = shape_distance(&plain, &again);
		CHECK(d < 0.25f, "position %d: the next plain hit is the plain sound: %.3f", position, d);
	}
	/* the hats share a voice: a variation on one does not outlive a hit on the other */
	{
		struct shape plain, again;
		render_key(&panel, 0, 3);
		shape_of(&plain, sound);
		start_engine(&panel);
		poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
		tap_keys(&panel, 1u << (2 + 18)); /* the closed hat's B */
		render_seconds(44100);
		tap_keys(&panel, 1u << 3);
		for (long f = 0; f + 128 <= SOUND_FRAMES; f += 128) pocket_lxr_render_q23(sound + 2 * f, 128, 0);
		shape_of(&again, sound);
		CHECK(shape_distance(&plain, &again) < 0.25f, "a hat's variation ends at the other hat: %.3f", shape_distance(&plain, &again));
	}

	/* recorded: stopped and live, it is that variation, and played back it is the key's sound */
	start_engine(&panel);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
	clear_pattern();
	press(&panel, BUTTON(REC));
	tap_keys(&panel, 1u << (1 + 9));
	CHECK(pocket_seq_pattern()->cell[POCKET_TRIG_SNARE][0] == POCKET_CELL(127, POCKET_TONE_VARIATION(POCKET_VARIATION_A)), "a variation is recorded as one");
	tap_keys(&panel, 1u << 1);
	CHECK(pocket_seq_pattern()->cell[POCKET_TRIG_SNARE][0] == POCKET_CELL(127, 0), "the plain key replaces it");
	tap_keys(&panel, 1u << 1);
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
		poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
		panel.recording = 1;
		panel.follow = 0;
		tap_keys(&panel, 1u << k);
		panel.recording = 0;
		/* what recording it sounded like has gone; the step plays from the same state */
		start_engine(&panel);
		const uint32_t cell = POCKET_CELL(127, POCKET_TONE_VARIATION(k / 9 + (k % 9 >= 7 ? POCKET_VARIATION_ALT : 0)));
		static const int track[9] = { 0, 3, 5, 6, 1, 2, 4, 2, 3 };
		clear_pattern();
		pocket_seq_set_cell(track[k % 9], 0, cell);
		pocket_seq_play(1);
		for (int b = 0; b < 60; b++) pocket_lxr_render_q23(stepped + 256 * b, 128, 0);
		pocket_seq_play(0);
		CHECK(!memcmp(keyed, stepped, 256 * sizeof(int32_t) * 20), "key %d: played back it is what the key played", k);
	}

	/* PITCH is untouched by any of it */
	start_engine(&panel);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
	press(&panel, BUTTON(SCL));
	clear_pattern();
	press(&panel, BUTTON(REC));
	panel.trigger = POCKET_TRIG_DRUM2;
	tap_keys(&panel, KEY(67));
	CHECK(pocket_seq_pattern()->cell[POCKET_TRIG_DRUM2][0] == POCKET_CELL(127, 67), "PITCH records a note, on the selected voice");
	CHECK(!POCKET_CELL_VARIATION(pocket_seq_pattern()->cell[POCKET_TRIG_DRUM2][0]), "and no variation");
}

/* Long patterns by the panel's controls. */
static void test_long_patterns(void)
{
	struct pocket_panel panel;
	static uint16_t band[POCKET_SCREEN_W * POCKET_BAND_ROWS];
	struct pocket_panel_status status = { 0 };
	status.state = POCKET_STATE_READY;
	start_engine(&panel);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
	press(&panel, BUTTON(SEQ));
	clear_pattern();
	/* KNOB4 stays inside the pattern until it is longer */
	turn(&panel, POCKET_ENC_KNOB4, 15);
	CHECK(panel.step == 15, "the last step of the page");
	turn(&panel, POCKET_ENC_KNOB4, 1);
	CHECK(panel.step == 0, "16 steps: round to the first");
	poll(&panel, 0, BUTTON(SAVE), 0, 0, 0, 1, 0, NULL);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
	CHECK(POCKET_PATTERN_STEPS(pocket_seq_pattern()) == 32, "SAVE+KNOB3: a page longer");
	turn(&panel, POCKET_ENC_KNOB4, 20);
	CHECK(panel.step == 20, "the cursor reaches the second page");
	poll(&panel, 0, BUTTON(SAVE), 0, 0, 0, 2, 0, NULL);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
	CHECK(POCKET_PATTERN_STEPS(pocket_seq_pattern()) == 64, "64");
	poll(&panel, 0, BUTTON(SAVE), 0, 0, 0, 0, 2, NULL);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
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
	poll(&panel, 0, BUTTON(SAVE), 0, 3, 0, 0, 0, NULL);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
	CHECK(POCKET_CELL_CONDITION(pocket_seq_pattern()->cell[0][51]) == POCKET_CONDITION_1_3, "SAVE+KNOB1: the step's condition");
	CHECK(panel.page == POCKET_PAGE_PATTERN, "no save page");
	poll(&panel, 0, BUTTON(SAVE), 0, 99, 0, 0, 0, NULL);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
	CHECK(POCKET_CELL_CONDITION(pocket_seq_pattern()->cell[0][51]) == POCKET_CONDITION_75, "it stops at the last");
	for (unsigned b = 0; b < POCKET_BANDS; b++) pocket_panel_draw_band(&panel, &status, b, band);
	poll(&panel, 0, BUTTON(SAVE), 0, -99, 0, 0, 0, NULL);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
	CHECK(POCKET_CELL_CONDITION(pocket_seq_pattern()->cell[0][51]) == POCKET_CONDITION_ALWAYS, "and at ALWAYS");
	poll(&panel, 0, BUTTON(SAVE), 0, 5, 0, 0, 0, NULL);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
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
	poll(&panel, 0, BUTTON(SAVE), 0, 0, 0, -3, 0, NULL);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
	CHECK(POCKET_PATTERN_STEPS(pocket_seq_pattern()) == 16 && panel.step < 16, "shorter: the cursor is inside: %d", panel.step);
	CHECK(pocket_seq_pattern()->cell[1][60] != 0, "and the steps of the pages left out stay");
	/* ARP and LFO work to the length */
	poll(&panel, 0, BUTTON(SAVE), 0, 0, 0, 3, 0, NULL);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
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
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
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
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
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
	poll(&panel, 0, BUTTON(SAVE), 0, 4, 0, 0, 0, NULL);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
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
	poll(&panel, KEY_KICK | KEY_SNARE, 0, 0, 0, 0, 0, 0, NULL);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
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
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
	press(&panel, BUTTON(SEQ));
	MARK(0);
	poll(&panel, 0, BUTTON(SAVE), 0, 0, 0, 3, 0, NULL);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
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
	press(&panel, BUTTON(OCT_DOWN));
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
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
	press(&panel, BUTTON(SEQ));
	poll(&panel, 0, BUTTON(SAVE), 0, 0, 0, 3, 0, NULL);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
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
		poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
		press2(&panel, BUTTON(SAVE), others[o]);
		CHECK(panel.page != POCKET_PAGE_STORE, "SAVE and button %u opens no save page", o);
		CHECK(panel.octave == 0 && panel.key_velocity == 127, "and changes neither octave nor velocity");
		pocket_seq_play(0);
	}
	/* the order of release: the other button let go first or second, the same */
	start_engine(&panel);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
	press(&panel, BUTTON(ARP));
	poll(&panel, 0, BUTTON(SAVE), 0, 0, 0, 0, 0, NULL);
	poll(&panel, 0, BUTTON(SAVE) | BUTTON(OCT_DOWN), 0, 0, 0, 0, 0, NULL);
	poll(&panel, 0, BUTTON(OCT_DOWN), 0, 0, 0, 0, 0, NULL);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
	CHECK(panel.page == POCKET_PAGE_MAIN && !strcmp(panel.notice, "UNDO"), "SAVE let go first: still the undo: %s", panel.notice);
	CHECK(panel.key_velocity == 127, "and OCT- then does nothing of its own");
	/* a knob or a key under SAVE is no tap either */
	poll(&panel, 0, BUTTON(SAVE), 0, 0, 0, 0, 0, NULL);
	poll(&panel, 0, BUTTON(SAVE), 1, 0, 0, 0, 0, NULL);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
	CHECK(panel.page == POCKET_PAGE_MAIN, "a knob under SAVE");
	poll(&panel, 0, BUTTON(SAVE), 0, 0, 0, 0, 0, NULL);
	poll(&panel, KEY_KICK, BUTTON(SAVE), 0, 0, 0, 0, 0, NULL);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
	CHECK(panel.page == POCKET_PAGE_MAIN, "a key under SAVE");
	/* SAVE held and let go is a tap however long, and only then does anything */
	poll(&panel, 0, BUTTON(SAVE), 0, 0, 0, 0, 0, NULL);
	CHECK(panel.page == POCKET_PAGE_MAIN, "nothing on the press");
	for (int i = 0; i < 50; i++) poll(&panel, 0, BUTTON(SAVE), 0, 0, 0, 0, 0, NULL);
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
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
	poll(&fresh, 0, BUTTON(SAVE), 0, 0, 0, 0, 0, NULL);
	poll(&fresh, 0, 0, 0, 0, 0, 0, 0, NULL);
	CHECK(fresh.page == POCKET_PAGE_MAIN, "a button held at the start does not tap");
}

int main(int argc, char** argv)
{
	struct pocket_panel panel;
	int exited = 0;
	start_engine(&panel);

	/* buttons held on entry (HOME, ARP of the entry gesture) do nothing */
	poll(&panel, 0, BUTTON(HOME) | BUTTON(ARP), 0, 0, 0, 0, 0, &exited);
	CHECK(!exited, "entry gesture read as exit");
	CHECK(panel.page == POCKET_PAGE_MAIN, "starts on MAIN");
	CHECK(panel.key_mode == POCKET_KEYS_DRUM, "starts in DRUM");
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, &exited);
	CHECK(render_peak(10) == 0, "panel made sound without a hit");

	/* the programmed-pitch key triggers the selected voice (drum 1) */
	poll(&panel, KEY_KICK, 0, 0, 0, 0, 0, 0, &exited);
	CHECK(render_peak(20) > POCKET_Q23_MAX / 20, "the key did not trigger");
	poll(&panel, KEY_KICK, 0, 0, 0, 0, 0, 0, &exited); /* held: no retrigger */
	CHECK(panel.hits == 1, "a held key retriggered (%u hits)", panel.hits);
	CHECK(panel.flash_ms[POCKET_TRIG_DRUM1] == POCKET_FLASH_MS, "a hit does not light its activity cell");
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);

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
	press(&panel, BUTTON(OCT_DOWN));
	press(&panel, BUTTON(OCT_UP));
	CHECK(panel.page == POCKET_PAGE_MAIN, "OCT changed the page (%d)", panel.page);
	press(&panel, BUTTON(SEQ));
	/* sound knobs act only on their page: on PATTERN and MAIN they move no sound parameter */
	{
		const int v = pocket_lxr_get_param(POCKET_PAR_COARSE1);
		poll(&panel, 0, 0, 0, 5, 5, 5, 5, NULL);
		press(&panel, BUTTON(HOME));
		poll(&panel, 0, 0, 0, 5, 5, 5, 5, NULL);
		CHECK(pocket_lxr_get_param(POCKET_PAR_COARSE1) == v, "a knob edited outside SOUND and MIXER");
	}

	/* SOUND: the four knobs step pitch, decay, filter, drive of the selected voice, clamped */
	press(&panel, BUTTON(EDIT));
	{
		const int pitch = pocket_lxr_get_param(POCKET_PAR_COARSE1), decay = pocket_lxr_get_param(POCKET_PAR_VELOD1);
		const int filter = pocket_lxr_get_param(POCKET_PAR_FILTER_FREQ_1);
		poll(&panel, 0, 0, 0, 5, -3, -200, 300, NULL);
		CHECK(pocket_lxr_get_param(POCKET_PAR_COARSE1) == pitch + 5, "KNOB1 does not set pitch");
		CHECK(pocket_lxr_get_param(POCKET_PAR_VELOD1) == decay - 3, "KNOB2 does not set decay");
		CHECK(pocket_lxr_get_param(POCKET_PAR_FILTER_FREQ_1) == 0 && filter > 0, "KNOB3 does not clamp filter at 0");
		CHECK(pocket_lxr_get_param(POCKET_PAR_DRIVE1) == 127, "KNOB4 does not clamp drive at 127");
	}

	/* SELECT picks the voice on every page and wraps */
	pocket_lxr_load_init_kit();
	render_peak(600);
	poll(&panel, 0, 0, 3, 0, 0, 0, 0, NULL);
	CHECK(panel.trigger == POCKET_TRIG_SNARE, "SELECT picked trigger %d", panel.trigger);
	CHECK(pocket_panel_param(&panel, POCKET_CTL_DECAY) == POCKET_PAR_VELOD4, "snare decay parameter");
	poll(&panel, 0, 0, -4, 0, 0, 0, 0, NULL);
	CHECK(panel.trigger == POCKET_TRIG_OPEN_HAT, "SELECT does not wrap (%d)", panel.trigger);
	CHECK(pocket_panel_param(&panel, POCKET_CTL_DECAY) == POCKET_PAR_VELOD6_OPEN, "open hat decay parameter");
	poll(&panel, 0, 0, 1, 0, 0, 0, 0, NULL);
	panel.follow = 0;
	{
		const uint32_t hits = panel.hits;
		poll(&panel, (1u << 0) | (1u << 12) | (1u << 26), 0, 0, 0, 0, 0, 0, NULL);
		CHECK(panel.hits == hits + 3, "three keys gave %u hits", panel.hits - hits);
		poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
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
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);
	press(&panel, BUTTON(FX));
	CHECK(panel.page == POCKET_PAGE_MIXER, "FX does not open MIXER");
	poll(&panel, 0, 0, 0, -200, 0, 0, 0, NULL);
	CHECK(pocket_lxr_get_param(POCKET_PAR_VOL1) == 0, "KNOB1 does not set level");
	hit_key(&panel);
	CHECK(render_peak(20) == 0, "level 0 still sounds");
	poll(&panel, 0, 0, 0, 100, -200, 0, 0, NULL);
	CHECK(pocket_lxr_get_param(POCKET_PAR_PAN1) == 0, "KNOB2 does not set pan");
	{
		float left = 0, right = 0;
		hit_key(&panel);
		render_peak(20);
		pocket_lxr_voice_block(0, &left, &right);
		CHECK(left > right, "pan 0 is not left (%f, %f)", left, right);
		poll(&panel, 0, 0, 0, 0, 300, 0, 0, NULL);
		hit_key(&panel);
		render_peak(20);
		pocket_lxr_voice_block(0, &left, &right);
		CHECK(right > left, "pan 127 is not right (%f, %f)", left, right);
	}

	/* activity: a MIDI hit names its voice for a moment; time ends it and redraws */
	pocket_panel_init(&panel);
	panel.dirty = 0;
	CHECK(pocket_panel_midi(&panel, 9, 38, 100) == 1, "MIDI note 38 plays");
	CHECK(panel.midi_trigger == POCKET_TRIG_SNARE && panel.flash_ms[POCKET_TRIG_SNARE], "MIDI hit not shown");
	CHECK(panel.dirty & (1u << 3), "MIDI hit does not redraw the MIDI line");
	CHECK(pocket_panel_midi(&panel, 9, 99, 100) == 0 && panel.midi_trigger == POCKET_TRIG_SNARE, "an unmapped note played");
	pocket_panel_tick(&panel, 1000);
	pocket_panel_midi(&panel, 9, 38, 100);
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
		pocket_panel_midi(&panel, 9, 38, 100);
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
		worst.overruns = worst.clipped = worst.dropped = worst.midi_messages = worst.midi_hits = worst.heap_bytes = 0xffffffffu;
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
	test_old_saves();
	test_variations();
	test_long_patterns();
	test_history();
	test_save_chords();

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
	status.heap_bytes = 134124;
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
