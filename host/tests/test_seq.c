/*
 * test_seq.c - host tests of the step sequencer and the pattern generator (seq/), the first
 * with no engine: the clock is checked against exact arithmetic, sample by sample.
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * test_seq --print SEED DENSITY COMPLEXITY STYLE   prints a generated pattern, one track per
 * line, 0 or 1 per step: tools/emu-test.mjs compares it with the emulated firmware's screen.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pocket_seq.h"
#include "pocket_gen.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define RATE 44118u
#define BLOCK 64u
#define MAX_EVENTS 200000
/* FNV-1a of the velocities of seed 12345, density 50, complexity 30, MACHINE */
#ifndef GOLDEN
#define GOLDEN 0xa118fffau
#endif

/* the voices: what was triggered, when */
static struct { uint64_t at; int track, velocity, note; } events[MAX_EVENTS];
static int event_count;
static uint64_t now;

static void record(int track, int velocity, int note)
{
	if (event_count < MAX_EVENTS)
	{
		events[event_count].at = now;
		events[event_count].track = track;
		events[event_count].velocity = velocity;
		events[event_count].note = note;
		event_count++;
	}
}

static void reset(unsigned rate)
{
	pocket_seq_init(rate, BLOCK, record);
	event_count = 0;
	now = 0;
}

static void run_blocks(uint64_t blocks)
{
	while (blocks--)
	{
		pocket_seq_block();
		now += BLOCK;
	}
}

static void run_until_event(int count)
{
	for (uint64_t guard = 0; event_count < count && guard < 100000000; guard++)
	{
		pocket_seq_block();
		now += BLOCK;
	}
}

/* exact time of step n after start in samples, as a fraction over (20 * bpm) */
static uint64_t exact_units(uint64_t n, unsigned rate, unsigned swing)
{
	const uint64_t pairs = n / 2;
	return pairs * 600u * rate + (n & 1 ? 6u * (uint64_t)rate * swing : 0);
}

static void test_step_timing(void)
{
	const unsigned tempos[] = { 30, 60, 87, 120, 133, 174, 240, 300 };
	const unsigned swings[] = { 50, 54, 62, 66, 75 };
	const unsigned rates[] = { 44118, 44100 };
	for (unsigned r = 0; r < 2; r++)
		for (unsigned t = 0; t < 8; t++)
			for (unsigned s = 0; s < 5; s++)
			{
				const unsigned rate = rates[r], bpm = tempos[t], swing = swings[s];
				reset(rate);
				pocket_seq_set_tempo((int)bpm);
				pocket_seq_set_swing((int)swing);
				pocket_seq_set_step(0, 0, 100, 0);
				for (int i = 0; i < POCKET_SEQ_STEPS; i++) pocket_seq_set_step(0, i, 100, 0);
				pocket_seq_play(1);
				const int want = bpm >= 120 ? 8000 : 2000;
				run_until_event(want);
				int worst = 0, bad = 0;
				for (int n = 0; n < want; n++)
				{
					/* fires at the first block boundary at or after the exact time: never early,
					   at most a block late, however long it runs */
					const uint64_t units = exact_units((uint64_t)n, rate, swing);
					const uint64_t at = events[n].at * 20u * bpm;
					if (at < units || at >= units + (uint64_t)BLOCK * 20u * bpm) bad++;
					const int late = (int)((at - units) / (20u * bpm));
					if (late > worst) worst = late;
				}
				CHECK(!bad, "%u BPM swing %u at %u: %d of %d steps outside one block", bpm, swing, rate, bad, want);
				CHECK(worst < (int)BLOCK, "worst lateness %d samples", worst);
				CHECK(pocket_seq_steps_played() == (uint32_t)want, "one step per event");
			}
}

static void test_tempo_is_exact_over_a_long_run(void)
{
	/* an hour at 137 BPM: exactly the right number of steps, no drift */
	reset(RATE);
	pocket_seq_set_tempo(137);
	pocket_seq_play(1);
	const uint64_t samples = (uint64_t)RATE * 3600u, blocks = samples / BLOCK;
	run_blocks(blocks);
	/* step n (from 0) fires when 15 * rate * n <= bpm * elapsed; the last block ends at
	   blocks * BLOCK, and its boundary was blocks - 1 */
	const uint64_t reached = (uint64_t)137 * (blocks - 1) * BLOCK / (15u * RATE) + 1;
	CHECK(pocket_seq_steps_played() == reached, "%u steps in an hour, expected %llu", pocket_seq_steps_played(), (unsigned long long)reached);
}

