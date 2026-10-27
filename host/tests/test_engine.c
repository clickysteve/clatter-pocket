/*
 * test_engine.c - host tests of CLATTER Pocket's LXR engine host (engine/pocket_lxr.c).
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Everything here runs the real LXR-02 DSP (lxr/upstream/), natively compiled, through the
 * same entry points the FM-1 firmware calls, in the FM-1's block size (128 frames). These are
 * host tests: they say nothing about timing on the device.
 */
#include <stdio.h>
#include <string.h>
#include "pocket_lxr.h"
#include "pocket_params.h"
#include "wav.h"

#define SECONDS(s) ((size_t)((s) * HOST_SAMPLE_RATE) / HOST_BLOCK_FRAMES * HOST_BLOCK_FRAMES)

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int32_t buf[2][SECONDS(12) * 2];

/* Render `frames` into b in FM-1 blocks. */
static void render(int32_t* b, size_t frames)
{
	for (size_t f = 0; f < frames; f += HOST_BLOCK_FRAMES) pocket_lxr_render_q23(b + 2 * f, HOST_BLOCK_FRAMES, 0);
}

/* Init, set kick parameters, trigger drum 1, render one second. */
static void kick(int32_t* b, int pitch, int decay, int level)
{
	pocket_lxr_init(1);
	if (pitch >= 0) pocket_lxr_set_param(POCKET_PAR_COARSE1, pitch);
	if (decay >= 0) pocket_lxr_set_param(POCKET_PAR_VELOD1, decay);
	if (level >= 0) pocket_lxr_set_param(POCKET_PAR_VOL1, level);
	pocket_lxr_trigger(POCKET_TRIG_DRUM1, 127, 60);
	render(b, SECONDS(1));
}

static void test_silence(void)
{
	pocket_lxr_init(1);
	render(buf[0], SECONDS(1));
	CHECK(peak_left(buf[0], 0, SECONDS(1)) == 0, "engine is not silent before a trigger");
}

static void test_kick_sounds(void)
{
	kick(buf[0], -1, -1, -1);
	const int32_t peak = peak_left(buf[0], 0, SECONDS(1));
	CHECK(peak > POCKET_Q23_MAX / 20, "init-kit kick is too quiet: peak %d", peak);
	/* sound starts in the first engine block after the trigger */
	CHECK(peak_left(buf[0], 0, POCKET_LXR_BLOCK) > 0, "kick does not start in the first block");
	printf("  init kick: peak %.1f dBFS, %.0f ms above -40 dB\n",
	       20.0 * __builtin_log10((double)peak / POCKET_Q23_MAX),
	       1000.0 * last_above(buf[0], SECONDS(1), peak / 100) / HOST_SAMPLE_RATE);
}

static void test_pitch(void)
{
	/* frequency once the pitch envelope has settled; COARSE is bipolar around 63 */
	kick(buf[0], 40, 100, -1);
	kick(buf[1], 80, 100, -1);
	const double lo = zero_cross_hz(buf[0], SECONDS(0.15), SECONDS(0.4));
	const double hi = zero_cross_hz(buf[1], SECONDS(0.15), SECONDS(0.4));
	printf("  pitch: COARSE 40 -> %.1f Hz, COARSE 80 -> %.1f Hz (ratio %.2f, 40 semitones = %.2f)\n",
	       lo, hi, hi / lo, __builtin_pow(2.0, 40.0 / 12.0));
	CHECK(lo > 0 && hi > lo * 4, "COARSE does not raise the pitch (%.1f -> %.1f Hz)", lo, hi);
	/* the same voice, 12 semitones up by note number */
	pocket_lxr_init(1);
	pocket_lxr_set_param(POCKET_PAR_COARSE1, 40);
	pocket_lxr_set_param(POCKET_PAR_VELOD1, 100);
	pocket_lxr_trigger(POCKET_TRIG_DRUM1, 127, 72);
	render(buf[1], SECONDS(1));
	const double oct = zero_cross_hz(buf[1], SECONDS(0.15), SECONDS(0.4));
	CHECK(oct > lo * 1.9 && oct < lo * 2.1, "note +12 is not an octave (%.1f -> %.1f Hz)", lo, oct);
}

