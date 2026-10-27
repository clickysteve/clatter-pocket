/*
 * fx_on.c - companion of the firmware's FX bus (FxBus.c and DSPAudio/FX/) in full builds.
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Clears the FX bus's delay memory on (re)initialisation, as the STM32 startup code zeroes
 * DTCM (FX_BUFFER is INDTCMZ) and as CLATTER's clatter_fx_clear does. Builds without the FX
 * bus link fx_off.c instead.
 */
#include "Soundengine/DSPAudio/FX/FxRamBuffer.h"

void pocket_fx_reset(void)
{
	for (int i = 0; i <= FX_BUFFER_SIZE; i++) FX_BUFFER[i] = 0;
}