static void test_swing(void)
{
	reset(RATE);
	pocket_seq_set_tempo(120);
	pocket_seq_set_swing(66);
	for (int i = 0; i < POCKET_SEQ_STEPS; i++) pocket_seq_set_step(0, i, 100, 0);
	pocket_seq_play(1);
	run_until_event(1001);
	const double straight = RATE * 15.0 / 120;
	double first = 0, second = 0;
	for (int n = 0; n < 1000; n += 2)
	{
		first += (double)(events[n + 1].at - events[n].at);
		second += (double)(events[n + 2].at - events[n + 1].at);
	}
	first /= 500;
	second /= 500;
	CHECK(first > straight * 2 * 0.66 - 1 && first < straight * 2 * 0.66 + 1, "first of a pair %.1f", first);
	CHECK(second > straight * 2 * 0.34 - 1 && second < straight * 2 * 0.34 + 1, "second of a pair %.1f", second);
	CHECK(first + second > straight * 2 - 1 && first + second < straight * 2 + 1, "a pair stays two steps");

	/* a swing change mid-pair waits for the next pair: every pair still adds up */
	reset(RATE);
	for (int i = 0; i < POCKET_SEQ_STEPS; i++) pocket_seq_set_step(0, i, 100, 0);
	pocket_seq_set_swing(75);
	pocket_seq_play(1);
	run_until_event(2);
	pocket_seq_set_swing(50);
	run_until_event(5);
	const uint64_t pair = (uint64_t)(RATE * 15u * 2u / 120u);
	CHECK(events[2].at + BLOCK > pair && events[2].at < pair + BLOCK, "pair length kept across a swing change");
	CHECK(events[3].at - events[2].at < events[1].at - events[0].at, "the new swing from the next pair");
}

static void test_tempo_change(void)
{
	/* a change moves no position: the next step arrives by the time already covered */
	reset(RATE);
	for (int i = 0; i < POCKET_SEQ_STEPS; i++) pocket_seq_set_step(0, i, 100, 0);
	pocket_seq_set_tempo(60);
	pocket_seq_play(1);
	run_until_event(5);
	const uint64_t slow = events[4].at - events[3].at;
	const uint64_t half = slow / 2 / BLOCK;
	run_blocks(half);
	pocket_seq_set_tempo(240);
	run_until_event(8);
	const uint64_t bridge = events[5].at - events[4].at, fast = events[7].at - events[6].at;
	CHECK(fast >= slow / 4 - BLOCK && fast <= slow / 4 + BLOCK, "the new tempo: %llu", (unsigned long long)fast);
	CHECK(bridge >= slow * 5 / 8 - BLOCK && bridge <= slow * 5 / 8 + 2 * BLOCK, "half at the old tempo, half at the new: %llu", (unsigned long long)bridge);
	/* every tempo in the range, swung hardest: never two steps in a block, none skipped */
	for (int bpm = POCKET_SEQ_BPM_MIN; bpm <= POCKET_SEQ_BPM_MAX; bpm += 13)
	{
		reset(RATE);
		for (int i = 0; i < POCKET_SEQ_STEPS; i++) pocket_seq_set_step(0, i, 100, 0);
		pocket_seq_set_tempo(bpm);
		pocket_seq_set_swing(POCKET_SEQ_SWING_MAX);
		pocket_seq_play(1);
		run_until_event(200);
		int doubled = 0;
		for (int n = 1; n < 200; n++) doubled += events[n].at == events[n - 1].at;
		CHECK(!doubled, "%d BPM: %d doubled", bpm, doubled);
		CHECK(pocket_seq_steps_played() == 200, "%d BPM: every step played once", bpm);
	}
}

static void test_loop_and_positions(void)
{
	reset(RATE);
	pocket_seq_set_step(3, 0, 90, 0);
	pocket_seq_set_step(3, 15, 40, 0);
	pocket_seq_play(1);
	CHECK(pocket_seq_position() == POCKET_SEQ_NOT_PLAYING, "nothing moves before a block");
	run_until_event(6);
	CHECK(event_count == 6, "two hits a loop for three loops");
	for (int i = 0; i < 6; i++)
	{
		CHECK(events[i].track == 3, "the track");
		CHECK(events[i].velocity == (i & 1 ? 40 : 90), "the velocity at %d", i);
	}
	CHECK(pocket_seq_steps_played() == 2 * 16 + 16, "16 steps a loop: %u", pocket_seq_steps_played());
	CHECK(pocket_seq_position() == 15, "at the last step");
	CHECK(pocket_seq_last_tracks() == 1u << 3, "the last step's tracks");
	CHECK(pocket_seq_triggers() == 6, "triggers counted");
}

static void test_transport(void)
{
	reset(RATE);
	pocket_seq_set_step(0, 0, 100, 0);
	pocket_seq_set_step(0, 8, 100, 0);
	run_blocks(1000);
	CHECK(event_count == 0, "stopped: silence");
	pocket_seq_play(1);
	run_blocks(1);
	CHECK(event_count == 1 && events[0].at == 1000u * BLOCK, "step 0 in the first block after PLAY");
	CHECK(pocket_seq_position() == 0, "at step 0");
	run_blocks(1500);
	const int heard = event_count;
	pocket_seq_play(0);
	run_blocks(1);
	CHECK(pocket_seq_position() == POCKET_SEQ_NOT_PLAYING, "stopped in the next block");
	run_blocks(5000);
	CHECK(event_count == heard, "no step after STOP");
	pocket_seq_play(1);
	run_blocks(1);
	CHECK(event_count == heard + 1 && pocket_seq_position() == 0, "restarts from step 0");
}

