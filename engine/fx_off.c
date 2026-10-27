/*
 * fx_off.c - the LXR FX bus, switched off, for builds without its delay memory.
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * The firmware's FX bus (FxBus.c and DSPAudio/FX/) needs FX_BUFFER: 32768 half floats,
 * 64 KB of delay memory. That is an eighth of the FM-1's internal SRAM, which the stock
 * instrument already shares (HARDWARE.md, ARCHITECTURE.md). Until a memory budget for it
 * is established, the FM-1 build links this file instead: the bus exists (the parameter
 * array points into it and the voices read fxBus.fxType) but never leaves FX_OFF and has no
 * delay memory. FX parameters are still stored in the kit, so kits round-trip unchanged.
 *
 * The voices do not pass through the FX bus, so this changes nothing they render.
 */
#include "Soundengine/FxBus.h"

FxBus fxBus;

void FxBus_init()
{
	fxBus.fxType = FX_OFF;
	fxBus.audioRouting = 0;
	fxBus.dryWet = 0.f;
}

void FxBus_process(int16_t* fxBuffer, int16_t* outL, int16_t* outR, int16_t* outL2, int16_t* outR2, uint32_t size)
{
	(void)fxBuffer; (void)outL; (void)outR; (void)outL2; (void)outR2; (void)size;
}

void FxBus_setParameter(uint8_t idx, float value)
{
	(void)idx; (void)value;
}

void FxBus_updateParameter()
{
	/* the parameter handler may have written fxType: the bus stays off */
	fxBus.fxType = FX_OFF;
}

void FxBus_prepareFxChange()
{
}

/* pocket_lxr.c: there is no delay memory to clear */
void pocket_fx_reset(void)
{
}

void Compressor_setRatio(Compressor* c, uint8_t ratio)
{
	(void)c; (void)ratio;
}

void Ringmod_setWave(Ringmod* r, uint8_t wave)
{
	(void)r; (void)wave;
}
