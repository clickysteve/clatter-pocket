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
enum { SALT_GEN = 1, SALT_VEL = 2, SALT_FILL = 4, SALT_SHAPE = 28, SALT_MUTATE = 29, SALT_TRACK = 30, SALT_PHRASE = 31 };
#define CERTAIN 65535u

struct pocket_lane {
	uint64_t digits;                      /* a hex digit per step, first step high: 0-9 */
	uint8_t ghost;                        /* percent */
	uint8_t hits_lo, hits_hi, len_lo, len_hi; /* Euclidean lane if hits_hi */
} __attribute__((packed));

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

uint32_t pocket_gen_mix(uint32_t h, uint32_t k)
{
	return fmix((h ^ k) + 0x6a09e667u + (h << 6) + (h >> 2));
}

/* 0..65535 for (seed, purpose, track, step) */
static uint32_t chance(uint32_t seed, uint32_t salt, uint32_t track, uint32_t step)
{
	return pocket_gen_mix(pocket_gen_mix(pocket_gen_mix(pocket_gen_mix(0x9e3779b9u, seed), salt), track), step) >> 16;
}

static uint32_t next(uint32_t* state)
{
	uint32_t t = *state += 0x6d2b79f5u;
	t = (t ^ (t >> 15)) * (t | 1u);
	t ^= t + (t ^ (t >> 7)) * (t | 61u);
	return t ^ (t >> 14);
}

/* uniform below n */
unsigned pocket_gen_below(uint32_t* state, unsigned n)
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
	const uint8_t* bytes = (const uint8_t*)&lane->digits; /* little-endian, host and FM-1 */
	const unsigned byte = bytes[7u - step / 2u];
	return step & 1u ? byte & 15u : byte >> 4;
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

/* Hits spread evenly over `len` steps (euclid.ts), repeating: is there one on step i? */
static unsigned euclid(unsigned i, unsigned len, unsigned hits, unsigned rotation)
{
	if (hits > len) hits = len;
	return ((i + rotation) % len) * hits % len < hits;
}

/* What a page after the first redraws of the one it follows, in percent, by role: the kick and
 * snare hold the phrase together, toms and percussion move. */
static const uint8_t phraseChange[POCKET_SEQ_TRACKS] = { 10, 40, 40, 10, 25, 20, 30 };