static void test_simultaneous(void)
{
	reset(RATE);
	for (int t = 0; t < POCKET_SEQ_TRACKS; t++) pocket_seq_set_step(t, 0, 100 + t, 0);
	pocket_seq_play(1);
	run_blocks(1);
	/* six triggers: the two hats are one voice, and the open hat takes it */
	CHECK(event_count == 6, "%d triggers", event_count);
	unsigned tracks = 0;
	for (int i = 0; i < event_count; i++)
	{
		CHECK(events[i].at == events[0].at, "all in one block");
		CHECK(events[i].velocity == 100 + events[i].track, "each at its velocity");
		tracks |= 1u << events[i].track;
	}
	CHECK(tracks == 0x5f, "all but the closed hat: %x", tracks);
	reset(RATE);
	pocket_seq_set_step(5, 0, 100, 0);
	pocket_seq_play(1);
	run_blocks(1);
	CHECK(event_count == 1 && events[0].track == 5, "a closed hat alone plays");
}

static void test_live_editing(void)
{
	reset(RATE);
	pocket_seq_set_step(0, 0, 100, 0);
	pocket_seq_play(1);
	run_blocks(1);
	/* an edit ahead of the playhead is heard this time round */
	pocket_seq_set_step(1, 4, 77, 0);
	run_until_event(2);
	CHECK(events[1].track == 1 && events[1].velocity == 77, "an edit ahead of the playhead sounds");
	/* one behind it waits for the next loop */
	pocket_seq_set_step(2, 0, 55, 0);
	run_until_event(4);
	CHECK(events[2].track == 0 && events[3].track == 2 && events[3].at == events[2].at, "an edit behind it sounds next loop");
	/* clearing a step ahead silences it */
	pocket_seq_set_step(1, 4, 0, 0);
	const int before = event_count;
	run_blocks(4000 / 2);
	for (int i = before; i < event_count; i++) CHECK(events[i].track != 1, "a cleared step stays quiet");

	/* a whole pattern changes between steps, keeps the position, and is never seen half made */
	reset(RATE);
	for (int t = 0; t < POCKET_SEQ_TRACKS; t++)
		for (int i = 0; i < POCKET_SEQ_STEPS; i++) pocket_seq_set_step(t, i, 10, 0);
	pocket_seq_play(1);
	run_blocks(500);
	const int position = pocket_seq_position();
	struct pocket_pattern* next = pocket_seq_edit_begin();
	for (int t = 0; t < POCKET_SEQ_TRACKS; t++)
	{
		for (int i = 0; i < POCKET_SEQ_STEPS; i++) next->cell[t][i] = 20;
		run_blocks(50); /* audio runs through the edit */
	}
	for (int i = event_count - 1; i >= 0; i--) CHECK(events[i].velocity == 10, "nothing from an uncommitted pattern");
	CHECK(pocket_seq_position() != position, "playback continued");
	pocket_seq_edit_commit();
	const int edge = event_count;
	run_blocks(1000);
	CHECK(event_count > edge, "plays on");
	for (int i = edge; i < event_count; i++) CHECK(events[i].velocity == 20, "the whole new pattern");

	/* undo returns the replaced pattern, and undo again returns */
	CHECK(POCKET_CELL_VELOCITY(pocket_seq_pattern()->cell[0][0]) == 20, "the new pattern shows");
	pocket_seq_undo();
	CHECK(POCKET_CELL_VELOCITY(pocket_seq_pattern()->cell[0][0]) == 10, "undo");
	pocket_seq_undo();
	CHECK(POCKET_CELL_VELOCITY(pocket_seq_pattern()->cell[0][0]) == 20, "redo");
}

static void test_boundaries(void)
{
	reset(RATE);
	pocket_seq_set_tempo(0);
	CHECK(pocket_seq_tempo() == POCKET_SEQ_BPM_MIN, "tempo floor");
	pocket_seq_set_tempo(100000);
	CHECK(pocket_seq_tempo() == POCKET_SEQ_BPM_MAX, "tempo ceiling");
	pocket_seq_set_swing(10);
	CHECK(pocket_seq_swing() == POCKET_SEQ_SWING_MIN, "swing floor");
	pocket_seq_set_swing(99);
	CHECK(pocket_seq_swing() == POCKET_SEQ_SWING_MAX, "swing ceiling");
	struct pocket_pattern before = *pocket_seq_pattern();
	pocket_seq_set_step(-1, 0, 100, 0);
	pocket_seq_set_step(POCKET_SEQ_TRACKS, 0, 100, 0);
	pocket_seq_set_step(0, -1, 100, 0);
	pocket_seq_set_step(0, POCKET_SEQ_STEPS, 100, 0);
	CHECK(!memcmp(&before, pocket_seq_pattern(), sizeof before), "out-of-range edits ignored");
	pocket_seq_set_step(0, 0, 500, 0);
	CHECK(POCKET_CELL_VELOCITY(pocket_seq_pattern()->cell[0][0]) == 127, "velocity ceiling");
	pocket_seq_set_step(0, 0, -5, 0);
	CHECK(POCKET_CELL_VELOCITY(pocket_seq_pattern()->cell[0][0]) == 0, "velocity floor");
	/* the fastest possible steps are far longer than a block */
	const uint32_t fastest = 6u * 44100u * (100u - POCKET_SEQ_SWING_MAX), perBlock = 20u * BLOCK * POCKET_SEQ_BPM_MAX;
	CHECK(fastest > perBlock, "a step can never fall inside one block");
	/* the phase cannot overflow: below 2^32 at the widest interval */
	CHECK((uint64_t)6u * 48000u * POCKET_SEQ_SWING_MAX + perBlock < (uint64_t)1 << 32, "phase range");
}

