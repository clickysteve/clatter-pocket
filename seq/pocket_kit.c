/*
 * pocket_kit.c - kit design. See pocket_kit.h.
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 * The archetypes and WILDNESS are CLATTER's src/sound/kitgen.ts, reduced to fit.
 *
 * Not random parameters: each voice is drawn from an ARCHETYPE that knows how the LXR's parameters
 * relate for that sound (a kick is a low sine with a fast, deep pitch sweep; a hat a high carrier
 * with two noisy FM modulators through a high-pass; a clap the snare voice's noise with envelope
 * repeats). A CHARACTER leans the ranges, and a FAMILY drawn from the seed alone gives all the
 * voices of a kit the same root, length, brightness and dirt, which is what makes them a kit.
 *
 * WILDNESS is how strictly a voice follows its archetype: low draws from a narrow core and the
 * usual menu choice, 50 is the recipe as written, high widens the ranges, opens the menus and
 * lets voices become other archetypes, with swells, sirens and rolls. Level, drive and resonance
 * are not widened: wild is different, not louder. The kick keeps its archetype and its menus at
 * any WILDNESS: a kit needs an anchor.
 */
#include "pocket_kit.h"
#include "pocket_gen.h"
#include "pocket_lxr.h"
#include "pocket_params.h"

/* What a voice's parameters are called here. From K_ATTACK on, WILDNESS does not widen them. */
enum {
	K_WAVE, K_COARSE, K_FINE, K_PWM, K_FM_AMT, K_FM_FREQ, K_FM_WAVE, K_FM_MODE, K_M1_WAVE, K_M1_FREQ, K_M1_AMT,
	K_M2_WAVE, K_M2_FREQ, K_M2_AMT, K_NOISE_FREQ, K_NOISE_MIX, K_P_DECAY, K_P_SLOPE, K_P_AMT, K_DECAY,
	K_DECAY_OPEN, K_SLOPE, K_REPEAT, K_T_WAVE, K_T_VOL, K_T_FREQ, K_CUTOFF, K_FTYPE,
	K_ATTACK, K_RESO, K_FDRIVE, K_VOL, K_PAN, K_RATE, K_DRIVE, KEYS
};

