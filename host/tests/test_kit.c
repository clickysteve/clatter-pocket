/*
 * test_kit.c - host tests of kit design (seq/pocket_kit.c): the rules a generated kit must keep,
 * and every factory kit and hundreds of generated ones rendered through the real LXR engine:
 * each voice audible, in its register, ending, and not clipping.
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * test_kit --print CHARACTER SEED WILD   prints a kit's parameters, a number per line: a check
 * against the firmware for tools/emu-test.mjs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pocket_lxr.h"
#include "pocket_params.h"
#include "pocket_kit.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define PARAMS POCKET_SOUND_PARAMS
#define RATE 44100

static void init_kit(uint8_t* kit)
{
	for (int p = 0; p < PARAMS; p++) kit[p] = (uint8_t)pocket_lxr_init_kit_value(p);
	for (int v = 0; v < POCKET_LXR_VOICES; v++) kit[POCKET_PAR_VOLUME_MOD_ON_OFF1 + v] = 1;
}

/* A kit in the engine, by the path the panel uses. */
static void load(const uint8_t* kit)
{
	static int32_t scratch[2 * 128];
	pocket_lxr_init(1);
	for (int p = 1; p < PARAMS; p++)
	{
		pocket_lxr_set_param(p, kit[p]);
		if (p % 24 == 0) pocket_lxr_render_q23(scratch, 64, 0);
	}
	pocket_lxr_render_q23(scratch, 128, 0);
}

struct heard {
	int32_t peak;
	long crossings_ms;   /* zero crossings per 100 ms over the first 100 ms: a register, roughly */
	long tail;           /* milliseconds until it stays under -60 dB, of 4000 */
	uint32_t clipped;
};

static int32_t rendered[2 * 2 * RATE];

static struct heard hear(int trigger)
{
	struct heard h = { 0, 0, 0, 0 };
	pocket_lxr_trigger(trigger, 127, 60);
	const long frames = 2 * RATE;
	for (long f = 0; f + 128 <= frames; f += 128) pocket_lxr_render_q23(rendered + 2 * f, 128, 0);
	long last = 0;
	for (long f = 0; f < frames; f++)
	{
		const int32_t a = rendered[2 * f] < 0 ? -rendered[2 * f] : rendered[2 * f];
		if (a > h.peak) h.peak = a;
		if (a > POCKET_Q23_MAX / 1000) last = f;
	}
	for (long f = 1; f < RATE / 10; f++) h.crossings_ms += (rendered[2 * (f - 1)] < 0) != (rendered[2 * f] < 0);
	h.tail = last * 1000 / RATE;
	struct pocket_lxr_stats s;
	pocket_lxr_stats_read(&s);
	h.clipped = s.clipped_samples;
	return h;
}

static void test_rules(void)
{
	static uint8_t a[PARAMS], b[PARAMS];
	/* every value in 0..127, deterministic, every kit different */
	long bad = 0, same = 0, unchanged = 0;
	for (unsigned c = 0; c < POCKET_KIT_CHARACTERS; c++)
		for (uint32_t seed = 0; seed < 400; seed++)
			for (unsigned w = 0; w <= 100; w += 25)
			{
				init_kit(a);
				init_kit(b);
				pocket_kit_generate(a, c, seed, w);
				pocket_kit_generate(b, c, seed, w);
				unchanged += !!memcmp(a, b, sizeof a);
				for (int p = 0; p < PARAMS; p++) bad += a[p] > 127;
				init_kit(b);
				pocket_kit_generate(b, c, seed + 1, w);
				same += !memcmp(a, b, sizeof a);
				bad += a[POCKET_PAR_VELOD1] > 90 || a[POCKET_PAR_VELOD5] > 90 || a[POCKET_PAR_VELOD6_OPEN] > 90;
				for (int v = 0; v < POCKET_LXR_VOICES; v++) bad += a[POCKET_PAR_VOLUME_MOD_ON_OFF1 + v] != 1;
			}
	CHECK(!bad, "%ld out-of-range parameters", bad);
	CHECK(!unchanged, "%ld kits were not deterministic", unchanged);
	CHECK(!same, "%ld seeds gave the same kit", same);
	/* a fixed kit: the same on the firmware as here */
	init_kit(a);
	pocket_kit_generate(a, 3, 1234, 50);
	uint32_t h = 2166136261u;
	for (int p = 0; p < PARAMS; p++) h = (h ^ a[p]) * 16777619u;
	printf("  kit hash %08x\n", h);

	/* the kick stays a kick at any WILDNESS; the rest are allowed to wander */
	int kick_low = 0, kits = 0;
	for (uint32_t seed = 0; seed < 2000; seed++)
	{
		init_kit(a);
		pocket_kit_generate(a, 15, seed, 100);
		kick_low += a[POCKET_PAR_COARSE1] <= 48;
		kits++;
	}
	CHECK(kick_low == kits, "a kick left the low end: %d of %d", kick_low, kits);

	/* one voice changes one voice; all of its parameters are its own */
	for (unsigned voice = 0; voice < POCKET_LXR_VOICES; voice++)
	{
		init_kit(a);
		pocket_kit_generate(a, 1, 77, 50);
		memcpy(b, a, sizeof a);
		pocket_kit_voice(b, 1, 77, 50, voice, 3);
		static const uint8_t decays[POCKET_LXR_VOICES] = { POCKET_PAR_VELOD1, POCKET_PAR_VELOD2, POCKET_PAR_VELOD3, POCKET_PAR_VELOD4, POCKET_PAR_VELOD5, POCKET_PAR_VELOD6_CLOSED };
		static const uint8_t coarses[POCKET_LXR_VOICES] = { POCKET_PAR_COARSE1, POCKET_PAR_COARSE2, POCKET_PAR_COARSE3, POCKET_PAR_COARSE4, POCKET_PAR_COARSE5, POCKET_PAR_COARSE6 };
		int changed = 0, others = 0;
		for (int p = 0; p < PARAMS; p++) changed += a[p] != b[p];
		for (unsigned o = 0; o < POCKET_LXR_VOICES; o++)
			if (o != voice) others += a[decays[o]] != b[decays[o]] || a[coarses[o]] != b[coarses[o]];
		CHECK(changed >= 5, "voice %u barely changed: %d", voice, changed);
		CHECK(!others, "voice %u changed another voice", voice);
	}

	/* the kit's family: voices of one seed share a root, kits differ */
	int on_scale = 0, drums = 0;
	for (uint32_t seed = 1; seed < 300; seed++)
	{
		init_kit(a);
		pocket_kit_generate(a, 0, seed, 30);
		const int roots[3] = { a[POCKET_PAR_COARSE1], a[POCKET_PAR_COARSE2], a[POCKET_PAR_COARSE3] };
		/* some root has all three on its minor pentatonic */
		static const int pentatonic[12] = { 1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0 };
		int any = 0;
		for (int r = 0; r < 12; r++)
		{
			int all = 1;
			for (int d = 0; d < 3; d++) all &= pentatonic[(roots[d] + 120 - r) % 12];
			any |= all;
		}
		on_scale += any;
		drums++;
	}
	CHECK(on_scale == drums, "the drums of a kit share a scale: %d of %d", on_scale, drums);
}

