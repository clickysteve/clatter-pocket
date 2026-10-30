/*
 * pocket_gen.c - pattern generation and mutation. See pocket_gen.h and
 * docs/POCKET-SEQUENCER.md.
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 * Ported from CLATTER's src/rhythm/generate.ts and mutate.ts; mutation is adapted there from
 * DOWNBEAT's EXTENDED pattern mutation (src/extended/mutate.ts, MIT licence, see
 * THIRD_PARTY_NOTICES.md).
 *
 * Generation is CLATTER's: each role has a lane of chances per step from its style, and the
 * chances are shaped by where the step falls in the bar rather than drawn independently:
 *
 *   DENSITY     an exponent on every chance, so certain steps stay and weak ones come and go
 *   COMPLEXITY  moves weight off the beat (pickups before strong beats, ghost notes, weaker
 *               written steps loosened) and raises the chance of a fill
 *
 * Then rules across the tracks: the kick is the anchor (it keeps step 0), hats choke, the kick
 * clears a strong backbeat, toms do not sit on the kick. What this port adds is for having one
 * bar instead of CLATTER's phrases: above 60 % COMPLEXITY supporting tracks are displaced by a
 * seeded rotation, above 80 % the closed hats become a Euclidean cross-rhythm and the kick
 * loses its anchor. Every chance is 0..65535, every strength and metric weight 0..256.
 */
#include "pocket_gen.h"

enum { KICK, LOW_TOM, HIGH_TOM, SNARE, CYMBAL, CLOSED_HAT, OPEN_HAT };
enum { SALT_GEN = 1, SALT_VEL = 2, SALT_FILL = 4, SALT_SHAPE = 28, SALT_MUTATE = 29 };
#define CERTAIN 65535u

struct pocket_lane {
	uint64_t digits;                      /* a hex digit per step, first step high: 0-9 */
	uint8_t ghost;                        /* percent */
	uint8_t hits_lo, hits_hi, len_lo, len_hi; /* Euclidean lane if hits_hi */
};

struct pocket_style {
	const char* name;
	uint8_t separate;                     /* the kick avoids the backbeat */
	uint8_t fills;                        /* tracks that fill the end of the bar */
	struct pocket_lane lanes[POCKET_SEQ_TRACKS];
};

#include "pocket_styles.h"

const char* pocket_gen_style_name(unsigned style)
{
	return styles[style < POCKET_GEN_STYLES ? style : 0].name;
}

/* ------------------------------------------------------------------ randomness */
/* CLATTER's src/rhythm/rng.ts: a keyed hash for decisions, mulberry32 for sequences. */
static uint32_t fmix(uint32_t h)
{
	h ^= h >> 16;
	h *= 0x85ebca6bu;
	h ^= h >> 13;
	h *= 0xc2b2ae35u;
	h ^= h >> 16;
	return h;
}

static uint32_t mixin(uint32_t h, uint32_t k)
{
	return fmix((h ^ k) + 0x6a09e667u + (h << 6) + (h >> 2));
}

/* 0..65535 for (seed, purpose, track, step) */
static uint32_t chance(uint32_t seed, uint32_t salt, uint32_t track, uint32_t step)
{
	return mixin(mixin(mixin(mixin(0x9e3779b9u, seed), salt), track), step) >> 16;
}

static uint32_t next(uint32_t* state)
{
	uint32_t t = *state += 0x6d2b79f5u;
	t = (t ^ (t >> 15)) * (t | 1u);
	t ^= t + (t ^ (t >> 7)) * (t | 61u);
	return t ^ (t >> 14);
}

/* uniform below n */
static unsigned below(uint32_t* state, unsigned n)
{
	return (unsigned)(((next(state) >> 16) * n) >> 16);
}

/* ------------------------------------------------------------------ generation */
/* Metric weight of a step: bar downbeat, beat, half beat, sixteenth (DOWNBEAT's). */
static unsigned weight(unsigned step)
{
	step &= POCKET_SEQ_STEPS - 1u;
	return step == 0 ? 256u : step % 4u == 0 ? 205u : step % 2u == 0 ? 128u : 64u;
}

static unsigned digit(const struct pocket_lane* lane, unsigned step)
{
	return (unsigned)(lane->digits >> (60u - 4u * step)) & 15u;
}