#define P(name) POCKET_PAR_##name
#define ROW6(a, b, c, d, e, f) { P(a), P(b), P(c), P(d), P(e), P(f) }
#define RUN6(name) ROW6(name##1, name##2, name##3, name##4, name##5, name##6)
#define RUN6_(name) ROW6(name##_1, name##_2, name##_3, name##_4, name##_5, name##_6)
/* 0: the voice has no such parameter */
static const uint8_t keyParam[KEYS][POCKET_LXR_VOICES] = {
	[K_WAVE] = ROW6(OSC_WAVE_DRUM1, OSC_WAVE_DRUM2, OSC_WAVE_DRUM3, OSC_WAVE_SNARE, WAVE1_CYM, WAVE1_HH),
	[K_COARSE] = RUN6(COARSE),
	[K_FINE] = RUN6(FINE),
	[K_PWM] = RUN6_(PWM),
	[K_FM_AMT] = { P(FMAMNT1), P(FMAMNT2), P(FMAMNT3) },
	[K_FM_FREQ] = { P(FM_FREQ1), P(FM_FREQ2), P(FM_FREQ3) },
	[K_FM_WAVE] = { P(MOD_WAVE_DRUM1), P(MOD_WAVE_DRUM2), P(MOD_WAVE_DRUM3) },
	[K_FM_MODE] = { P(MIX_MOD_1), P(MIX_MOD_2), P(MIX_MOD_3) },
	[K_M1_WAVE] = { [4] = P(WAVE2_CYM), [5] = P(WAVE2_HH) },
	[K_M1_FREQ] = { [4] = P(MOD_OSC_F1_CYM), [5] = P(MOD_OSC_F1) },
	[K_M1_AMT] = { [4] = P(MOD_OSC_GAIN1_CYM), [5] = P(MOD_OSC_GAIN1) },
	[K_M2_WAVE] = { [4] = P(WAVE3_CYM), [5] = P(WAVE3_HH) },
	[K_M2_FREQ] = { [4] = P(MOD_OSC_F2_CYM), [5] = P(MOD_OSC_F2) },
	[K_M2_AMT] = { [4] = P(MOD_OSC_GAIN2_CYM), [5] = P(MOD_OSC_GAIN2) },
	[K_NOISE_FREQ] = { [3] = P(NOISE_FREQ1) },
	[K_NOISE_MIX] = { [3] = P(MIX1) },
	[K_P_DECAY] = { P(MOD_EG1), P(MOD_EG2), P(MOD_EG3), P(MOD_EG4) },
	[K_P_SLOPE] = { P(PITCH_SLOPE1), P(PITCH_SLOPE2), P(PITCH_SLOPE3), P(PITCH_SLOPE4) },
	[K_P_AMT] = { P(MODAMNT1), P(MODAMNT2), P(MODAMNT3), P(MODAMNT4) },
	[K_DECAY] = ROW6(VELOD1, VELOD2, VELOD3, VELOD4, VELOD5, VELOD6_CLOSED),
	[K_DECAY_OPEN] = { [5] = P(VELOD6_OPEN) },
	[K_SLOPE] = RUN6(VOL_SLOPE),
	[K_REPEAT] = { [3] = P(REPEAT4), [4] = P(REPEAT5) },
	[K_T_WAVE] = ROW6(TRANS1_WAVE, TRANS2_WAVE, TRANS3_WAVE, TRANS4_WAVE, TRANS5_WAVE, TRANS6_WAVE),
	[K_T_VOL] = ROW6(TRANS1_VOL, TRANS2_VOL, TRANS3_VOL, TRANS4_VOL, TRANS5_VOL, TRANS6_VOL),
	[K_T_FREQ] = ROW6(TRANS1_FREQ, TRANS2_FREQ, TRANS3_FREQ, TRANS4_FREQ, TRANS5_FREQ, TRANS6_FREQ),
	[K_CUTOFF] = RUN6_(FILTER_FREQ),
	[K_FTYPE] = RUN6_(FILTER_TYPE),
	[K_ATTACK] = ROW6(VELOA1, VELOA2, VELOA3, VELOA4, VELOA5, VELOA6),
	[K_RESO] = RUN6_(RESO),
	[K_FDRIVE] = RUN6_(FILTER_DRIVE),
	[K_VOL] = RUN6(VOL),
	[K_PAN] = RUN6(PAN),
	[K_RATE] = RUN6(VOICE_DECIMATION),
	[K_DRIVE] = ROW6(DRIVE1, DRIVE2, DRIVE3, SNARE_DISTORTION, CYMBAL_DISTORTION, HAT_DISTORTION),
};

enum { KICK, TOM, PERC, ZAP, SNARE, CLAP, HAT, CYMBAL, ARCHETYPES };

/* A recipe is a list of three bytes each. R: a key's range. M: four menu choices, the first the
 * usual one, repeats for weight. 0: waves SINE TRI SAW REC NOISE PWM; filters LP HP BP UBP NOTCH
 * PEAK LP2; transients SNAP OFFSET CLICK CLICK2 TICK KICK RIM DROP HAT CLAP KICK2 SNARE TOM SNAP2. */
