/*
 * dump_tables.c - writes the LXR firmware's constant tables, as compiled, to one binary file
 * for tools/pack-lxr-tables.py: sine_table, triTable, sawTable, recTable (int16, little
 * endian), transientData (int8), transientVolumeTable (float32 bit patterns).
 *
 *   build/host/dump_tables build/host/lxr_tables.bin
 *
 * Built with POCKET_PACKED_TABLES against the packed engine, it unpacks first and writes the
 * result, which `make test` compares byte for byte with the original.
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdint.h>
#include <stdio.h>

extern const int16_t sine_table[4097], triTable[11][1024], sawTable[11][1024], recTable[11][1024];
extern const int8_t transientData[12][2205];
extern const float transientVolumeTable[69];
#ifdef POCKET_PACKED_TABLES
int pocket_lxr_tables_unpack(void);
#endif

int main(int argc, char** argv)
{
	if (argc != 2) { fprintf(stderr, "usage: %s out.bin\n", argv[0]); return 2; }
#ifdef POCKET_PACKED_TABLES
	if (pocket_lxr_tables_unpack() != 0) { fprintf(stderr, "packed tables inconsistent\n"); return 1; }
#endif
	FILE* f = fopen(argv[1], "wb");
	if (!f) { perror(argv[1]); return 1; }
	fwrite(sine_table, 1, sizeof sine_table, f);
	fwrite(triTable, 1, sizeof triTable, f);
	fwrite(sawTable, 1, sizeof sawTable, f);
	fwrite(recTable, 1, sizeof recTable, f);
	fwrite(transientData, 1, sizeof transientData, f);
	fwrite(transientVolumeTable, 1, sizeof transientVolumeTable, f);
	return fclose(f) != 0;
}