/* ------------------------------------------------------------------ generation */
static int hits_of(const struct pocket_pattern* p, int t)
{
	int n = 0;
	for (int i = 0; i < POCKET_SEQ_STEPS; i++) n += POCKET_CELL_VELOCITY(p->cell[t][i]) != 0;
	return n;
}

static int total_hits(const struct pocket_pattern* p)
{
	int n = 0;
	for (int t = 0; t < POCKET_SEQ_TRACKS; t++) n += hits_of(p, t);
	return n;
}

static int differences(const struct pocket_pattern* a, const struct pocket_pattern* b)
{
	int n = 0;
	for (int t = 0; t < POCKET_SEQ_TRACKS; t++)
		for (int i = 0; i < POCKET_SEQ_STEPS; i++) n += !POCKET_CELL_VELOCITY(a->cell[t][i]) != !POCKET_CELL_VELOCITY(b->cell[t][i]);
	return n;
}

static struct pocket_pattern generated(uint32_t seed, int density, int complexity, int style, int locks)
{
	struct pocket_pattern p;
	memset(&p, 0, sizeof p);
	struct pocket_gen_settings s = { seed, (uint8_t)density, (uint8_t)complexity, (uint8_t)style, (uint8_t)locks };
	pocket_gen_generate(&p, &s);
	return p;
}

static void test_deterministic_seeds(void)
{
	for (uint32_t seed = 0; seed < 50; seed++)
	{
		struct pocket_pattern a = generated(seed, 50, 30, 0, 0), b = generated(seed, 50, 30, 0, 0);
		CHECK(!memcmp(&a, &b, sizeof a), "seed %u reproduces", seed);
	}
	int same = 0;
	struct pocket_pattern first = generated(1, 50, 30, 1, 0);
	for (uint32_t seed = 2; seed < 50; seed++)
	{
		struct pocket_pattern p = generated(seed, 50, 30, 1, 0);
		same += !memcmp(&first, &p, sizeof p);
	}
	CHECK(same == 0, "other seeds, other patterns: %d the same", same);
	/* fixed across platforms and builds: the firmware must generate exactly this */
	struct pocket_pattern p = generated(12345, 50, 30, 0, 0);
	uint32_t h = 2166136261u;
	for (int t = 0; t < POCKET_SEQ_TRACKS; t++)
		for (int i = 0; i < POCKET_SEQ_STEPS; i++) h = (h ^ POCKET_CELL_VELOCITY(p.cell[t][i])) * 16777619u;
	CHECK(h == GOLDEN, "the pattern of seed 12345 changed: %08x", h);
}

static void test_generation_every_setting(void)
{
	/* every style across its range: in range, no empty bar, the hats' choke holds */
	for (int style = 0; style < POCKET_GEN_STYLES; style++)
		for (int density = 0; density <= 100; density += 10)
			for (int complexity = 0; complexity <= 100; complexity += 10)
				for (uint32_t seed = 1; seed <= 8; seed++)
				{
					struct pocket_pattern p = generated(seed, density, complexity, style, 0);
					int bad = 0, choked = 0;
					for (int t = 0; t < POCKET_SEQ_TRACKS; t++)
						for (int i = 0; i < POCKET_SEQ_STEPS; i++) bad += POCKET_CELL_VELOCITY(p.cell[t][i]) > 127;
					for (int i = 0; i < POCKET_SEQ_STEPS; i++) choked += POCKET_CELL_VELOCITY(p.cell[5][i]) && POCKET_CELL_VELOCITY(p.cell[6][i]);
					checks++;
					if (bad || choked || !total_hits(&p))
					{
						failures++;
						printf("FAIL style %d density %d complexity %d seed %u: %d bad, %d choked, %d hits\n", style, density, complexity, seed, bad, choked, total_hits(&p));
					}
				}
	CHECK(!strcmp(pocket_gen_style_name(0), "MACHINE"), "the first style");
	CHECK(!strcmp(pocket_gen_style_name(99), "MACHINE"), "out of range falls back");
}