static uint32_t apply_density(uint32_t p, unsigned density)
{
	if (p >= CERTAIN) return density < 8u ? 39321u : CERTAIN; /* 0.6: even a certain step can go */
	const unsigned row = density >= 100u ? 9u : density / 10u, across = density >= 100u ? 10u : density % 10u;
	const unsigned at = (unsigned)(p >> 12), part = (unsigned)(p & 4095u);
	const uint32_t low = densityCurve[row][at] + (((uint32_t)(densityCurve[row][at + 1] - densityCurve[row][at]) * part) >> 12);
	const uint32_t high = densityCurve[row + 1][at] + (((uint32_t)(densityCurve[row + 1][at + 1] - densityCurve[row + 1][at]) * part) >> 12);
	return low + (high - low) * across / 10u;
}

/* Strong digits are loud (CLATTER's velocityFor at its default ACCENT). */
static unsigned velocity_for(unsigned strength, uint32_t u)
{
	const int base = 52 + (int)(strength * 62u >> 8);
	const int v = 88 + (base - 88) * 95 / 100 + ((int)(u * 15u >> 16) - 7);
	return (unsigned)(v < 18 ? 18 : v > 127 ? 127 : v);
}

/* Hits spread evenly over `len` steps (euclid.ts), repeating across the bar, as a step mask. */
static uint32_t euclid(unsigned len, unsigned hits, unsigned rotation)
{
	uint32_t row = 0;
	if (hits > len) hits = len;
	for (unsigned i = 0; i < POCKET_SEQ_STEPS; i++)
		if (((i + rotation) % len) * hits % len < hits) row |= 1u << i;
	return row;
}

static uint32_t lane_euclid(const struct pocket_lane* lane, const struct pocket_gen_settings* s, unsigned t)
{
	const unsigned len = lane->len_lo + (chance(s->seed, SALT_GEN, t, 777) * (lane->len_hi - lane->len_lo + 1u) >> 16);
	const unsigned drawn = lane->hits_lo + (chance(s->seed, SALT_GEN, t, 778) * (lane->hits_hi - lane->hits_lo + 1u) >> 16);
	const unsigned scaled = drawn * (60u + s->density * 80u / 100u) / 100u;
	return euclid(len, scaled ? scaled : 1u, chance(s->seed, SALT_GEN, t, 779) * len >> 16);
}

static void lane_row(uint8_t* row, const struct pocket_lane* lane, const struct pocket_gen_settings* s, unsigned t)
{
	const unsigned c = s->complexity;
	const uint32_t euclidRow = lane->hits_hi ? lane_euclid(lane, s, t) : 0;
	unsigned total = 0;
	if (!lane->hits_hi)
		for (unsigned i = 0; i < POCKET_SEQ_STEPS; i++) total += digit(lane, i);
	const unsigned activity = total * 256u / 144u, lively = activity * 3u > 256u ? 256u : activity * 3u;
	for (unsigned i = 0; i < POCKET_SEQ_STEPS; i++)
	{
		const unsigned w = weight(i);
		uint32_t p;
		unsigned strength;
		if (lane->hits_hi)
		{
			const unsigned on = (euclidRow >> i) & 1u;
			p = on ? 63570u : 0;
			strength = on ? (w >= 205u ? 218u : 154u) : 0;
			if (!on && c > 40u && w <= 64u) p = (c - 40u) * 78u;
		}
		else
		{
			const unsigned d = digit(lane, i);
			p = d * CERTAIN / 9u;
			strength = d * 256u / 9u;
			if (d == 0 && total)
			{
				/* a pickup where the lane is silent, mostly just before a strong beat */
				const uint32_t before = weight(i + 1u) >= 205u ? 410u : 256u;
				p = ((((c * 1442u * (256u - w)) >> 8) * before) >> 8) * lively >> 8;
				strength = 77u;
			}
			else if (d > 0 && d < 9u)
				p = (p >> 8) * (256u - c * w / 400u) + c * 52u; /* the weaker written steps loosen */
			if (lane->ghost && p == 0 && w <= 64u)
			{
				p = lane->ghost * (40u + c * 60u / 100u) * 6u;
				strength = 13u;
			}
		}
		if (chance(s->seed, SALT_GEN, t, i) < apply_density(p, s->density))
			row[i] = (uint8_t)velocity_for(strength, chance(s->seed, SALT_VEL, t, i));
		else
			row[i] = 0;
	}
}