static void test_decay(void)
{
	kick(buf[0], -1, 30, -1);
	kick(buf[1], -1, 45, -1);
	const size_t a = last_above(buf[0], SECONDS(1), POCKET_Q23_MAX / 100);
	const size_t b = last_above(buf[1], SECONDS(1), POCKET_Q23_MAX / 100);
	CHECK(b < SECONDS(1) - 1, "decay test window too short");
	printf("  decay: VELOD 30 -> %.0f ms, VELOD 45 -> %.0f ms above -40 dBFS\n",
	       1000.0 * a / HOST_SAMPLE_RATE, 1000.0 * b / HOST_SAMPLE_RATE);
	CHECK(b > a * 3 / 2, "longer decay is not longer (%zu -> %zu frames)", a, b);
}

static void test_level(void)
{
	kick(buf[0], -1, -1, 127);
	kick(buf[1], -1, -1, 40);
	const int32_t a = peak_left(buf[0], 0, SECONDS(1)), b = peak_left(buf[1], 0, SECONDS(1));
	printf("  level: VOL 127 -> %d, VOL 40 -> %d\n", a, b);
	CHECK(b > 0 && b < a / 2, "VOL does not lower the level (%d -> %d)", a, b);
}

static void test_determinism(void)
{
	for (int run = 0; run < 2; run++)
	{
		pocket_lxr_init(1234);
		for (int t = 0; t < POCKET_LXR_TRIGGERS; t++) pocket_lxr_trigger(t, 100, 60);
		render(buf[run], SECONDS(1));
	}
	CHECK(memcmp(buf[0], buf[1], sizeof(int32_t) * 2 * SECONDS(1)) == 0, "same seed renders differently");
	pocket_lxr_init(99);
	for (int t = 0; t < POCKET_LXR_TRIGGERS; t++) pocket_lxr_trigger(t, 100, 60);
	render(buf[1], SECONDS(1));
	CHECK(memcmp(buf[0], buf[1], sizeof(int32_t) * 2 * SECONDS(1)) != 0, "the seed does not reach the noise");
}

static void test_every_trigger(void)
{
	static const char* names[] = { "drum 1", "drum 2", "drum 3", "snare", "cymbal", "cl hat", "op hat" };
	for (int t = 0; t < POCKET_LXR_TRIGGERS; t++)
	{
		pocket_lxr_init(7);
		pocket_lxr_trigger(t, 127, 60);
		render(buf[0], SECONDS(0.5));
		const int32_t peak = peak_left(buf[0], 0, SECONDS(0.5));
		const int voice = t >= 5 ? 5 : t;
		int others = 0;
		for (int v = 0; v < POCKET_LXR_VOICES; v++)
		{
			const int16_t* d = pocket_lxr_voice_block(v, 0, 0);
			for (int i = 0; i < POCKET_LXR_BLOCK; i++) if (v != voice && d[i]) others++;
		}
		CHECK(peak > 0, "%s is silent", names[t]);
		CHECK(others == 0, "%s sounds on another voice", names[t]);
	}
}

static void test_queue(void)
{
	pocket_lxr_init(1);
	int refused = 0;
	for (int i = 0; i < POCKET_LXR_QUEUE + 10; i++)
		if (pocket_lxr_set_param(POCKET_PAR_VELOD1, i & 127)) refused++;
	CHECK(refused == 10, "full ring accepted %d too many events", 10 - refused);
	struct pocket_lxr_stats s;
	pocket_lxr_stats_read(&s);
	CHECK(s.events_dropped == 10, "dropped events not counted (%u)", s.events_dropped);
	/* drained at most POCKET_LXR_DRAIN_MAX per engine block */
	pocket_lxr_render_q23(buf[0], POCKET_LXR_BLOCK, 0);
	pocket_lxr_stats_read(&s);
	CHECK(s.events_applied == POCKET_LXR_DRAIN_MAX, "one block applied %u events", s.events_applied);
	CHECK(s.backlog_max == POCKET_LXR_QUEUE, "backlog %u", s.backlog_max);
	render(buf[0], SECONDS(0.2));
	pocket_lxr_stats_read(&s);
	CHECK(s.events_applied == POCKET_LXR_QUEUE, "ring did not drain (%u)", s.events_applied);
	CHECK(pocket_lxr_get_param(POCKET_PAR_VELOD1) == ((POCKET_LXR_QUEUE - 1) & 127), "shadow value wrong");
	/* bad arguments are refused without touching the ring */
	CHECK(pocket_lxr_set_param(0, 1) == -1 && pocket_lxr_set_param(POCKET_SOUND_PARAMS, 1) == -1, "bad param accepted");
	CHECK(pocket_lxr_trigger(-1, 1, 60) == -1 && pocket_lxr_trigger(POCKET_LXR_TRIGGERS, 1, 60) == -1, "bad trigger accepted");
	/* a kit reload goes through the ring too, spread over blocks */
	CHECK(pocket_lxr_load_init_kit() == 0, "init kit reload refused on an empty ring");
}

