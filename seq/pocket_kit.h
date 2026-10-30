/*
 * pocket_kit.h - CLATTER Pocket's drum kits: sets of LXR parameters designed from a seed, a
 * compact port of browser CLATTER's src/sound/kitgen.ts. See docs/POCKET-CONTROLS.md.
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Control context only. No samples and no stored kits: a kit is a function of (character, seed,
 * wildness), so the 16 factory kits cost a row of numbers each. Writes parameters into an array
 * indexed by PAR_* number; applying them to the engine is the caller's.
 */
#ifndef POCKET_KIT_H
#define POCKET_KIT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define POCKET_KIT_CHARACTERS 16
#define POCKET_KIT_WILD_DEFAULT 50 /* the character as written; 0-100 leans it either way */

const char* pocket_kit_name(unsigned character);
/* Design all six voices into `kit`. The same arguments always give the same kit. */
void pocket_kit_generate(uint8_t* kit, unsigned character, uint32_t seed, unsigned wildness);
/* Design one voice (0-5; the hats are one voice) into `kit`, of the same family as the kit that
 * (character, seed) gives: the same character, root and brightness. `take` draws another sound.
 * No other voice's parameters are written. */
void pocket_kit_voice(uint8_t* kit, unsigned character, uint32_t seed, unsigned wildness, unsigned voice, unsigned take);

#ifdef __cplusplus
}
#endif

#endif