#define R(key, lo, hi) K_##key, lo, hi
#define M(key, a, b, c, d) K_##key | MENU, (a) | (b) << 4, (c) | (d) << 4
enum { MENU = 0x80, END = 0xff };
static const uint8_t recipes[] = {
	/* KICK */
	M(WAVE, 0, 0, 0, 1), R(COARSE, 24, 36), R(FINE, 58, 68), R(P_AMT, 45, 110), R(P_DECAY, 12, 40), R(P_SLOPE, 10, 60),
	R(ATTACK, 0, 2), R(DECAY, 16, 34), R(SLOPE, 20, 70), R(FM_AMT, 0, 22), R(FM_FREQ, 30, 70), M(FM_WAVE, 0, 1, 0, 1),
	M(T_WAVE, 5, 0, 10, 2), R(T_VOL, 20, 80), R(T_FREQ, 25, 60), R(CUTOFF, 80, 127), R(RESO, 0, 30), M(FTYPE, 0, 0, 6, 0),
	R(DRIVE, 0, 40), R(VOL, 110, 127), END,
	/* TOM */
	M(WAVE, 0, 0, 1, 0), R(COARSE, 38, 60), R(FINE, 55, 72), R(P_AMT, 18, 60), R(P_DECAY, 16, 45), R(P_SLOPE, 10, 50),
	R(ATTACK, 0, 2), R(DECAY, 14, 28), R(SLOPE, 10, 50), R(FM_AMT, 0, 40), R(FM_FREQ, 40, 90), M(FM_WAVE, 0, 1, 2, 0),
	M(FM_MODE, 0, 0, 1, 0), M(T_WAVE, 12, 0, 2, 6), R(T_VOL, 10, 60), R(T_FREQ, 25, 70), R(CUTOFF, 75, 127), R(RESO, 0, 50),
	M(FTYPE, 0, 0, 6, 0), R(DRIVE, 0, 35), R(VOL, 100, 125), END,
	/* PERC */
	M(WAVE, 0, 1, 3, 5), R(COARSE, 58, 90), R(FINE, 40, 88), R(PWM, 10, 110), R(P_AMT, 0, 50), R(P_DECAY, 4, 25),
	R(P_SLOPE, 0, 60), R(ATTACK, 0, 3), R(DECAY, 5, 16), R(SLOPE, 0, 40), R(FM_AMT, 30, 110), R(FM_FREQ, 50, 110),
	M(FM_WAVE, 0, 1, 3, 4), M(FM_MODE, 0, 0, 1, 0), M(T_WAVE, 4, 6, 2, 9), R(T_VOL, 10, 70), R(T_FREQ, 30, 100),
	R(CUTOFF, 60, 127), R(RESO, 0, 80), M(FTYPE, 0, 1, 2, 5), R(DRIVE, 0, 60), R(VOL, 100, 125), END,
	/* ZAP */
	M(WAVE, 0, 2, 3, 5), R(COARSE, 70, 105), R(FINE, 30, 96), R(PWM, 0, 127), R(P_AMT, 80, 127), R(P_DECAY, 5, 22),
	R(P_SLOPE, 0, 80), R(ATTACK, 0, 4), R(DECAY, 6, 20), R(SLOPE, 0, 50), R(FM_AMT, 40, 127), R(FM_FREQ, 20, 110),
	M(FM_WAVE, 0, 2, 3, 4), M(FM_MODE, 0, 1, 0, 1), M(T_WAVE, 0, 7, 13, 0), R(T_VOL, 20, 100), R(T_FREQ, 20, 110),
	R(CUTOFF, 50, 127), R(RESO, 20, 100), M(FTYPE, 0, 2, 3, 5), R(DRIVE, 10, 90), R(VOL, 96, 120), END,
	/* SNARE */
	M(WAVE, 1, 0, 1, 1), R(COARSE, 42, 62), R(FINE, 50, 76), R(NOISE_FREQ, 90, 127), R(NOISE_MIX, 92, 127), R(P_AMT, 20, 80),
	R(P_DECAY, 8, 30), R(P_SLOPE, 0, 40), R(ATTACK, 0, 2), R(DECAY, 8, 20), R(SLOPE, 0, 40), R(REPEAT, 0, 0),
	M(T_WAVE, 11, 0, 2, 6), R(T_VOL, 20, 80), R(T_FREQ, 30, 70), R(CUTOFF, 85, 127), R(RESO, 0, 50), M(FTYPE, 0, 1, 2, 3),
	R(DRIVE, 10, 60), R(VOL, 105, 127), END,
	/* CLAP */
	M(WAVE, 0, 1, 0, 1), R(COARSE, 50, 70), R(NOISE_FREQ, 105, 127), R(NOISE_MIX, 100, 127), R(P_AMT, 0, 30), R(P_DECAY, 6, 20),
	R(ATTACK, 0, 2), R(DECAY, 9, 18), R(SLOPE, 0, 30), R(REPEAT, 3, 6), M(T_WAVE, 9, 0, 2, 9), R(T_VOL, 10, 60),
	R(T_FREQ, 30, 70), R(CUTOFF, 60, 110), R(RESO, 10, 60), M(FTYPE, 2, 3, 1, 2), R(DRIVE, 10, 50), R(VOL, 110, 127), END,
	/* HAT */
	M(WAVE, 3, 0, 2, 5), R(COARSE, 70, 110), R(FINE, 40, 88), R(PWM, 20, 100), M(M1_WAVE, 3, 0, 2, 5), R(M1_FREQ, 70, 127),
	R(M1_AMT, 60, 127), M(M2_WAVE, 4, 4, 3, 4), R(M2_FREQ, 80, 127), R(M2_AMT, 70, 127), R(ATTACK, 0, 1), R(DECAY, 2, 8),
	R(DECAY_OPEN, 12, 32), R(SLOPE, 0, 30), M(T_WAVE, 8, 4, 0, 2), R(T_VOL, 0, 50), R(T_FREQ, 40, 100), R(CUTOFF, 70, 118),
	R(RESO, 0, 60), M(FTYPE, 1, 1, 2, 3), R(DRIVE, 0, 40), R(VOL, 100, 125), END,
	/* CYMBAL */
	M(WAVE, 3, 4, 2, 5), R(COARSE, 60, 112), R(FINE, 40, 88), R(PWM, 20, 100), M(M1_WAVE, 3, 0, 2, 4), R(M1_FREQ, 60, 127),
	R(M1_AMT, 50, 127), M(M2_WAVE, 4, 3, 0, 4), R(M2_FREQ, 60, 127), R(M2_AMT, 50, 127), R(ATTACK, 0, 3), R(DECAY, 40, 80),
	R(SLOPE, 0, 40), R(REPEAT, 0, 0), M(T_WAVE, 8, 4, 0, 8), R(T_VOL, 0, 50), R(T_FREQ, 40, 100), R(CUTOFF, 55, 110),
	R(RESO, 0, 60), M(FTYPE, 1, 1, 2, 3), R(DRIVE, 0, 40), R(VOL, 96, 120), END,
};