static void test_clipping(void)
{
	pocket_lxr_init(5);
	pocket_lxr_set_master(4 * POCKET_GAIN_UNITY);
	for (int t = 0; t < 5; t++) pocket_lxr_trigger(t, 127, 60);
	render(buf[0], SECONDS(0.5));
	struct pocket_lxr_stats s;
	pocket_lxr_stats_read(&s);
	int out_of_range = 0;
	for (size_t i = 0; i < SECONDS(0.5) * 2; i++)
		if (buf[0][i] > POCKET_Q23_MAX || buf[0][i] < POCKET_Q23_MIN) out_of_range++;
	printf("  +12 dB master, five voices: %u samples saturated, peak %u\n", s.clipped_samples, s.peak_q23);
	CHECK(s.clipped_samples > 0, "overdriven mix did not clip");
	CHECK(out_of_range == 0, "output left the Q23 range");
	CHECK(s.peak_q23 == POCKET_Q23_MAX || s.peak_q23 == (uint32_t)POCKET_Q23_MAX + 1, "peak %u", s.peak_q23);
}

static void test_accumulate(void)
{
	kick(buf[0], -1, -1, -1);
	pocket_lxr_init(1);
	pocket_lxr_trigger(POCKET_TRIG_DRUM1, 127, 60);
	for (size_t i = 0; i < SECONDS(1) * 2; i++) buf[1][i] = 1000;
	for (size_t f = 0; f < SECONDS(1); f += HOST_BLOCK_FRAMES)
		pocket_lxr_render_q23(buf[1] + 2 * f, HOST_BLOCK_FRAMES, 1);
	int wrong = 0;
	for (size_t i = 0; i < SECONDS(1) * 2; i++)
		if (buf[1][i] != buf[0][i] + 1000) wrong++;
	CHECK(wrong == 0, "accumulate does not add (%d samples differ)", wrong);
}

static void test_locks(void)
{
	/* a decay lock lasts one hit; the next plain hit returns to the kit's decay */
	const size_t n = SECONDS(3);
	pocket_lxr_init(1);
	pocket_lxr_set_param(POCKET_PAR_VELOD1, 40);
	pocket_lxr_trigger(POCKET_TRIG_DRUM1, 127, 60);
	render(buf[0], n);
	const size_t reference = last_above(buf[0], n, POCKET_Q23_MAX / 100);
	pocket_lxr_trigger_locked(POCKET_TRIG_DRUM1, 127, 60, POCKET_PAR_VELOD1, 100, 0, 0);
	render(buf[0], n);
	const size_t locked = last_above(buf[0], n, POCKET_Q23_MAX / 100);
	pocket_lxr_trigger(POCKET_TRIG_DRUM1, 127, 60);
	render(buf[1], n);
	const size_t plain = last_above(buf[1], n, POCKET_Q23_MAX / 100);
	printf("  lock: VELOD 40 %.0f ms, hit locked to 100 %.0f ms, next plain hit %.0f ms\n",
	       1000.0 * reference / HOST_SAMPLE_RATE, 1000.0 * locked / HOST_SAMPLE_RATE,
	       1000.0 * plain / HOST_SAMPLE_RATE);
	CHECK(locked > reference * 3 / 2, "lock did not apply (%zu vs %zu)", locked, reference);
	CHECK(plain == reference, "lock did not return (%zu vs %zu)", plain, reference);
	CHECK(pocket_lxr_get_param(POCKET_PAR_VELOD1) == 40, "lock changed the kit");
}