/* The roles at ordinary settings: this is what makes it a drum pattern and not noise. */
static void test_roles(void)
{
	int kick_downbeat = 0, backbeat = 0, four_floor = 0, hats = 0, count = 0;
	double offbeats_low = 0, offbeats_high = 0;
	for (uint32_t seed = 1; seed <= 200; seed++)
	{
		struct pocket_pattern p = generated(seed, 50, 30, 0, 0);
		kick_downbeat += POCKET_CELL_VELOCITY(p.cell[0][0]) != 0;
		backbeat += POCKET_CELL_VELOCITY(p.cell[3][4]) && POCKET_CELL_VELOCITY(p.cell[3][12]);
		four_floor += POCKET_CELL_VELOCITY(p.cell[0][0]) && POCKET_CELL_VELOCITY(p.cell[0][4]) && POCKET_CELL_VELOCITY(p.cell[0][8]) && POCKET_CELL_VELOCITY(p.cell[0][12]);
		hats += hits_of(&p, 5) >= 4;
		count++;
		struct pocket_pattern low = generated(seed, 50, 10, 5, 0), high = generated(seed, 50, 90, 5, 0);
		int on = 0, off = 0;
		for (int t = 0; t < POCKET_SEQ_TRACKS; t++)
			for (int i = 0; i < POCKET_SEQ_STEPS; i++)
			{
				if (POCKET_CELL_VELOCITY(low.cell[t][i])) { if (i & 1) off++; else on++; }
			}
		offbeats_low += (double)off / (on + off);
		on = off = 0;
		for (int t = 0; t < POCKET_SEQ_TRACKS; t++)
			for (int i = 0; i < POCKET_SEQ_STEPS; i++)
			{
				if (POCKET_CELL_VELOCITY(high.cell[t][i])) { if (i & 1) off++; else on++; }
			}
		offbeats_high += (double)off / (on + off);
	}
	CHECK(kick_downbeat == count, "the kick anchors step 0: %d of %d", kick_downbeat, count);
	CHECK(backbeat == count, "MACHINE keeps the backbeat: %d of %d", backbeat, count);
	CHECK(four_floor == count, "MACHINE's four on the floor: %d of %d", four_floor, count);
	CHECK(hats >= count * 9 / 10, "hats subdivide: %d of %d", hats, count);
	CHECK(offbeats_high > offbeats_low * 1.2, "COMPLEXITY moves weight off the beat: %.2f to %.2f", offbeats_low / 200, offbeats_high / 200);
	/* velocities carry the accents: strong steps louder than weak ones */
	long strong = 0, weak = 0, nstrong = 0, nweak = 0;
	for (uint32_t seed = 1; seed <= 200; seed++)
	{
		struct pocket_pattern p = generated(seed, 60, 30, 1, 0);
		for (int i = 0; i < POCKET_SEQ_STEPS; i++)
		{
			if (!POCKET_CELL_VELOCITY(p.cell[5][i])) continue;
			if (i % 4 == 0) { strong += POCKET_CELL_VELOCITY(p.cell[5][i]); nstrong++; }
			if (i & 1) { weak += POCKET_CELL_VELOCITY(p.cell[5][i]); nweak++; }
		}
	}
	CHECK(nstrong && nweak && strong * nweak > weak * nstrong, "beats are louder than off-beats");
}

static void test_density(void)
{
	for (int style = 0; style < 8; style++)
	{
		int low = 0, mid = 0, high = 0;
		for (uint32_t seed = 1; seed <= 100; seed++)
		{
			struct pocket_pattern a = generated(seed, 10, 30, style, 0), b = generated(seed, 50, 30, style, 0), c = generated(seed, 95, 30, style, 0);
			low += total_hits(&a);
			mid += total_hits(&b);
			high += total_hits(&c);
		}
		CHECK(low * 4 < mid * 3 && mid * 4 < high * 3, "style %d: density scales %d %d %d", style, low, mid, high);
	}
	/* more density adds to a rhythm rather than replacing it (keyed decisions) */
	int kept = 0, was = 0;
	for (uint32_t seed = 1; seed <= 100; seed++)
	{
		struct pocket_pattern a = generated(seed, 40, 30, 3, 0), b = generated(seed, 60, 30, 3, 0);
		for (int t = 0; t < POCKET_SEQ_TRACKS; t++)
			for (int i = 0; i < POCKET_SEQ_STEPS; i++)
			{
				was += POCKET_CELL_VELOCITY(a.cell[t][i]) != 0;
				kept += POCKET_CELL_VELOCITY(a.cell[t][i]) && POCKET_CELL_VELOCITY(b.cell[t][i]);
			}
	}
	CHECK(kept * 100 > was * 90, "a denser pattern keeps its hits: %d of %d", kept, was);
}