static void lane_row(uint32_t* row, const struct pocket_lane* lane, const struct pocket_gen_settings* s, unsigned t, unsigned steps)
{
	const unsigned c = s->complexity;
	unsigned len = 1, hits = 0, rotation = 0, total = 0;
	if (lane->hits_hi)
	{
		len = lane->len_lo + (chance(s->seed, SALT_GEN, t, 777) * (lane->len_hi - lane->len_lo + 1u) >> 16);
		const unsigned drawn = lane->hits_lo + (chance(s->seed, SALT_GEN, t, 778) * (lane->hits_hi - lane->hits_lo + 1u) >> 16);
		hits = drawn * (60u + s->density * 80u / 100u) / 100u;
		if (!hits) hits = 1;
		rotation = chance(s->seed, SALT_GEN, t, 779) * len >> 16;
	}
	else
		for (unsigned i = 0; i < POCKET_SEQ_STEPS; i++) total += digit(lane, i);
	const unsigned activity = total * 256u / 144u, lively = activity * 3u > 256u ? 256u : activity * 3u;
	const uint32_t change = (phraseChange[t] + c / 2u) * 655u;
	for (unsigned i = 0; i < steps; i++)
	{
		/* a Euclidean lane runs on across the pages: that is its cross-rhythm. Any other answers
		   the page it follows, A A' A A'', and redraws a few steps on its own key */
		if (i >= POCKET_SEQ_STEPS && !lane->hits_hi && chance(s->seed, SALT_PHRASE, t, i) >= change)
		{
			row[i] = row[i - (i >= 2u * POCKET_SEQ_STEPS ? 2u * POCKET_SEQ_STEPS : POCKET_SEQ_STEPS)];
			continue;
		}
		const unsigned w = weight(i);
		uint32_t p;
		unsigned strength;
		if (lane->hits_hi)
		{
			const unsigned on = euclid(i, len, hits, rotation);
			p = on ? 63570u : 0;
			strength = on ? (w >= 205u ? 218u : 154u) : 0;
			if (!on && c > 40u && w <= 64u) p = (c - 40u) * 78u;
		}
		else
		{
			const unsigned d = digit(lane, i & (POCKET_SEQ_STEPS - 1u));
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
			row[i] = velocity_for(strength, chance(s->seed, SALT_VEL, t, i));
		else
			row[i] = 0;
	}
}

/* A fill into the end of a page, `at` its first step (fillBar). High toms lead, low toms follow. */
static void fill_row(uint32_t* row, const struct pocket_gen_settings* s, unsigned t, unsigned at)
{
	const unsigned fills = 20u + s->complexity * 60u / 100u;
	if (chance(s->seed, SALT_FILL, t, 100u + at) * 100u >> 16 >= fills) return;
	const unsigned from = chance(s->seed, SALT_FILL, t, 99u + at) * 100u >> 16 < 15u + fills / 3u ? 8u : 12u;
	const unsigned span = POCKET_SEQ_STEPS - from;
	for (unsigned i = from; i < POCKET_SEQ_STEPS; i++)
	{
		const unsigned k = (i - from) * 256u / span;
		unsigned percent = 35u + fills / 2u;
		if (t == HIGH_TOM) percent = percent * (256u - k * 154u / 256u) / 256u;
		if (t == LOW_TOM) percent = percent * (102u + k * 154u / 256u) / 256u;
		if (chance(s->seed, SALT_FILL, t, i + at) * 100u >> 16 < percent)
			row[at + i] = 60u + (k * 55u >> 8) + (chance(s->seed, SALT_VEL, t, i + at + 32u) * 16u >> 16);
	}
}

static void reverse_row(uint32_t* row, unsigned from, unsigned to)
{
	while (from + 1u < to)
	{
		const uint32_t was = row[from];
		row[from++] = row[--to];
		row[to] = was;
	}
}

/* Later by `by` steps, round the pattern. */
static void rotate_row(uint32_t* row, unsigned by, unsigned steps)
{
	reverse_row(row, 0, steps);
	reverse_row(row, 0, by);
	reverse_row(row, by, steps);
}

/* Above 60 %: rhythmic displacement; above 80 %: the grid itself starts to go. */
static void shape(struct pocket_pattern* p, const struct pocket_gen_settings* s)
{
	const unsigned c = s->complexity, steps = POCKET_PATTERN_STEPS(p);
	if (c <= 60u) return;
	static const uint8_t displaced[] = { LOW_TOM, HIGH_TOM, CYMBAL, OPEN_HAT };
	for (unsigned i = 0; i < sizeof displaced; i++)
	{
		const unsigned t = displaced[i];
		if (!(s->locks >> t & 1u) && (chance(s->seed, SALT_SHAPE, t, 0) * 100u >> 16) < (c - 60u) * 2u)
			rotate_row(p->cell[t], 1u + (chance(s->seed, SALT_SHAPE, t, 1) * 15u >> 16), steps);
	}
	if (c <= 80u) return;
	if (!(s->locks >> CLOSED_HAT & 1u))
	{
		static const uint8_t primes[] = { 5, 7, 11, 13 };
		const unsigned len = primes[chance(s->seed, SALT_SHAPE, CLOSED_HAT, 2) * 4u >> 16];
		const unsigned hits = 2u + (chance(s->seed, SALT_SHAPE, CLOSED_HAT, 3) * (len - 2u) >> 16), rotation = chance(s->seed, SALT_SHAPE, CLOSED_HAT, 4) * len >> 16;
		for (unsigned i = 0; i < steps; i++)
			p->cell[CLOSED_HAT][i] = euclid(i, len, hits, rotation) ? velocity_for(weight(i), chance(s->seed, SALT_VEL, CLOSED_HAT, i + 64u)) : 0;
	}
}

/* Rules across tracks (arrange). Locked tracks are never written, but are heard by the others. */
static void arrange(struct pocket_pattern* p, const struct pocket_gen_settings* s)
{
	const struct pocket_style* style = &styles[s->style];
	const unsigned locks = s->locks;
	const unsigned anchored = s->complexity <= 80u, steps = POCKET_PATTERN_STEPS(p);
	uint32_t* kick = p->cell[KICK];
	for (unsigned i = 0; i < steps; i++)
	{
		if (!(locks >> CLOSED_HAT & 1u) && p->cell[OPEN_HAT][i]) p->cell[CLOSED_HAT][i] = 0;
		if (!(locks >> KICK & 1u) && style->separate && (i & 7u) == 4u && POCKET_CELL_VELOCITY(p->cell[SNARE][i]) > 90u) kick[i] = 0;
	}
	if (!(locks >> KICK & 1u) && anchored && !style->lanes[KICK].hits_hi && !kick[0])
		kick[0] = velocity_for(256u, chance(s->seed, SALT_VEL, KICK, 0));
	for (unsigned t = LOW_TOM; t <= HIGH_TOM; t++)
		for (unsigned i = 0; i < steps; i++)
			if (!(locks >> t & 1u) && kick[i] && anchored) p->cell[t][i] = 0;
	/* never an empty bar: a generated pattern always plays something */
	unsigned hits = 0;
	for (unsigned t = 0; t < POCKET_SEQ_TRACKS; t++)
		for (unsigned i = 0; i < steps; i++) hits += p->cell[t][i] != 0;
	if (!hits && !(locks & 1u)) kick[0] = 100;
}

void pocket_gen_track(struct pocket_pattern* p, const struct pocket_gen_settings* s, unsigned track, unsigned take)
{
	struct pocket_gen_settings one = *s;
	one.locks = (uint8_t)(POCKET_GEN_ALL_TRACKS & ~(1u << track));
	const unsigned steps = (s->last_page + 1u) * POCKET_SEQ_STEPS;
	uint64_t was = 0;
	for (unsigned i = 0; i < steps; i++) was |= (uint64_t)(p->cell[track][i] != 0) << i;
	/* a lane can draw nothing, or be all certainties and draw what is there: neither is a new rhythm */
	for (unsigned tries = 0; tries < 8u; tries++)
	{
		one.seed = pocket_gen_mix(pocket_gen_mix(s->seed, track + SALT_TRACK), take + tries);
		pocket_gen_generate(p, &one);
		unsigned hits = 0, differs = 0;
		for (unsigned i = 0; i < steps; i++)
		{
			hits += p->cell[track][i] != 0;
			differs += (p->cell[track][i] != 0) != (unsigned)(was >> i & 1u);
		}
		if (hits && differs) return;
	}
	const unsigned hits = 3u + (chance(one.seed, SALT_TRACK, track, 0) * 6u >> 16), rotation = 1u + (chance(one.seed, SALT_TRACK, track, 1) * 15u >> 16);
	for (unsigned i = 0; i < steps; i++)
		p->cell[track][i] = euclid(i, POCKET_SEQ_STEPS, hits, rotation) ? velocity_for(weight(i), chance(one.seed, SALT_VEL, track, i)) : 0;
}

void pocket_gen_generate(struct pocket_pattern* p, const struct pocket_gen_settings* s)
{
	const struct pocket_style* style = &styles[s->style < POCKET_GEN_STYLES ? s->style : 0];
	p->last_page = s->last_page & 3u;
	const unsigned steps = POCKET_PATTERN_STEPS(p);
	for (unsigned t = 0; t < POCKET_SEQ_TRACKS; t++)
	{
		if (s->locks >> t & 1u) continue;
		lane_row(p->cell[t], &style->lanes[t], s, t, steps);
		for (unsigned i = steps; i < POCKET_SEQ_MAX_STEPS; i++) p->cell[t][i] = 0;
		if (style->fills >> t & 1u)
		{
			fill_row(p->cell[t], s, t, steps - POCKET_SEQ_STEPS);
			if (steps == POCKET_SEQ_MAX_STEPS) fill_row(p->cell[t], s, t, POCKET_SEQ_STEPS);
		}
	}
	shape(p, s);
	arrange(p, s);
}

/* ------------------------------------------------------------------ mutation */
enum { OP_ADD, OP_REMOVE, OP_SHIFT, OP_VELOCITY, OP_ACCENT, OP_SWAP, OP_DENSITY, OP_REST, OP_RECOMBINE, OP_ROTATE, OP_REVERSE, OP_PITCH, OPS };

/* mutate.ts's weights, and the strength from which each edit is allowed, in 0..256. */
static const uint8_t opWeight[OPS] = { 24, 20, 16, 12, 8, 8, 5, 4, 5, 3, 2, 6 };
static const uint8_t opFrom[OPS] = { 0, 0, 0, 0, 0, 38, 77, 90, 102, 154, 154, 0 };
/* strength 0.2, 0.5, 0.85 of 256, and their 1.4th powers (how many edits) */
static const uint8_t strengths[POCKET_MUTATE_STRENGTHS] = { 51, 128, 218 };
static const uint8_t editShare[POCKET_MUTATE_STRENGTHS] = { 27, 97, 204 };

struct mutation {
	uint32_t rng;
	unsigned strength, steps;
};

enum { WANT_HIT, WANT_REST, WANT_HIT_WEAK, WANT_ANY_OFF_BAR, WANT_REMOVABLE, WANT_NEAR_HIT };

/* How much step i is wanted by `want`: weak metric positions preferred at low strength (pickPos). */
static unsigned wanted(const struct mutation* m, const uint32_t* row, unsigned want, unsigned i)
{
	const unsigned w = weight(i);
	unsigned ok = 0;
	switch (want)
	{
	case WANT_HIT: ok = row[i] != 0; break;
	case WANT_REST: ok = row[i] == 0; break;
	case WANT_ANY_OFF_BAR: ok = i != 0; break;
	case WANT_REMOVABLE: ok = row[i] && (m->strength > 128u || POCKET_CELL_VELOCITY(row[i]) < 100u || w < 205u); break;
	case WANT_NEAR_HIT: ok = row[i] || row[(i + 1u) % m->steps]; break;
	default: break;
	}
	const unsigned share = 256u - (w * (256u - m->strength) >> 8);
	return ok ? (share < 5u ? 5u : share) : 0;
}

/* A step wanted by `want`, or -1. */
static int pick(struct mutation* m, const uint32_t* row, unsigned want)
{
	unsigned total = 0;
	for (unsigned i = 0; i < m->steps; i++) total += wanted(m, row, want, i);
	if (!total) return -1;
	unsigned x = pocket_gen_below(&m->rng, total);
	for (unsigned i = 0; i < m->steps; i++)
	{
		const unsigned share = wanted(m, row, want, i);
		if (x < share) return (int)i;
		x -= share;
	}
	return -1;
}

static unsigned edit(struct mutation* m, uint32_t* row, unsigned op)
{
	enum { BEAT = 4 };
	const int N = (int)m->steps;
	int i, j;
	switch (op)
	{
	case OP_ADD:
		if ((i = pick(m, row, WANT_REST)) < 0) return 0;
		row[i] = 45u + pocket_gen_below(&m->rng, 45);
		return 1;
	case OP_REMOVE:
		if ((i = pick(m, row, WANT_REMOVABLE)) < 0) return 0;
		row[i] = 0;
		return 1;
	case OP_SHIFT:
		if ((i = pick(m, row, WANT_HIT)) < 0) return 0;
		j = (i + (pocket_gen_below(&m->rng, 2) ? 1 : N - 1)) % N;
		if (row[j]) return 0;
		row[j] = row[i];
		row[i] = 0;
		return 1;
	case OP_SWAP:
	{
		if ((i = pick(m, row, WANT_NEAR_HIT)) < 0) return 0;
		j = (i + 1) % N;
		const uint32_t was = row[i];
		row[i] = row[j];
		row[j] = was;
		return 1;
	}
	case OP_VELOCITY:
	{
		if ((i = pick(m, row, WANT_HIT)) < 0) return 0;
		const int by = 12 + (int)pocket_gen_below(&m->rng, 18), v = (int)POCKET_CELL_VELOCITY(row[i]) + (pocket_gen_below(&m->rng, 2) ? by : -by);
		row[i] = (row[i] & ~0xffu) | (uint32_t)(v < 1 ? 1 : v > 127 ? 127 : v);
		return 1;
	}
	case OP_ACCENT:
		if ((i = pick(m, row, WANT_HIT)) < 0) return 0;
		row[i] = (row[i] & ~0xffu) | (POCKET_CELL_VELOCITY(row[i]) >= 110u ? 80u : 118u + pocket_gen_below(&m->rng, 9));
		return 1;
	case OP_DENSITY:
	{
		const unsigned add = pocket_gen_below(&m->rng, 2), wanted = add ? WANT_REST : WANT_HIT;
		unsigned done = 0;
		for (unsigned c = 0; c < 2u; c++)
		{
			if ((i = pick(m, row, wanted)) < 0) break;
			row[i] = add ? 40u + pocket_gen_below(&m->rng, 30) : 0;
			done = 1;
		}
		return done;
	}
	case OP_REST:
	{
		const unsigned length = 2u + pocket_gen_below(&m->rng, 3);
		uint32_t any = 0;
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
		const unsigned from = pocket_gen_below(&m->rng, N / BEAT);
		unsigned to = pocket_gen_below(&m->rng, N / BEAT - 1);
		if (to >= from) to++;
		for (unsigned c = 0; c < BEAT; c++) row[to * BEAT + c] = row[from * BEAT + c];
		return 1;
	}
	case OP_ROTATE:
		rotate_row(row, pocket_gen_below(&m->rng, 2) ? 1u : (unsigned)N - 1u, (unsigned)N);
		return 1;
	case OP_REVERSE:
	{
		const unsigned at = pocket_gen_below(&m->rng, N / BEAT) * BEAT;
		for (unsigned c = 0; c < BEAT / 2; c++)
		{
			const uint32_t was = row[at + c];
			row[at + c] = row[at + BEAT - 1u - c];
			row[at + BEAT - 1u - c] = was;
		}
		return 1;
	}
	case OP_PITCH:
	{
		/* only a track that already plays notes moves, unless the change is meant to be big:
		   a rhythm of programmed pitches keeps its sound at LOW and MED */
		static const int8_t intervals[8] = { -12, -7, -5, -2, 2, 5, 7, 12 };
		unsigned pitched = 0;
		for (int c = 0; c < N; c++) pitched |= POCKET_CELL_NOTE(row[c]);
		/* a step has a note or a variation: a variation stays what was played */
		if ((!pitched && m->strength <= 128u) || (i = pick(m, row, WANT_HIT)) < 0 || POCKET_CELL_VARIATION(row[i])) return 0;
		const unsigned from = POCKET_CELL_NOTE(row[i]) ? POCKET_CELL_NOTE(row[i]) : POCKET_SEQ_NOTE_PROGRAMMED;
		const int to = (int)from + intervals[pocket_gen_below(&m->rng, 8)];
		row[i] = (row[i] & ~0xff00u) | (to < 12 || to > 115 ? from : (unsigned)to) << 8;
		return 1;
	}
	}
	return 0;
}

unsigned pocket_gen_mutate(struct pocket_pattern* p, uint32_t seed, unsigned strength, unsigned locks)
{
	if (strength >= POCKET_MUTATE_STRENGTHS) strength = POCKET_MUTATE_LARGE;
	struct mutation m = { pocket_gen_mix(seed, SALT_MUTATE), strengths[strength], POCKET_PATTERN_STEPS(p) };
	uint8_t tracks[POCKET_SEQ_TRACKS];
	unsigned count = 0, hits = 0;
	for (unsigned t = 0; t < POCKET_SEQ_TRACKS; t++)
	{
		if (locks >> t & 1u) continue;
		tracks[count++] = (uint8_t)t;
		for (unsigned i = 0; i < m.steps; i++) hits += p->cell[t][i] != 0;
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
		unsigned x = pocket_gen_below(&m.rng, total), op = 0;
		for (unsigned o = 0; o < OPS; o++)
		{
			if (m.strength < opFrom[o]) continue;
			if (x < opWeight[o]) { op = o; break; }
			x -= opWeight[o];
		}
		made += edit(&m, p->cell[tracks[pocket_gen_below(&m.rng, count)]], op);
	}
	return made;
}
