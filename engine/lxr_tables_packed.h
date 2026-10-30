/*
 * lxr_tables_packed.h - the LXR's constant tables, losslessly packed (engine/lxr_tables_unpack.c,
 * tools/pack-lxr-tables.py).
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef LXR_TABLES_PACKED_H
#define LXR_TABLES_PACKED_H
#include <stddef.h>
#include <stdint.h>

#define LXR_PACK_PROB_BITS 12u
#define LXR_PACK_RATE 4u
#define LXR_PACK_LENGTHS 19u   /* bit lengths of a zigzag value, 0..18 */
#define LXR_PACK_SIZES 12u     /* contexts from the size of the last two values */

struct lxr_packed_table
{
	const uint8_t* data;
	uint32_t bytes;   /* packed size */
	uint32_t count;   /* values */
	uint8_t width;    /* bytes per value: 2 int16, 1 int8 */
	uint8_t order;    /* predictor order */
};

extern const struct lxr_packed_table lxr_packed_sine_table, lxr_packed_triTable, lxr_packed_sawTable,
	lxr_packed_recTable, lxr_packed_transientData;
extern const uint32_t lxr_packed_crc; /* CRC-32 of all five tables as the firmware has them */


/* Unpack every table into RAM (sine_table, triTable, sawTable, recTable, transientData).
   Returns 0, or -1 if the result is not the tables, byte for byte. Call once, before the engine runs. */
int pocket_lxr_tables_unpack(void);

#endif