/* The rendered tests are slow: the factory kits fully, and a sweep at each extreme. */
static void test_rendered(void)
{
	static uint8_t kit[PARAMS];
	long quiet = 0, clipping = 0, endless = 0, voices = 0;
	long sum[POCKET_LXR_TRIGGERS] = { 0 };
	long kick_low = 0, hat_high = 0, kits = 0;
	for (unsigned c = 0; c < POCKET_KIT_CHARACTERS; c++)
		for (uint32_t seed = 0; seed < 12; seed++)
		{
			const unsigned wild = seed < 4 ? 50 : seed < 8 ? 0 : 100;
			init_kit(kit);
			pocket_kit_generate(kit, c, seed * 37u, wild);
			load(kit);
			struct heard h[POCKET_LXR_TRIGGERS];
			for (int t = 0; t < POCKET_LXR_TRIGGERS; t++)
			{
				h[t] = hear(t);
				voices++;
				sum[t] += h[t].crossings_ms;
				if (h[t].peak < POCKET_Q23_MAX / 200) { quiet++; printf("  quiet: %s seed %u wild %u trigger %d peak %d\n", pocket_kit_name(c), seed * 37u, wild, t, h[t].peak); }
				clipping += h[t].clipped > 0;
				if (h[t].tail >= 1990 && t != POCKET_TRIG_CYMBAL && t != POCKET_TRIG_OPEN_HAT) { endless++; printf("  long: %s seed %u wild %u trigger %d\n", pocket_kit_name(c), seed * 37u, wild, t); }
			}
			kits++;
			if (wild <= 50)
			{
				kick_low += h[POCKET_TRIG_DRUM1].crossings_ms < 100 * 2 / 2 * 3;
				hat_high += h[POCKET_TRIG_CLOSED_HAT].crossings_ms > h[POCKET_TRIG_DRUM1].crossings_ms;
			}
		}
	CHECK(quiet * 100 <= voices, "%ld of %ld voices were nearly silent", quiet, voices);
	CHECK(!clipping, "%ld voices clipped alone", clipping);
	CHECK(endless * 50 <= voices, "%ld of %ld short voices rang on past 2 s", endless, voices);
	const long tamed = kits * 2 / 3;
	CHECK(kick_low >= tamed * 9 / 10, "kicks are low: %ld of %ld", kick_low, tamed);
	CHECK(hat_high >= tamed * 9 / 10, "hats are over kicks: %ld of %ld", hat_high, tamed);
	CHECK(sum[POCKET_TRIG_CLOSED_HAT] > sum[POCKET_TRIG_DRUM2] && sum[POCKET_TRIG_SNARE] > sum[POCKET_TRIG_DRUM1], "the roles keep their registers");
	printf("  mean crossings per 100 ms:");
	for (int t = 0; t < POCKET_LXR_TRIGGERS; t++) printf(" %ld", sum[t] / kits);
	printf("\n");
}

int main(int argc, char** argv)
{
	if (argc > 4 && !strcmp(argv[1], "--print"))
	{
		static uint8_t kit[PARAMS];
		init_kit(kit);
		pocket_kit_generate(kit, (unsigned)atoi(argv[2]), (uint32_t)atoi(argv[3]), (unsigned)atoi(argv[4]));
		for (int p = 0; p < PARAMS; p++) printf("%d\n", kit[p]);
		return 0;
	}
	test_rules();
	test_rendered();
	printf("kits: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