/* A fill into the end of the bar (fillBar). High toms lead, low toms follow. */
static void fill_row(uint8_t* row, const struct pocket_gen_settings* s, unsigned t)
{
	const unsigned fills = 20u + s->complexity * 60u / 100u;
	if (chance(s->seed, SALT_FILL, t, 100) * 100u >> 16 >= fills) return;
	const unsigned from = chance(s->seed, SALT_FILL, t, 99) * 100u >> 16 < 15u + fills / 3u ? 8u : 12u;
	const unsigned span = POCKET_SEQ_STEPS - from;
	for (unsigned i = from; i < POCKET_SEQ_STEPS; i++)
	{
		const unsigned k = (i - from) * 256u / span;
		unsigned percent = 35u + fills / 2u;
		if (t == HIGH_TOM) percent = percent * (256u - k * 154u / 256u) / 256u;
		if (t == LOW_TOM) percent = percent * (102u + k * 154u / 256u) / 256u;
		if (chance(s->seed, SALT_FILL, t, i) * 100u >> 16 < percent)
			row[i] = (uint8_t)(60u + (k * 55u >> 8) + (chance(s->seed, SALT_VEL, t, i + 32u) * 16u >> 16));
	}
}

static void rotate_row(uint8_t* row, unsigned by)
{
	uint8_t was[POCKET_SEQ_STEPS];
	for (unsigned i = 0; i < POCKET_SEQ_STEPS; i++) was[i] = row[i];
	for (unsigned i = 0; i < POCKET_SEQ_STEPS; i++) row[(i + by) & (POCKET_SEQ_STEPS - 1u)] = was[i];
}

/* Above 60 %: rhythmic displacement; above 80 %: the grid itself starts to go. */
static void shape(struct pocket_pattern* p, const struct pocket_gen_settings* s)
{
	const unsigned c = s->complexity;
	if (c <= 60u) return;
	static const uint8_t displaced[] = { LOW_TOM, HIGH_TOM, CYMBAL, OPEN_HAT };
	for (unsigned i = 0; i < sizeof displaced; i++)
	{
		const unsigned t = displaced[i];
		if (!(s->locks >> t & 1u) && (chance(s->seed, SALT_SHAPE, t, 0) * 100u >> 16) < (c - 60u) * 2u)
			rotate_row(p->vel[t], 1u + (chance(s->seed, SALT_SHAPE, t, 1) * 15u >> 16));
	}
	if (c <= 80u) return;
	if (!(s->locks >> CLOSED_HAT & 1u))
	{
		static const uint8_t primes[] = { 5, 7, 11, 13 };
		const unsigned len = primes[chance(s->seed, SALT_SHAPE, CLOSED_HAT, 2) * 4u >> 16];
		const uint32_t row = euclid(len, 2u + (chance(s->seed, SALT_SHAPE, CLOSED_HAT, 3) * (len - 2u) >> 16), chance(s->seed, SALT_SHAPE, CLOSED_HAT, 4) * len >> 16);
		for (unsigned i = 0; i < POCKET_SEQ_STEPS; i++)
			p->vel[CLOSED_HAT][i] = row >> i & 1u ? (uint8_t)velocity_for(weight(i), chance(s->seed, SALT_VEL, CLOSED_HAT, i + 64u)) : 0;
	}
}

/* Rules across tracks (arrange). Locked tracks are never written, but are heard by the others. */
static void arrange(struct pocket_pattern* p, const struct pocket_gen_settings* s)
{
	const struct pocket_style* style = &styles[s->style];
	const unsigned locks = s->locks;
	const unsigned anchored = s->complexity <= 80u;
	uint8_t* kick = p->vel[KICK];
	for (unsigned i = 0; i < POCKET_SEQ_STEPS; i++)
	{
		if (!(locks >> CLOSED_HAT & 1u) && p->vel[OPEN_HAT][i]) p->vel[CLOSED_HAT][i] = 0;
		if (!(locks >> KICK & 1u) && style->separate && (i == 4u || i == 12u) && p->vel[SNARE][i] > 90u) kick[i] = 0;
	}
	if (!(locks >> KICK & 1u) && anchored && !style->lanes[KICK].hits_hi && !kick[0])
		kick[0] = (uint8_t)velocity_for(256u, chance(s->seed, SALT_VEL, KICK, 0));
	for (unsigned t = LOW_TOM; t <= HIGH_TOM; t++)
		for (unsigned i = 0; i < POCKET_SEQ_STEPS; i++)
			if (!(locks >> t & 1u) && kick[i] && anchored) p->vel[t][i] = 0;
	/* never an empty bar: a generated pattern always plays something */
	unsigned hits = 0;
	for (unsigned t = 0; t < POCKET_SEQ_TRACKS; t++)
		for (unsigned i = 0; i < POCKET_SEQ_STEPS; i++) hits += p->vel[t][i] != 0;
	if (!hits && !(locks & 1u)) kick[0] = 100;
}

