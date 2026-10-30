/*
 * lxr_tables_unpack.c - the LXR firmware's constant tables, unpacked into RAM at start-up.
 *
 * Builds with POCKET_PACKED_TABLES (the FM-1 build) compile this file and
 * engine/lxr_tables_packed.c instead of the firmware's wavetable.c and transientTables.c.
 * The tables keep their firmware names and layout (sine_table, triTable, sawTable,
 * recTable, transientData; transientVolumeTable is stored verbatim) and are byte-identical
 * after unpacking: their CRC is checked here at every start, make test compares every byte, and
 * the engine tests run on this variant too. The firmware's files declare them const; here they are written once,
 * by pocket_lxr_tables_unpack(), before the engine reads them.
 *
 * Format (tools/pack-lxr-tables.py): per table a predictor order p (0: x, 1: x - x[-1],
 * 2: x - 2x[-1] + x[-2], over the whole table in memory order); residuals zigzag-coded, then
 * through an LZMA-style binary range coder: the value's bit length in unary, in a context set by
 * the size of the last two values, the first bit below the leading one modelled by length, the
 * rest raw.
 * The sawtooth is exactly odd in every row, x[1024 - i] = -x[i], so only the first SAW_KEPT
 * values of each row are stored, row after row.
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
	uint32_t range, code;
};

/* the models, here rather than on the stack: the engine's start is the deepest control path */
static uint16_t probs[LXR_PACK_SIZES * LXR_PACK_LENGTHS + LXR_PACK_LENGTHS];

static void normalise(struct reader* r)
{
	while (r->range < 1u << 24)
	{
		r->range <<= 8;
		r->code = r->code << 8 | *r->p++;
	}
}

static unsigned modelled(struct reader* r, uint16_t* prob)
{
	const uint32_t bound = (r->range >> LXR_PACK_PROB_BITS) * *prob;
	const unsigned b = r->code < bound;
	if (b)
	{
		r->range = bound;
		*prob += (uint16_t)(((1u << LXR_PACK_PROB_BITS) - *prob) >> LXR_PACK_RATE);
	}
	else
	{
		r->code -= bound;
		r->range -= bound;
		*prob -= *prob >> LXR_PACK_RATE;
	}
	normalise(r);
	return b;
}

static unsigned raw(struct reader* r)
{
	r->range >>= 1;
	const unsigned b = r->code >= r->range;
	if (b) r->code -= r->range;
	normalise(r);
	return b;
}

static unsigned bit_length(uint32_t z)
{
	unsigned n = 0;
	for (; z; z >>= 1) n++;
	return n;
}

/* Unpacks one table of n values; width 2 (int16) or 1 (int8). Returns 0 or -1. */
static int unpack(const struct lxr_packed_table* t, void* out, size_t n, unsigned width)
{
	if (t->count != n || t->width != width) return -1;
	for (unsigned i = 0; i < sizeof probs / sizeof *probs; i++) probs[i] = 1u << (LXR_PACK_PROB_BITS - 1);
	struct reader r = { t->data, 0xffffffffu, 0 };
	for (unsigned i = 0; i < 4; i++) r.code = r.code << 8 | *r.p++;
	int32_t x1 = 0, x2 = 0;
	uint32_t z1 = 0, z2 = 0;
	for (size_t i = 0; i < n; i++)
	{
		const unsigned size = bit_length(2 * z1 + z2);
		uint16_t* lengths = &probs[(size < LXR_PACK_SIZES ? size : LXR_PACK_SIZES - 1u) * LXR_PACK_LENGTHS];
		unsigned length = 0;
		while (length < LXR_PACK_LENGTHS - 1u && modelled(&r, &lengths[length])) length++;
		uint32_t z = length ? 1u << (length - 1u) : 0;
		if (length > 1u)
		{
			z |= (uint32_t)modelled(&r, &probs[LXR_PACK_SIZES * LXR_PACK_LENGTHS + length]) << (length - 2u);
			for (unsigned j = length - 2u; j--;) z |= (uint32_t)raw(&r) << j;
		}
		z2 = z1;
		z1 = z;
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

uint32_t pocket_crc32(uint32_t crc, const void* data, size_t bytes)
{
	const uint8_t* p = data;
	crc = ~crc;
	while (bytes--)
	{
		crc ^= *p++;
		for (unsigned i = 0; i < 8; i++) crc = crc >> 1 ^ (0xedb88320u & -(crc & 1u));
	}
	return ~crc;
}

#define SAW_ROWS 11
#define SAW_KEPT 513

/* The packed rows sit one after another at the start of the table. Each moves up to its place
 * and is mirrored, last row first, so that nothing is written before it has been read. */
static void saw_expand(void)
{
	const int16_t* packed = &sawTable[0][0];
	for (int r = SAW_ROWS - 1; r >= 0; r--)
	{
		for (int i = SAW_KEPT - 1; i >= 0; i--) sawTable[r][i] = packed[r * SAW_KEPT + i];
		for (int i = 1; i < SAW_KEPT - 1; i++) sawTable[r][1024 - i] = (int16_t)-sawTable[r][i];
	}
}

int pocket_lxr_tables_unpack(void)
{
	int bad = 0;
	bad |= unpack(&lxr_packed_sine_table, sine_table, sizeof sine_table / 2, 2);
	bad |= unpack(&lxr_packed_triTable, triTable, sizeof triTable / 2, 2);
	bad |= unpack(&lxr_packed_sawTable, sawTable, SAW_ROWS * SAW_KEPT, 2);
	saw_expand();
	bad |= unpack(&lxr_packed_recTable, recTable, sizeof recTable / 2, 2);
	bad |= unpack(&lxr_packed_transientData, transientData, sizeof transientData, 1);
	if (bad) return bad;
	uint32_t crc = pocket_crc32(0, sine_table, sizeof sine_table);
	crc = pocket_crc32(crc, triTable, sizeof triTable);
	crc = pocket_crc32(crc, sawTable, sizeof sawTable);
	crc = pocket_crc32(crc, recTable, sizeof recTable);
	crc = pocket_crc32(crc, transientData, sizeof transientData);
	return crc == lxr_packed_crc ? 0 : -1;
}