static void test_experimental(void)
{
	/* steps that no ordinary pattern of the style uses, and the anchor let go */
	int unanchored = 0, patterns_differ = 0;
	for (uint32_t seed = 1; seed <= 200; seed++)
	{
		struct pocket_pattern calm = generated(seed, 50, 20, 0, 0), wild = generated(seed, 50, 100, 0, 0);
		unanchored += !POCKET_CELL_VELOCITY(wild.cell[0][0]);
		patterns_differ += differences(&calm, &wild) >= 8;
	}
	CHECK(unanchored > 0 || patterns_differ >= 180, "high complexity lets go: %d %d", unanchored, patterns_differ);
	CHECK(patterns_differ >= 180, "high complexity is another rhythm: %d of 200", patterns_differ);
}

static void test_locks(void)
{
	struct pocket_pattern p = generated(7, 50, 50, 0, 0), before = p;
	struct pocket_gen_settings s = { 8, 90, 90, 1, (1u << 0) | (1u << 3) };
	pocket_gen_generate(&p, &s);
	CHECK(!memcmp(p.cell[0], before.cell[0], sizeof p.cell[0]), "a locked kick survives GENERATE");
	CHECK(!memcmp(p.cell[3], before.cell[3], sizeof p.cell[3]), "a locked snare survives GENERATE");
	CHECK(differences(&p, &before) > 0, "the rest changed");
	before = p;
	for (uint32_t seed = 0; seed < 100; seed++) pocket_gen_mutate(&p, seed, POCKET_MUTATE_LARGE, (1u << 0) | (1u << 3));
	CHECK(!memcmp(p.cell[0], before.cell[0], sizeof p.cell[0]), "a locked kick survives MUTATE");
	CHECK(!memcmp(p.cell[3], before.cell[3], sizeof p.cell[3]), "a locked snare survives MUTATE");
	CHECK(pocket_gen_mutate(&p, 1, POCKET_MUTATE_LARGE, 0x7f) == 0, "all locked: nothing to do");
}

static void test_mutation(void)
{
	long changed[POCKET_MUTATE_STRENGTHS] = { 0 };
	int anchors_kept = 0, count = 0, unchanged = 0;
	for (uint32_t seed = 1; seed <= 300; seed++)
	{
		const struct pocket_pattern base = generated(seed, 50, 30, 0, 0);
		for (int s = 0; s < POCKET_MUTATE_STRENGTHS; s++)
		{
			struct pocket_pattern p = base, q = base;
			const unsigned made = pocket_gen_mutate(&p, seed * 7u, (unsigned)s, 0);
			pocket_gen_mutate(&q, seed * 7u, (unsigned)s, 0);
			CHECK(!memcmp(&p, &q, sizeof p), "mutation is deterministic");
			changed[s] += differences(&base, &p);
			if (s == POCKET_MUTATE_SMALL)
			{
				count++;
				unchanged += !memcmp(&base, &p, sizeof p) && made == 0;
				anchors_kept += POCKET_CELL_VELOCITY(p.cell[0][0]) && POCKET_CELL_VELOCITY(p.cell[3][4]) && POCKET_CELL_VELOCITY(p.cell[3][12]);
				CHECK(differences(&base, &p) <= 6, "small stays small: %d", differences(&base, &p));
			}
			int bad = 0;
			for (int t = 0; t < POCKET_SEQ_TRACKS; t++)
				for (int i = 0; i < POCKET_SEQ_STEPS; i++) bad += POCKET_CELL_VELOCITY(p.cell[t][i]) > 127;
			CHECK(!bad, "velocities in range");
		}
	}
	CHECK(changed[0] < changed[1] && changed[1] < changed[2], "strengths ordered: %ld %ld %ld", changed[0], changed[1], changed[2]);
	CHECK(anchors_kept * 100 >= count * 90, "small keeps the anchors: %d of %d", anchors_kept, count);
	CHECK(unchanged * 100 <= count * 5, "a mutation nearly always changes something: %d of %d", unchanged, count);
	/* a mutated pattern stays related; a new seed does not */
	long related = 0, unrelated = 0;
	for (uint32_t seed = 1; seed <= 100; seed++)
	{
		const struct pocket_pattern base = generated(seed, 50, 30, 1, 0), other = generated(seed + 1000, 50, 30, 1, 0);
		struct pocket_pattern p = base;
		pocket_gen_mutate(&p, seed, POCKET_MUTATE_MEDIUM, 0);
		related += differences(&base, &p);
		unrelated += differences(&base, &other);
	}
	CHECK(related < unrelated, "mutation is closer than generation: %ld vs %ld", related, unrelated);
	/* an empty pattern and a full one do not trip it (under AddressSanitizer) */
	struct pocket_pattern empty, full;
	memset(&empty, 0, sizeof empty);
	memset(&full, 100, sizeof full);
	for (uint32_t seed = 0; seed < 200; seed++)
	{
		pocket_gen_mutate(&empty, seed, seed % 3, 0);
		pocket_gen_mutate(&full, seed, seed % 3, 0);
	}
	checks++;
}