static void test_stability(void)
{
	/* ten seconds of dense random hits and parameter changes, then ten of silence: the
	   engine must return to silence (no runaway filter, no stuck envelope) */
	pocket_lxr_init(42);
	uint32_t r = 12345;
	for (size_t f = 0; f < SECONDS(10); f += HOST_BLOCK_FRAMES)
	{
		r = r * 1664525u + 1013904223u;
		if ((r >> 28) < 6) pocket_lxr_trigger((int)((r >> 8) % POCKET_LXR_TRIGGERS), 1 + (int)((r >> 16) & 126), 36 + (int)((r >> 4) % 48));
		r = r * 1664525u + 1013904223u;
		const int p = 1 + (int)((r >> 8) % (POCKET_SOUND_PARAMS - 1));
		/* FX and global settings are not voice parameters; skip them */
		if (p < POCKET_PAR_FX_TYPE) pocket_lxr_set_param(p, (int)((r >> 20) & 127));
		pocket_lxr_render_q23(buf[0] + 2 * f, HOST_BLOCK_FRAMES, 0);
	}
	struct pocket_lxr_stats s;
	pocket_lxr_stats_read(&s);
	CHECK(s.events_dropped == 0, "events dropped under load (%u)", s.events_dropped);
	/* Random settings legitimately sustain: an amp decay of 127 never ends and 126 lasts
	   minutes (test_long_decays), a decimator near rate 0 holds a DC level
	   (test_decimator_hold). What must not happen is hidden state running away (NaN or
	   infinity in a filter or envelope). Reload the init kit, which has finite decays, and
	   the engine must return to silence. */
	CHECK(pocket_lxr_load_init_kit() == 0, "init kit reload refused");
	pocket_lxr_set_dc_block(1);
	render(buf[0], SECONDS(10));
	const int32_t tail = peak_left(buf[0], SECONDS(9), SECONDS(10));
	printf("  stability: after 10 s of random abuse and an init-kit reload, peak in the 10th second %d\n", tail);
	CHECK(tail < POCKET_Q23_MAX / 10000, "engine did not return to silence (%d)", tail);
	/* and it still plays normally */
	pocket_lxr_trigger(POCKET_TRIG_DRUM1, 127, 60);
	render(buf[0], SECONDS(1));
	CHECK(peak_left(buf[0], 0, SECONDS(1)) > POCKET_Q23_MAX / 20, "engine no longer plays after abuse");
}

static void test_long_decays(void)
{
	/* firmware behaviour: amp decay 127 sustains indefinitely */
	pocket_lxr_init(1);
	pocket_lxr_set_param(POCKET_PAR_VELOD1, 127);
	pocket_lxr_trigger(POCKET_TRIG_DRUM1, 127, 60);
	render(buf[0], SECONDS(12));
	const int32_t first = peak_left(buf[0], 0, SECONDS(1)), last = peak_left(buf[0], SECONDS(11), SECONDS(12));
	CHECK(last > first * 9 / 10, "decay 127 does not sustain (%d -> %d)", first, last);
}

static void test_decimator_hold(void)
{
	/* firmware behaviour: VOICE_DECIMATION 0 freezes the voice's sample-and-hold */
	pocket_lxr_init(1);
	pocket_lxr_trigger(POCKET_TRIG_DRUM1, 127, 60);
	render(buf[0], SECONDS(0.01));
	pocket_lxr_set_param(POCKET_PAR_VOICE_DECIMATION1, 0);
	render(buf[0], SECONDS(1));
	const int32_t held = buf[0][2 * SECONDS(0.5)];
	CHECK(held != 0 && buf[0][2 * SECONDS(1) - 2] == held, "decimation 0 does not hold");
	/* the DC blocker removes the held level */
	pocket_lxr_set_dc_block(1);
	render(buf[0], SECONDS(2));
	CHECK(peak_left(buf[0], SECONDS(1.5), SECONDS(2)) < POCKET_Q23_MAX / 10000, "held level not blocked");
	/* and with the blocker on, a kick still sounds at the same peak (within 5 %) */
	kick(buf[0], -1, -1, -1);
	const int32_t plain = peak_left(buf[0], 0, SECONDS(1));
	pocket_lxr_init(1);
	pocket_lxr_set_dc_block(1);
	pocket_lxr_trigger(POCKET_TRIG_DRUM1, 127, 60);
	render(buf[1], SECONDS(1));
	const int32_t blocked = peak_left(buf[1], 0, SECONDS(1));
	printf("  DC blocker: kick peak %d -> %d\n", plain, blocked);
	CHECK(blocked > plain * 95 / 100 && blocked < plain * 105 / 100, "DC blocker changes the kick");
}

int main(void)
{
#ifdef POCKET_NOFX
	printf("engine tests (FX bus off, as the FM-1 build)\n");
#else
	printf("engine tests (full LXR engine, as CLATTER)\n");
#endif
	test_silence();
	test_kick_sounds();
	test_pitch();
	test_decay();
	test_level();
	test_determinism();
	test_every_trigger();
	test_queue();
	test_clipping();
	test_accumulate();
	test_locks();
	test_stability();
	test_long_decays();
	test_decimator_hold();
	printf("%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
