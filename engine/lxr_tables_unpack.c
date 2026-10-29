/*
 * lxr_tables_unpack.c - the LXR firmware's constant tables, unpacked into RAM at start-up.
 *
 * Builds with POCKET_PACKED_TABLES (the FM-1 build) compile this file and
 * engine/lxr_tables_packed.c instead of the firmware's wavetable.c and transientTables.c.
 * The tables keep their firmware names and layout (sine_table, triTable, sawTable,
 * recTable, transientData; transientVolumeTable is stored verbatim) and are byte-identical
 * after unpacking: host/tests/test_lxr_tables.c checks every byte, and the engine tests run
 * on this variant too. The firmware's files declare them const; here they are written once,
 * by pocket_lxr_tables_unpack(), before the engine reads them.
 *
 * Format (tools/pack-lxr-tables.py): per table a predictor order p (0: x, 1: x - x[-1],
 * 2: x - 2x[-1] + x[-2], over the whole table in memory order); residuals zigzag-coded and
 * Rice-coded in blocks of 64 with a 4-bit parameter k per block; a quotient of ESCAPE or more
 * is written as ESCAPE ones and the zigzag value in 18 raw bits. Bits are read LSB first.
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <stddef.h>
#include <stdint.h>
#include "lxr_tables_packed.h"

int16_t sine_table[4097];
int16_t triTable[11][1024];
int16_t sawTable[11][1024];
int16_t recTable[11][1024];
int8_t transientData[12][2205];

struct reader
{
	const uint8_t* p;
	uint32_t bits;
	unsigned count;
};

static unsigned bit(struct reader* r)
{
	if (!r->count)
	{
		r->bits = *r->p++;
		r->count = 8;
	}
	const unsigned b = r->bits & 1u;
	r->bits >>= 1;
	r->count--;
	return b;
}

static uint32_t bits(struct reader* r, unsigned n)
{
	uint32_t v = 0;
	for (unsigned i = 0; i < n; i++) v |= (uint32_t)bit(r) << i;
	return v;
}

/* Unpacks one table of n values; width 2 (int16) or 1 (int8). Returns 0 or -1. */
static int unpack(const struct lxr_packed_table* t, void* out, size_t n, unsigned width)
{
	if (t->count != n || t->width != width) return -1;
	struct reader r = {t->data, 0, 0};
	int32_t x1 = 0, x2 = 0;
	unsigned k = 0;
	for (size_t i = 0; i < n; i++)
	{
		if (!(i % LXR_PACK_BLOCK)) k = bits(&r, 4);
		uint32_t q = 0;
		while (q < LXR_PACK_ESCAPE && bit(&r)) q++;
		const uint32_t z = q == LXR_PACK_ESCAPE ? bits(&r, 18) : (q << k) | bits(&r, k);
		const int32_t residual = (int32_t)(z >> 1) ^ -(int32_t)(z & 1u);
		const int32_t predicted = t->order == 2 ? 2 * x1 - x2 : t->order == 1 ? x1 : 0;
		const int32_t x = predicted + residual;
		if (width == 2) ((int16_t*)out)[i] = (int16_t)x;
		else ((int8_t*)out)[i] = (int8_t)x;
		x2 = x1;
		x1 = x;
	}
	return (size_t)(r.p - t->data) == t->bytes ? 0 : -1;
}

int pocket_lxr_tables_unpack(void)
{
	int bad = 0;
	bad |= unpack(&lxr_packed_sine_table, sine_table, sizeof sine_table / 2, 2);
	bad |= unpack(&lxr_packed_triTable, triTable, sizeof triTable / 2, 2);
	bad |= unpack(&lxr_packed_sawTable, sawTable, sizeof sawTable / 2, 2);
	bad |= unpack(&lxr_packed_recTable, recTable, sizeof recTable / 2, 2);
	bad |= unpack(&lxr_packed_transientData, transientData, sizeof transientData, 1);
	return bad;
}