/* Steps carry notes: played, kept apart from the programmed pitch, moved with their hits. */
static void test_notes(void)
{
	reset(RATE);
	pocket_seq_set_step(1, 0, 90, 67);
	pocket_seq_set_step(2, 0, 80, 0);
	pocket_seq_play(1);
	run_blocks(1);
	CHECK(event_count == 2 && events[0].note == 67 && events[0].velocity == 90, "a step plays its note: %d", events[0].note);
	CHECK(events[1].note == POCKET_SEQ_NOTE_PROGRAMMED, "no note: the programmed pitch: %d", events[1].note);
	CHECK(POCKET_CELL_NOTE(pocket_seq_pattern()->cell[1][0]) == 67, "the note is stored");
	pocket_seq_set_step(1, 0, 0, 67);
	CHECK(pocket_seq_pattern()->cell[1][0] == 0, "a step that is off keeps no note");
	pocket_seq_set_step(1, 0, 90, 300);
	CHECK(POCKET_CELL_NOTE(pocket_seq_pattern()->cell[1][0]) == 0, "a note out of range is no note");

	/* every kind of mutation carries a note with its hit: no hit changes note unless OP_PITCH did,
	   and no note is ever left on a step that is off */
	long stray = 0, pitch_at_low = 0, moved = 0, kept = 0;
	for (uint32_t seed = 1; seed <= 400; seed++)
	{
		struct pocket_pattern p = generated(seed, 60, 40, 0, 0);
		const int pitched = (seed & 3u) == 0;
		for (int i = 0; pitched && i < POCKET_SEQ_STEPS; i++)
			if (p.cell[1][i]) p.cell[1][i] = POCKET_CELL(POCKET_CELL_VELOCITY(p.cell[1][i]), 72);
		for (int s = 0; s < POCKET_MUTATE_STRENGTHS; s++)
		{
			struct pocket_pattern q = p;
			pocket_gen_mutate(&q, seed * 13u, (unsigned)s, 0);
			for (int t = 0; t < POCKET_SEQ_TRACKS; t++)
				for (int i = 0; i < POCKET_SEQ_STEPS; i++)
				{
					const unsigned n = POCKET_CELL_NOTE(q.cell[t][i]);
					stray += n && !POCKET_CELL_VELOCITY(q.cell[t][i]);
					if (s < POCKET_MUTATE_LARGE && !pitched) pitch_at_low += n != 0;
					if (pitched && t == 1 && n) { kept += n == 72; moved += n != 72; }
				}
		}
	}
	CHECK(!stray, "a note left on an empty step: %ld", stray);
	CHECK(!pitch_at_low, "LOW and MED gave programmed-pitch tracks notes: %ld", pitch_at_low);
	CHECK(moved > 0 && kept > moved * 4, "notes move now and then, and mostly stay: %ld moved, %ld kept", moved, kept);
	struct pocket_pattern big = generated(3, 70, 40, 0, 0);
	int notes = 0;
	for (uint32_t seed = 0; seed < 60; seed++) pocket_gen_mutate(&big, seed, POCKET_MUTATE_LARGE, 0);
	for (int t = 0; t < POCKET_SEQ_TRACKS; t++)
		for (int i = 0; i < POCKET_SEQ_STEPS; i++) notes += POCKET_CELL_NOTE(big.cell[t][i]) != 0;
	CHECK(notes > 0, "BIG may give a rhythm notes");
}

/* Where a live hit lands, and that it is not heard twice. */
static void test_quantise(void)
{
	const uint32_t step_blocks = RATE * 15u / 120u / BLOCK; /* a step at 120 BPM, in blocks */
	reset(RATE);
	int ahead = -1;
	CHECK(pocket_seq_quantise(1, &ahead) == 0 && ahead, "before the first step: step 0, ahead");
	pocket_seq_play(1);
	run_blocks(1);
	/* just after step 0, and just before step 1 */
	run_blocks(step_blocks / 2u - 2u);
	CHECK(pocket_seq_quantise(1, &ahead) == 0 && !ahead, "the first half of a step belongs to it");
	run_blocks(4);
	CHECK(pocket_seq_quantise(1, &ahead) == 1 && ahead, "the second half to the next");
	CHECK(pocket_seq_quantise(0, &ahead) == 0 && !ahead, "BEHIND stays on the step sounding");
	/* every position of a whole loop, with swing, against what the clock does next */
	reset(RATE);
	pocket_seq_set_swing(66);
	pocket_seq_play(1);
	run_blocks(1);
	int wrong = 0, late = 0;
	for (uint32_t b = 0; b < 2500u; b++)
	{
		const int at = pocket_seq_position(), to = pocket_seq_quantise(1, &ahead);
		if (ahead ? to != ((at + 1) & 15) : to != at) wrong++;
		late += ahead;
		run_blocks(1);
	}
	CHECK(!wrong, "quantised steps are the playhead or the next: %d wrong", wrong);
	CHECK(late > 800 && late < 1700, "about half of all times round up: %d of 2500", late);
	/* a wrap: the last step's later half goes to step 0 */
	reset(RATE);
	pocket_seq_play(1);
	run_blocks(1);
	while (pocket_seq_position() != 15) run_blocks(1);
	run_blocks(step_blocks * 3u / 4u);
	CHECK(pocket_seq_quantise(1, &ahead) == 0 && ahead, "the end of the bar rounds to step 0");

	/* the skip: once, on that step, and it cannot wait for a later one */
	reset(RATE);
	for (int i = 0; i < POCKET_SEQ_STEPS; i++) pocket_seq_set_step(0, i, 100, 0);
	pocket_seq_set_step(1, 2, 100, 0);
	pocket_seq_skip(0, 2);
	pocket_seq_skip(1, 2);
	pocket_seq_play(1);
	run_blocks(step_blocks * 6u);
	int on2 = 0, seen = 0;
	for (int i = 0; i < event_count; i++)
	{
		on2 += events[i].track == 1;
		seen += events[i].track == 0;
	}
	CHECK(on2 == 1, "a skip is forgotten when another step plays: %d", on2);
	CHECK(seen >= 5, "the track keeps playing around it: %d", seen);
	reset(RATE);
	for (int i = 0; i < POCKET_SEQ_STEPS; i++) pocket_seq_set_step(0, i, 100, 0);
	pocket_seq_play(1);
	run_blocks(1);
	pocket_seq_skip(0, 1);
	const int before = event_count;
	run_blocks(step_blocks + 2u);
	CHECK(event_count == before, "the skipped step is silent: %d", event_count - before);
	run_blocks(step_blocks + 2u);
	CHECK(event_count == before + 1, "and only that one: %d", event_count - before);
}

