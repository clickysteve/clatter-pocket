/*
 * pocket_gen.h - CLATTER Pocket's pattern generator and mutation, ported from browser CLATTER
 * (src/rhythm/generate.ts, styles.ts, mutate.ts). See docs/POCKET-SEQUENCER.md.
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Control context only. Integer arithmetic and keyed hashes: the same inputs give the same
 * pattern on any platform, and a decision depends only on (seed, purpose, track, step), so
 * raising DENSITY adds hits to a rhythm instead of drawing another one.
 */
#ifndef POCKET_GEN_H
#define POCKET_GEN_H

#include <stdint.h>
#include "pocket_seq.h"

#ifdef __cplusplus
extern "C" {
#endif

#define POCKET_GEN_STYLES 11

enum { POCKET_MUTATE_SMALL, POCKET_MUTATE_MEDIUM, POCKET_MUTATE_LARGE, POCKET_MUTATE_STRENGTHS };

struct pocket_gen_settings {
	uint32_t seed;
	uint8_t density;    /* 0-100: how busy; 50 is the style as written */
	uint8_t complexity; /* 0-100: how unpredictable */
	uint8_t style;      /* below POCKET_GEN_STYLES */
	uint8_t locks;      /* tracks never touched, bit per track */
};

const char* pocket_gen_style_name(unsigned style);
/* Replace every unlocked track of `pattern`. */
void pocket_gen_generate(struct pocket_pattern* pattern, const struct pocket_gen_settings* settings);
/* Musical edits to the unlocked tracks of `pattern`. Returns how many were made. */
unsigned pocket_gen_mutate(struct pocket_pattern* pattern, uint32_t seed, unsigned strength, unsigned locks);

#ifdef __cplusplus
}
#endif

#endif
