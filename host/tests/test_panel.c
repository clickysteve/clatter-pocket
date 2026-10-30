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

#define KEY_PROGRAMMED (1u << 12)
static void hit_key(struct pocket_panel* p)
{
	poll(p, KEY_PROGRAMMED, 0, 0, 0, 0, 0, 0, NULL);
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
		for (int s = 0; s < POCKET_SEQ_STEPS; s++) hits += pocket_seq_pattern()->vel[t][s] != 0;
	CHECK(hits > 8, "a first pattern: %d hits", hits);
	CHECK(render_peak(50) == 0, "silent until PLAY");

	/* one kick on steps 1 and 9: the sound starts exactly where the clock says */
	clear_pattern();
	press(&panel, BUTTON(REC));
	CHECK(pocket_seq_pattern()->vel[0][0] == POCKET_SEQ_VEL_DEFAULT, "REC sets the selected step");
	turn(&panel, POCKET_ENC_ALGORITHM, 8);
	CHECK(panel.step == 8, "ALGORITHM moves the step");
	press(&panel, BUTTON(REC));
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
	press(&panel, BUTTON(REC));
	turn(&panel, POCKET_ENC_KNOB1, 7);
	CHECK(pocket_seq_pattern()->vel[0][0] == 127, "KNOB1 sets the velocity: %d", pocket_seq_pattern()->vel[0][0]);
	for (int t = 0; t < POCKET_SEQ_TRACKS; t++)
	{
		int32_t peak[2];
		for (int soft = 0; soft < 2; soft++)
		{
			clear_pattern();
			pocket_seq_set_step(t, 0, soft ? 30 : 127);
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
	pocket_seq_set_step(0, 0, 100);

	/* the knobs, the layer, the limits */
	turn(&panel, POCKET_ENC_KNOB2, 20);
	CHECK(pocket_seq_tempo() == 140, "KNOB2 sets the tempo: %d", pocket_seq_tempo());
	turn(&panel, POCKET_ENC_KNOB2, -9999);
	CHECK(pocket_seq_tempo() == POCKET_SEQ_BPM_MIN, "tempo floor");
	turn(&panel, POCKET_ENC_KNOB2, 90);
	turn(&panel, POCKET_ENC_KNOB3, 16);
	CHECK(pocket_seq_swing() == 66, "KNOB3 sets the swing: %d", pocket_seq_swing());
	turn(&panel, POCKET_ENC_KNOB4, -5);
	CHECK(panel.mutate_strength == POCKET_MUTATE_SMALL, "KNOB4 sets the mutation");
	press(&panel, BUTTON(SCL));
	CHECK(panel.generator_knobs, "SCL changes the knobs");
	turn(&panel, POCKET_ENC_KNOB1, 5);
	turn(&panel, POCKET_ENC_KNOB2, 9);
	turn(&panel, POCKET_ENC_KNOB3, 41);
	turn(&panel, POCKET_ENC_KNOB4, 1);
	CHECK(panel.density == 75 && panel.complexity == 75 && panel.seed == 42 && panel.style == 1, "the generator's knobs");
	CHECK(pocket_seq_tempo() == 120 && pocket_seq_swing() == 66, "not the tempo or swing");
	turn(&panel, POCKET_ENC_KNOB4, -1);
	turn(&panel, POCKET_ENC_KNOB3, -43);
	CHECK(panel.seed == 9999, "the seed wraps: %d", panel.seed);
	turn(&panel, POCKET_ENC_KNOB3, 43);

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
	press(&panel, BUTTON(ENV));
	CHECK(!memcmp(&first_pattern, pocket_seq_pattern(), sizeof expected), "ENV undoes it");
	press(&panel, BUTTON(ENV));

	/* MUTATE changes the pattern and each press differs; a locked track stays */
	panel.trigger = 0;
	press(&panel, BUTTON(SAVE));
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
	CHECK(!memcmp(base.vel[0], pocket_seq_pattern()->vel[0], POCKET_SEQ_STEPS), "MUTATE leaves a locked track");
	press(&panel, BUTTON(ARP));
	CHECK(!memcmp(base.vel[0], pocket_seq_pattern()->vel[0], POCKET_SEQ_STEPS), "GENERATE leaves a locked track");

	/* MIDI and keys sound over the sequence */
	press(&panel, BUTTON(PLAY));
	hit_key(&panel);
	render_seconds(44100);
	press(&panel, BUTTON(PLAY));
	CHECK(panel.hits == 1, "keys still play over the sequence: %u", panel.hits);
	struct pocket_lxr_stats stats;
	pocket_lxr_stats_read(&stats);
	CHECK(stats.events_dropped == 0, "nothing dropped");

	/* the longest fields, locked, both knob layers, on every page: no crash under test */
	static uint16_t band[POCKET_SCREEN_W * POCKET_BAND_ROWS];
	for (unsigned page = 0; page < POCKET_PAGES; page++)
		for (unsigned layer = 0; layer < 2; layer++)
		{
			panel.page = (uint8_t)page;
			panel.generator_knobs = (uint8_t)layer;
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

int main(int argc, char** argv)
{
	struct pocket_panel panel;
	int exited = 0;
	start_engine(&panel);

	/* buttons held on entry (HOME, ARP of the entry gesture) do nothing */
	poll(&panel, 0, BUTTON(HOME) | BUTTON(ARP), 0, 0, 0, 0, 0, &exited);
	CHECK(!exited, "entry gesture read as exit");
	CHECK(panel.page == POCKET_PAGE_MAIN, "starts on MAIN");
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, &exited);
	CHECK(render_peak(10) == 0, "panel made sound without a hit");

	/* the programmed-pitch key triggers the selected voice (drum 1) */
	poll(&panel, KEY_PROGRAMMED, 0, 0, 0, 0, 0, 0, &exited);
	CHECK(render_peak(20) > POCKET_Q23_MAX / 20, "the key did not trigger");
	poll(&panel, KEY_PROGRAMMED, 0, 0, 0, 0, 0, 0, &exited); /* held: no retrigger */
	CHECK(panel.hits == 1, "a held key retriggered (%u hits)", panel.hits);
	CHECK(panel.flash_ms[POCKET_TRIG_DRUM1] == POCKET_FLASH_MS, "a hit does not light its activity cell");
	poll(&panel, 0, 0, 0, 0, 0, 0, 0, NULL);

	/* navigation: direct buttons, OCT-/OCT+ wrapping, HOME back to MAIN */
	static const struct { uint32_t button; int page; } direct[] = {
		{ BUTTON(SEQ), POCKET_PAGE_PATTERN }, { BUTTON(EDIT), POCKET_PAGE_SOUND },
		{ BUTTON(FX), POCKET_PAGE_MIXER }, { BUTTON(GLO), POCKET_PAGE_SYSTEM }, { BUTTON(HOME), POCKET_PAGE_MAIN }
	};
	for (unsigned i = 0; i < sizeof direct / sizeof direct[0]; i++)
	{
		panel.dirty = 0;
		press(&panel, direct[i].button);
		CHECK(panel.page == direct[i].page, "button %u opened page %d", i, panel.page);
		CHECK(panel.dirty == (1u << POCKET_BANDS) - 1u, "a page change does not redraw the whole screen");
	}
	press(&panel, BUTTON(OCT_DOWN));
	CHECK(panel.page == POCKET_PAGE_SYSTEM, "OCT- does not wrap from MAIN (%d)", panel.page);
	press(&panel, BUTTON(OCT_UP));
	CHECK(panel.page == POCKET_PAGE_MAIN, "OCT+ does not wrap from SYSTEM (%d)", panel.page);
	press(&panel, BUTTON(OCT_UP));
	CHECK(panel.page == POCKET_PAGE_PATTERN, "OCT+ does not step");
	/* the pattern buttons act on PATTERN only: elsewhere they cannot lose a pattern */
	{
		press(&panel, BUTTON(EDIT));
		struct pocket_panel before = panel;
		const struct pocket_pattern pattern = *pocket_seq_pattern();
		press(&panel, BUTTON(SAVE));
		press(&panel, BUTTON(REC));
		press(&panel, BUTTON(ARP));
		press(&panel, BUTTON(SCL));
		press(&panel, BUTTON(ENV));
		press(&panel, BUTTON(LFO));
		CHECK(panel.page == before.page && panel.locks == before.locks && panel.seed == before.seed
			&& panel.generator_knobs == before.generator_knobs, "a pattern button acted off PATTERN");
		CHECK(!memcmp(&pattern, pocket_seq_pattern(), sizeof pattern), "a pattern changed off PATTERN");
		press(&panel, BUTTON(SEQ));
	}

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

	/* SELECT picks the voice on every page and wraps; keys play it chromatically */
	pocket_lxr_load_init_kit();
	render_peak(600);
	poll(&panel, 0, 0, 3, 0, 0, 0, 0, NULL);
	CHECK(panel.trigger == POCKET_TRIG_SNARE, "SELECT picked trigger %d", panel.trigger);
	CHECK(pocket_panel_param(&panel, POCKET_CTL_DECAY) == POCKET_PAR_VELOD4, "snare decay parameter");
	poll(&panel, 0, 0, -4, 0, 0, 0, 0, NULL);
	CHECK(panel.trigger == POCKET_TRIG_OPEN_HAT, "SELECT does not wrap (%d)", panel.trigger);
	CHECK(pocket_panel_param(&panel, POCKET_CTL_DECAY) == POCKET_PAR_VELOD6_OPEN, "open hat decay parameter");
	poll(&panel, 0, 0, 1, 0, 0, 0, 0, NULL);
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
	pocket_panel_midi_hit(&panel, POCKET_TRIG_SNARE, 38);
	CHECK(panel.midi_trigger == POCKET_TRIG_SNARE && panel.flash_ms[POCKET_TRIG_SNARE], "MIDI hit not shown");
	CHECK(panel.dirty & (1u << 3), "MIDI hit does not redraw the MIDI line");
	pocket_panel_midi_hit(&panel, 99, 38);
	CHECK(panel.midi_trigger == POCKET_TRIG_SNARE, "invalid voice accepted");
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
		pocket_panel_midi_hit(&panel, POCKET_TRIG_SNARE, 38);
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