/* One track of a pattern, and only that one. */
static void test_one_track(void)
{
	int empty = 0, same = 0, unchanged = 0, others = 0;
	for (uint32_t seed = 1; seed <= 200; seed++)
		for (unsigned style = 0; style < POCKET_GEN_STYLES; style++)
			for (unsigned t = 0; t < POCKET_SEQ_TRACKS; t++)
			{
				struct pocket_pattern p = generated(seed, 50, 30, (int)style, 0), a = p, b = p;
				const struct pocket_gen_settings s = { seed, 50, 30, (uint8_t)style, 0 };
				a.cell[t][3] = POCKET_CELL(100, 70); /* a recorded note goes with the old rhythm */
				const struct pocket_pattern c = a;
				pocket_gen_track(&a, &s, t, 0);
				pocket_gen_track(&b, &s, t, 1);
				int hits = 0;
				for (int i = 0; i < POCKET_SEQ_STEPS; i++)
				{
					hits += a.cell[t][i] != 0;
					others += POCKET_CELL_NOTE(a.cell[t][i]) != 0;
				}
				empty += !hits;
				same += !memcmp(a.cell[t], b.cell[t], sizeof a.cell[t]);
				int rhythm = 0;
				for (int i = 0; i < POCKET_SEQ_STEPS; i++) rhythm += !a.cell[t][i] != !c.cell[t][i];
				unchanged += !rhythm;
				for (unsigned o = 0; o < POCKET_SEQ_TRACKS; o++)
					if (o != t) others += memcmp(a.cell[o], p.cell[o], sizeof a.cell[o]) != 0;
			}
	CHECK(!empty, "a one-track pattern came back empty: %d", empty);
	CHECK(!others, "a one-track pattern touched another track: %d", others);
	CHECK(!unchanged, "a new rhythm was the old one: %d of 15400", unchanged);
	CHECK(same < 3000, "another take is usually another rhythm: %d of 15400 the same", same);
	struct pocket_pattern x, y;
	memset(&x, 0, sizeof x);
	memset(&y, 0, sizeof y);
	const struct pocket_gen_settings s = { 9, 50, 30, 2, 0 };
	pocket_gen_track(&x, &s, 3, 4);
	pocket_gen_track(&y, &s, 3, 4);
	CHECK(!memcmp(&x, &y, sizeof x), "a one-track pattern is deterministic");
}

static void print_pattern(int argc, char** argv)
{
	if (argc < 6) exit(2);
	struct pocket_pattern p = generated((uint32_t)atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), atoi(argv[5]), 0);
	for (int t = 0; t < POCKET_SEQ_TRACKS; t++)
	{
		for (int i = 0; i < POCKET_SEQ_STEPS; i++) putchar(POCKET_CELL_VELOCITY(p.cell[t][i]) ? '1' : '0');
		putchar('\n');
	}
}

int main(int argc, char** argv)
{
	if (argc > 1 && !strcmp(argv[1], "--print"))
	{
		print_pattern(argc, argv);
		return 0;
	}
	test_step_timing();
	test_tempo_is_exact_over_a_long_run();
	test_swing();
	test_tempo_change();
	test_loop_and_positions();
	test_transport();
	test_simultaneous();
	test_live_editing();
	test_boundaries();
	test_deterministic_seeds();
	test_generation_every_setting();
	test_roles();
	test_density();
	test_experimental();
	test_locks();
	test_mutation();
	test_notes();
	test_quantise();
	test_one_track();
	printf("sequencer: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