/* How a character leans every archetype. decay and fm in percent; rate 127 is no decimation. */
struct character {
	uint8_t wild, decay, drive, fm, rate;
	int8_t bright, tune;
	uint8_t reso;
};
static const struct character characters[POCKET_KIT_CHARACTERS] = {
	/*               wild decay drive  fm rate bright tune reso */
	/* CLASSIC    */ { 30,   90,    0,  60, 127,    0,   0,   0 },
	/* ANALOG     */ { 35,  125,    8,  50, 127,   -5,  -3,   0 },
	/* DIGITAL    */ { 45,   80,    5, 130, 100,   10,   0,   0 },
	/* INDUSTRIAL */ { 55,  110,   45, 140,  90,    0,   0,  25 },
	/* MINIMAL    */ { 20,   60,    0,  40, 127,   -5,   0,   0 },
	/* GLITCH     */ { 65,   60,   20, 150,  40,    5,   5,   0 },
	/* TECHNO     */ { 40,  100,   20,  90, 127,    0,  -2,  10 },
	/* ELECTRO    */ { 40,  125,    6, 110, 127,    0,  -3,   0 },
	/* DUB        */ { 35,  140,    5,  70, 127,  -20,  -4,   0 },
	/* NOISE      */ { 70,   90,   30, 120,  70,   10,   0,  15 },
	/* METALLIC   */ { 60,  100,   10, 160, 127,   15,   5,  30 },
	/* SOFT       */ { 15,  110,    0,  30, 127,  -20,   0,   0 },
	/* BROKEN     */ { 60,   70,   25, 120,  60,    0,   0,  10 },
	/* ACID       */ { 45,   90,   30, 100, 127,   -5,   0,  45 },
	/* LO-FI      */ { 40,  100,   10,  80,  35,  -25,   0,   0 },
	/* CHAOS      */ { 95,  100,   30, 170,  30,    0,   0,  20 },
};
static const char* const names[POCKET_KIT_CHARACTERS] = {
	"CLASSIC", "ANALOG", "DIGITAL", "INDUSTRIAL", "MINIMAL", "GLITCH", "TECHNO", "ELECTRO",
	"DUB", "NOISE", "METALLIC", "SOFT", "BROKEN", "ACID", "LO-FI", "CHAOS"
};

