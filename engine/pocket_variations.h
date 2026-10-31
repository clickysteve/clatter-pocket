/*
 * pocket_variations.h - what a variation does to a hit: the sounds on banks two and three of the
 * DRUM keys, and the two further sounds on the voices of DRUM 3 and SNARE (docs/POCKET-CONTROLS.md).
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Offsets from the kit, not values: a variation stays related to whatever the kit has made of the
 * voice, and PERC and CLAP cannot come out the same as the sound they share a voice with. A is a
 * moderate alternative, B goes further. Read in the audio interrupt: included by pocket_lxr.c
 * alone, so it is placed in SRAM with it.
 */
#ifndef POCKET_VARIATIONS_H
#define POCKET_VARIATIONS_H

#define VARIATION_LOCKS 4
#define VARIATION_LONGEST 100 /* of a decay: 126 lasts minutes, and a hit must end */
#define VARIATION_SHORTEST 2  /* and 0 is silence, which is no variation */
#define VARIATION_SLOWEST 16  /* of a decimator: near 0 it holds a sample for seconds */
#define L(name, by) { POCKET_PAR_##name, by }

struct variation_lock {
	uint8_t param;
	int8_t by;
};

/* The first row of each trigger: two for most, A and B, and five for DRUM 3 and SNARE. */
static const uint8_t variationRow[POCKET_LXR_TRIGGERS] = { 0, 2, 4, 9, 14, 16, 18 };

static const struct variation_lock variations[20][VARIATION_LOCKS] = {
	/* DRUM 1 */
	{ L(VELOD1, -5), L(MODAMNT1, 24), L(DRIVE1, 18), L(COARSE1, 3) },            /* A: tight, clicky */
	{ L(VELOD1, 22), L(COARSE1, -6), L(MODAMNT1, 40), L(DRIVE1, 48) },           /* B: long, driven */
	/* DRUM 2 */
	{ L(COARSE2, 5), L(VELOD2, -4), L(MODAMNT2, 16) },                           /* A: higher, shorter */
	{ L(FMAMNT2, 64), L(VELOD2, 14), L(DRIVE2, 40), L(COARSE2, -3) },            /* B: FM, longer */
	/* DRUM 3 */
	{ L(COARSE3, 5), L(VELOD3, -4), L(MODAMNT3, 16) },
	{ L(FMAMNT3, 64), L(VELOD3, 14), L(DRIVE3, 40), L(COARSE3, -3) },
	{ L(COARSE3, 19), L(VELOD3, -10), L(FMAMNT3, 48) },                          /* PERC: small, bright */
	{ L(COARSE3, 26), L(VELOD3, -12), L(FMAMNT3, 80), L(MODAMNT3, 30) },         /* PERC A */
	{ L(COARSE3, 12), L(VELOD3, -8), L(FMAMNT3, 110), L(VOICE_DECIMATION3, -70) }, /* PERC B: crushed */
	/* SNARE */
	{ L(VELOD4, -4), L(COARSE4, 4), L(SNARE_DISTORTION, 18) },                   /* A: tight, higher */
	{ L(VELOD4, 12), L(SNARE_DISTORTION, 60), L(VOICE_DECIMATION4, -50), L(NOISE_FREQ1, -20) }, /* B: dirty */
	{ L(REPEAT4, 4), L(MIX1, 40), L(FILTER_FREQ_4, -14), L(RESO_4, 30) },        /* CLAP: noise, repeats */
	{ L(REPEAT4, 6), L(MIX1, 40), L(VELOD4, 6), L(FILTER_FREQ_4, -28) },         /* CLAP A: wider */
	{ L(REPEAT4, 8), L(MIX1, 40), L(VOICE_DECIMATION4, -70), L(SNARE_DISTORTION, 50) }, /* CLAP B: FX */
	/* CYMBAL */
	{ L(VELOD5, -20), L(FILTER_FREQ_5, 12), L(COARSE5, 5) },                     /* A: choked */
	{ L(VELOD5, 10), L(COARSE5, -12), L(CYMBAL_DISTORTION, 50), L(VOICE_DECIMATION5, -50) }, /* B: gong */
	/* CL HAT */
	{ L(FILTER_FREQ_6, -24), L(VELOD6_CLOSED, 3) },                              /* A: darker */
	{ L(COARSE6, 14), L(RESO_6, 45), L(HAT_DISTORTION, 30) },                    /* B: metal */
	/* OP HAT */
	{ L(VELOD6_OPEN, -10), L(FILTER_FREQ_6, -18) },                              /* A: shorter, darker */
	{ L(VELOD6_OPEN, 14), L(HAT_DISTORTION, 50), L(VOICE_DECIMATION6, -45) },    /* B: long, crushed */
};

#undef L
#endif
