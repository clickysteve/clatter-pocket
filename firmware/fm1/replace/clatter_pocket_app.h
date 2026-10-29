/*
 * clatter_pocket_app.h - CLATTER Pocket as the FM-1's instrument (build.sh --replace).
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 */
#ifndef CLATTER_POCKET_APP_H
#define CLATTER_POCKET_APP_H

#include <stdint.h>
#include "fm1.h"

/* Audio interrupt: fill one FM1_BLOCK_FRAMES block of signed Q23 stereo. Returns 0. */
FM1_AUDIO_CODE int clatter_pocket_render(int32_t* output);
/* Audio interrupt: the MASTER knob's output level, 0..127 (as fm1_engine_output_level). */
FM1_AUDIO_CODE unsigned clatter_pocket_output_level(void);

#endif