const char* pocket_kit_name(unsigned character)
{
	return names[character < POCKET_KIT_CHARACTERS ? character : 0];
}

static const uint8_t* recipe_of(unsigned archetype)
{
	const uint8_t* r = recipes;
	while (archetype--)
	{
		while (*r != END) r += 3;
		r++;
	}
	return r;
}

/* One value of a recipe entry at a WILDNESS. */
static unsigned draw(const uint8_t* e, uint32_t* rng, unsigned wild, int open_menus)
{
	const unsigned key = e[0] & (MENU - 1u);
	if (e[0] & MENU)
	{
		if (open_menus && wild > 65u && pocket_gen_below(rng, 100) < (wild - 65u) * 17u / 10u)
			return pocket_gen_below(rng, key == K_T_WAVE ? 14 : key == K_FTYPE ? 7 : key == K_FM_MODE ? 2 : 6);
		const unsigned pick = wild < 35u ? 0 : pocket_gen_below(rng, 4), byte = e[1 + pick / 2u];
		return pick & 1u ? byte >> 4 : byte & 15u;
	}
	const unsigned width = key >= K_ATTACK ? 256u : wild < 50u ? 102u + 3u * wild : 256u + (wild - 50u) * 46u / 10u;
	const int h = (int)((unsigned)(e[2] - e[1]) * width >> 9);
	const int v = (e[1] + e[2]) / 2 - h + (int)pocket_gen_below(rng, 2u * (unsigned)h + 1u);
	return (unsigned)(v < 0 ? 0 : v > 127 ? 127 : v);
}

static unsigned limited(int v, int lo, int hi)
{
	return (unsigned)(v < lo ? lo : v > hi ? hi : v);
}

static unsigned chance(uint32_t* rng, unsigned percent)
{
	return pocket_gen_below(rng, 100) < percent;
}

/* The archetype of a voice in this kit. High WILDNESS lets it become another. */
static unsigned archetype_of(unsigned voice, uint32_t family, uint32_t* rng, unsigned wild)
{
	static const uint8_t first[POCKET_LXR_VOICES] = { KICK, TOM, TOM, SNARE, CYMBAL, HAT };
	unsigned a = first[voice];
	if (voice == 2 && family >> 21 & 1u) a = PERC;  /* the high tom's voice also plays percussion */
	if (voice == 3 && !(family >> 22 & 3u)) a = CLAP;
	if (voice && wild > 70u && chance(rng, (wild - 70u) * 3u / 2u))
		a = voice < 3 ? TOM + pocket_gen_below(rng, 3) : voice == 3 ? SNARE + pocket_gen_below(rng, 2) : HAT + pocket_gen_below(rng, 2);
	return a;
}

/* Minor pentatonic above the kit's root: the distance to the nearest note, by semitone. */
static const int8_t pentatonic[12] = { 0, -1, 1, 0, 1, 0, 1, 0, -1, 1, 0, -1 };