void pocket_gen_generate(struct pocket_pattern* p, const struct pocket_gen_settings* s)
{
	const struct pocket_style* style = &styles[s->style < POCKET_GEN_STYLES ? s->style : 0];
	for (unsigned t = 0; t < POCKET_SEQ_TRACKS; t++)
	{
		if (s->locks >> t & 1u) continue;
		lane_row(p->vel[t], &style->lanes[t], s, t);
		if (style->fills >> t & 1u) fill_row(p->vel[t], s, t);
	}
	shape(p, s);
	arrange(p, s);
}

/* ------------------------------------------------------------------ mutation */
enum { OP_ADD, OP_REMOVE, OP_SHIFT, OP_VELOCITY, OP_ACCENT, OP_SWAP, OP_DENSITY, OP_REST, OP_RECOMBINE, OP_ROTATE, OP_REVERSE, OPS };

/* mutate.ts's weights, and the strength from which each edit is allowed, in 0..256. */
static const uint8_t opWeight[OPS] = { 24, 20, 16, 12, 8, 8, 5, 4, 5, 3, 2 };
static const uint8_t opFrom[OPS] = { 0, 0, 0, 0, 0, 38, 77, 90, 102, 154, 154 };
/* strength 0.2, 0.5, 0.85 of 256, and their 1.4th powers (how many edits) */
static const uint8_t strengths[POCKET_MUTATE_STRENGTHS] = { 51, 128, 218 };
static const uint8_t editShare[POCKET_MUTATE_STRENGTHS] = { 27, 97, 204 };

struct mutation {
	uint32_t rng;
	unsigned strength;
};

enum { WANT_HIT, WANT_REST, WANT_HIT_WEAK, WANT_ANY_OFF_BAR, WANT_REMOVABLE, WANT_NEAR_HIT };

/* A step wanted by `want`, weak metric positions preferred at low strength (pickPos), or -1. */
static int pick(struct mutation* m, const uint8_t* row, unsigned want)
{
	unsigned total = 0, weights[POCKET_SEQ_STEPS];
	for (unsigned i = 0; i < POCKET_SEQ_STEPS; i++)
	{
		const unsigned w = weight(i);
		unsigned ok = 0;
		switch (want)
		{
		case WANT_HIT: ok = row[i] != 0; break;
		case WANT_REST: ok = row[i] == 0; break;
		case WANT_ANY_OFF_BAR: ok = i != 0; break;
		case WANT_REMOVABLE: ok = row[i] && (m->strength > 128u || row[i] < 100u || w < 205u); break;
		case WANT_NEAR_HIT: ok = row[i] || row[(i + 1u) & (POCKET_SEQ_STEPS - 1u)]; break;
		default: break;
		}
		const unsigned share = 256u - (w * (256u - m->strength) >> 8);
		weights[i] = ok ? (share < 5u ? 5u : share) : 0;
		total += weights[i];
	}
	if (!total) return -1;
	unsigned x = below(&m->rng, total);
	for (unsigned i = 0; i < POCKET_SEQ_STEPS; i++)
	{
		if (x < weights[i]) return (int)i;
		x -= weights[i];
	}
	return -1;
}