void pocket_kit_voice(uint8_t* kit, unsigned character, uint32_t seed, unsigned wildness, unsigned voice, unsigned take)
{
	const struct character* c = &characters[character < POCKET_KIT_CHARACTERS ? character : 0];
	const unsigned wild = limited((int)c->wild + (int)wildness - POCKET_KIT_WILD_DEFAULT, 0, 100);
	/* the family: the same for every voice and every take of a kit */
	const uint32_t family = pocket_gen_mix(seed, 0x6b1u);
	const int root = (int)(family % 12u), length = 88 + (int)(family >> 8 & 31u);
	const int bright = (int)(family >> 13 & 15u) - 7 + c->bright, dirt = (int)(family >> 17 & 15u);
	uint32_t rng = pocket_gen_mix(pocket_gen_mix(seed, voice + 1u), take);
	const unsigned archetype = archetype_of(voice, family, &rng, wild);

	/* what a recipe does not mention stays as the init kit has it, so nothing of an earlier
	   sound survives in this one */
	uint8_t v[KEYS];
	for (unsigned k = 0; k < KEYS; k++) v[k] = (uint8_t)pocket_lxr_init_kit_value(keyParam[k][voice]);
	v[K_PAN] = 63;
	v[K_RATE] = 127;
	v[K_FDRIVE] = 0;
	for (const uint8_t* e = recipe_of(archetype); *e != END; e += 3) v[*e & (MENU - 1u)] = (uint8_t)draw(e, &rng, wild, voice != 0);

	/* the character */
	v[K_DECAY] = (uint8_t)limited(v[K_DECAY] * c->decay * length / 10000, 1, 90);
	v[K_DECAY_OPEN] = (uint8_t)limited(v[K_DECAY_OPEN] * c->decay * length / 10000, 1, 90);
	v[K_DRIVE] = (uint8_t)limited(v[K_DRIVE] + (c->drive ? (int)pocket_gen_below(&rng, c->drive) : 0) + dirt, 0, 110);
	v[K_FM_AMT] = (uint8_t)limited(v[K_FM_AMT] * c->fm / 100, 0, 127);
	v[K_M1_AMT] = (uint8_t)limited(v[K_M1_AMT] * c->fm / 100, 0, 127);
	v[K_M2_AMT] = (uint8_t)limited(v[K_M2_AMT] * c->fm / 100, 0, 127);
	v[K_RESO] = (uint8_t)limited(v[K_RESO] + c->reso, 0, 100);
	v[K_CUTOFF] = (uint8_t)limited(v[K_CUTOFF] + bright, 20, 127);
	if (c->rate < 127u && chance(&rng, 40u + wild / 2u)) v[K_RATE] = (uint8_t)(c->rate + pocket_gen_below(&rng, 128u - c->rate));
	if (c->drive > 15u && chance(&rng, 50)) v[K_FDRIVE] = (uint8_t)pocket_gen_below(&rng, 60);
	if (voice < 3u)
	{
		/* the two toms are two pitches; the drums of a kit sit on one scale */
		int coarse = v[K_COARSE] + c->tune + (archetype == TOM ? (voice == 2u ? 6 : voice == 1u ? -4 : 0) : 0);
		coarse = (int)limited(coarse, 12, 115);
		v[K_COARSE] = (uint8_t)(coarse + pentatonic[(coarse + 120 - root) % 12]);
	}
	/* Three ways to design silence, which CLATTER finds by rendering every voice and here cannot
	   happen: a low-pass over a noise voice, a high-pass over a drum, a pulse with no width */
	if ((archetype == SNARE || archetype == CLAP) && (v[K_FTYPE] == 0 || v[K_FTYPE] == 6) && v[K_CUTOFF] < 100u) v[K_CUTOFF] = 100;
	if (voice < 3u && v[K_FTYPE] == 1u && v[K_CUTOFF] > 50u) v[K_CUTOFF] = 50;
	if (v[K_PWM] < 20u) v[K_PWM] = (uint8_t)(20u + pocket_gen_below(&rng, 80));
	/* swells, sirens and rolls */
	if (wild > 60u)
	{
		const unsigned p = (wild - 60u) * 3u / 4u;
		if (voice && chance(&rng, p)) v[K_ATTACK] = (uint8_t)(3u + pocket_gen_below(&rng, 14));
		if (chance(&rng, p))
		{
			v[K_P_AMT] = (uint8_t)(90u + pocket_gen_below(&rng, 38));
			v[K_P_DECAY] = (uint8_t)(40u + pocket_gen_below(&rng, 51));
		}
		if (archetype != CLAP && chance(&rng, p * 4u / 5u)) v[K_REPEAT] = (uint8_t)(2u + pocket_gen_below(&rng, 7));
		if (chance(&rng, p * 4u / 5u)) v[K_SLOPE] = (uint8_t)pocket_gen_below(&rng, 128);
	}
	for (unsigned k = 0; k < KEYS; k++)
		if (keyParam[k][voice]) kit[keyParam[k][voice]] = v[k];
}

void pocket_kit_generate(uint8_t* kit, unsigned character, uint32_t seed, unsigned wildness)
{
	for (unsigned voice = 0; voice < POCKET_LXR_VOICES; voice++) pocket_kit_voice(kit, character, seed, wildness, voice, 0);
}