static unsigned edit(struct mutation* m, uint8_t* row, unsigned op)
{
	enum { N = POCKET_SEQ_STEPS, BEAT = 4 };
	int i, j;
	switch (op)
	{
	case OP_ADD:
		if ((i = pick(m, row, WANT_REST)) < 0) return 0;
		row[i] = (uint8_t)(45u + below(&m->rng, 45));
		return 1;
	case OP_REMOVE:
		if ((i = pick(m, row, WANT_REMOVABLE)) < 0) return 0;
		row[i] = 0;
		return 1;
	case OP_SHIFT:
		if ((i = pick(m, row, WANT_HIT)) < 0) return 0;
		j = (i + (below(&m->rng, 2) ? 1 : N - 1)) & (N - 1);
		if (row[j]) return 0;
		row[j] = row[i];
		row[i] = 0;
		return 1;
	case OP_SWAP:
	{
		if ((i = pick(m, row, WANT_NEAR_HIT)) < 0) return 0;
		j = (i + 1) & (N - 1);
		const uint8_t was = row[i];
		row[i] = row[j];
		row[j] = was;
		return 1;
	}
	case OP_VELOCITY:
	{
		if ((i = pick(m, row, WANT_HIT)) < 0) return 0;
		const int by = 12 + (int)below(&m->rng, 18), v = row[i] + (below(&m->rng, 2) ? by : -by);
		row[i] = (uint8_t)(v < 1 ? 1 : v > 127 ? 127 : v);
		return 1;
	}
	case OP_ACCENT:
		if ((i = pick(m, row, WANT_HIT)) < 0) return 0;
		row[i] = row[i] >= 110u ? 80u : (uint8_t)(118u + below(&m->rng, 9));
		return 1;
	case OP_DENSITY:
	{
		const unsigned add = below(&m->rng, 2), wanted = add ? WANT_REST : WANT_HIT;
		unsigned done = 0;
		for (unsigned c = 0; c < 2u; c++)
		{
			if ((i = pick(m, row, wanted)) < 0) break;
			row[i] = add ? (uint8_t)(40u + below(&m->rng, 30)) : 0;
			done = 1;
		}
		return done;
	}
	case OP_REST:
	{
		const unsigned length = 2u + below(&m->rng, 3);
		unsigned any = 0;
		if ((i = pick(m, row, WANT_ANY_OFF_BAR)) < 0) return 0;
		for (unsigned c = 0; c < length && i + (int)c < N; c++)
		{
			any |= row[i + (int)c];
			row[i + (int)c] = 0;
		}
		return any != 0;
	}
	case OP_RECOMBINE:
	{
		const unsigned from = below(&m->rng, N / BEAT);
		unsigned to = below(&m->rng, N / BEAT - 1);
		if (to >= from) to++;
		for (unsigned c = 0; c < BEAT; c++) row[to * BEAT + c] = row[from * BEAT + c];
		return 1;
	}
	case OP_ROTATE:
		rotate_row(row, below(&m->rng, 2) ? 1u : N - 1u);
		return 1;
	case OP_REVERSE:
	{
		const unsigned at = below(&m->rng, N / BEAT) * BEAT;
		for (unsigned c = 0; c < BEAT / 2; c++)
		{
			const uint8_t was = row[at + c];
			row[at + c] = row[at + BEAT - 1u - c];
			row[at + BEAT - 1u - c] = was;
		}
		return 1;
	}
	}
	return 0;
}

unsigned pocket_gen_mutate(struct pocket_pattern* p, uint32_t seed, unsigned strength, unsigned locks)
{
	if (strength >= POCKET_MUTATE_STRENGTHS) strength = POCKET_MUTATE_LARGE;
	struct mutation m = { mixin(seed, SALT_MUTATE), strengths[strength] };
	uint8_t tracks[POCKET_SEQ_TRACKS];
	unsigned count = 0, hits = 0;
	for (unsigned t = 0; t < POCKET_SEQ_TRACKS; t++)
	{
		if (locks >> t & 1u) continue;
		tracks[count++] = (uint8_t)t;
		for (unsigned i = 0; i < POCKET_SEQ_STEPS; i++) hits += p->vel[t][i] != 0;
	}
	if (!count) return 0;
	unsigned edits = (editShare[strength] * (hits / 2u + 3u) + 128u) >> 8;
	if (!edits) edits = 1;
	unsigned total = 0, made = 0;
	for (unsigned o = 0; o < OPS; o++)
		if (m.strength >= opFrom[o]) total += opWeight[o];
	/* an edit can find nothing to do (a shift into a hit): draw again, within a bound */
	for (unsigned tries = 0; made < edits && tries < 4u * edits; tries++)
	{
		unsigned x = below(&m.rng, total), op = 0;
		for (unsigned o = 0; o < OPS; o++)
		{
			if (m.strength < opFrom[o]) continue;
			if (x < opWeight[o]) { op = o; break; }
			x -= opWeight[o];
		}
		made += edit(&m, p->vel[tracks[below(&m.rng, count)]], op);
	}
	return made;
}
