/*
 * pocket_store.c - see pocket_store.h and docs/POCKET-STORAGE.md.
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <stddef.h>
#include "pocket_store.h"
#include "pocket_lxr.h" /* pocket_crc32 */

/* The SDK's persistent user Flash (sdk/include/fm1.h): offsets are within the user partition. The
 * host tests provide the same three on a model of a NOR chip. */
uint32_t fm1_flash_size(void);
int fm1_flash_read(uint32_t offset, void* data, size_t bytes);
int fm1_flash_write(uint32_t offset, const void* data, size_t bytes);
int fm1_flash_erase(uint32_t offset);

#define SECTOR_BYTES 4096u
#define SECTORS 4u
#define PARTITION_BYTES 536576u  /* as the stock layout, or these offsets mean something else */
/* Flash 0xE5000..0xE8FFF: above the stock projects, below V15's BTIF and the vendor's USR area */
#define FIRST_SECTOR 430080u
/* The stock sample bank: its header, and how long it must be to reach FIRST_SECTOR. */
#define SAMPLE_OFFSET 69632u
#define SAMPLE_MAGIC 0x53314d46u
#define SAMPLE_REACH (FIRST_SECTOR - SAMPLE_OFFSET)

#define MAGIC 0x32535043u        /* "CPS2" */
#define FORMAT(page) (2u << 8 | (page))
#define PER_SECTOR 3u
#define PAGES (POCKET_STORE_PROJECTS / PER_SECTOR) /* a page: the three projects of a sector */

struct sector {
	uint32_t magic;
	uint32_t commit;     /* all ones as written, zero once the sector is whole */
	uint32_t crc;        /* of everything after it */
	uint32_t format;     /* version << 8 | page */
	uint32_t generation; /* the highest good copy of a page is current */
	uint8_t settings[POCKET_STORE_SETTINGS_BYTES]; /* reserved, all zero, before 0.9.2 */
	uint8_t projects[PER_SECTOR][POCKET_STORE_PROJECT_BYTES];
	uint8_t padding[2];
};
_Static_assert(sizeof(struct sector) == SECTOR_BYTES, "a record is a sector");
#define CRC_FROM offsetof(struct sector, format)

enum { BLANK, VALID, WRITTEN, OURS, FOREIGN }; /* WRITTEN: whole, not yet committed. OURS: not whole */

static struct sector ram;
static uint8_t states[SECTORS], pageInRam;
static uint32_t generations[SECTORS], formats[SECTORS], newest;
static int8_t current[PAGES]; /* a sector, or -1 */
static int8_t closed;  /* what the last look found, if it found nothing to save to */
static uint8_t settings[POCKET_STORE_SETTINGS_BYTES];

static uint32_t sector_offset(unsigned sector)
{
	return FIRST_SECTOR + sector * SECTOR_BYTES;
}

/* What a sector holds. Every byte is read: a sector is blank only if all of it is. */
static unsigned scan(unsigned sector, uint32_t* found, uint32_t* format)
{
	union { uint32_t words[16]; uint8_t bytes[64]; } chunk; /* on the stack: keep it this small */
	const uint32_t offset = sector_offset(sector);
	uint32_t commit = 0, stored = 0, crc = 0, magic = 0, blank = ~0u;
	for (uint32_t at = 0; at < SECTOR_BYTES; at += sizeof chunk.bytes)
	{
		if (fm1_flash_read(offset + at, chunk.bytes, sizeof chunk.bytes)) return FOREIGN;
		for (unsigned i = 0; i < 16; i++) blank &= chunk.words[i];
		if (!at)
		{
			magic = chunk.words[offsetof(struct sector, magic) / 4];
			commit = chunk.words[offsetof(struct sector, commit) / 4];
			stored = chunk.words[offsetof(struct sector, crc) / 4];
			*format = chunk.words[offsetof(struct sector, format) / 4];
			*found = chunk.words[offsetof(struct sector, generation) / 4];
			crc = pocket_crc32(0, chunk.bytes + CRC_FROM, sizeof chunk.bytes - CRC_FROM);
		}
		else crc = pocket_crc32(crc, chunk.bytes, sizeof chunk.bytes);
	}
	if (blank == ~0u) return BLANK;
	if (magic != MAGIC) return FOREIGN;
	if (crc != stored) return OURS;
	if ((*format >> 8) != 2u || (*format & 255u) >= PAGES) return OURS;
	return commit == 0 ? VALID : commit == ~0u ? WRITTEN : OURS;
}

static int examine(void)
{
	uint32_t sample[3];
	unsigned foreign = 0, bad = 0, valid = 0;
	newest = 0;
	for (unsigned p = 0; p < PAGES; p++) current[p] = -1;
	if (fm1_flash_size() != PARTITION_BYTES) return POCKET_STORE_UNAVAILABLE;
	/* a sample bank can have a blank sector in it, so being blank here does not make it free */
	if (fm1_flash_read(SAMPLE_OFFSET, sample, sizeof sample)) return POCKET_STORE_FAILED;
	if (sample[0] == SAMPLE_MAGIC && sample[2] > SAMPLE_REACH) return POCKET_STORE_SAMPLES;
	for (unsigned s = 0; s < SECTORS; s++)
	{
		states[s] = (uint8_t)scan(s, &generations[s], &formats[s]);
		if (states[s] == VALID)
		{
			const int8_t held = current[formats[s] & 255u];
			valid++;
			if (held < 0 || (int32_t)(generations[s] - generations[held]) > 0) current[formats[s] & 255u] = (int8_t)s;
			if ((int32_t)(generations[s] - newest) > 0) newest = generations[s];
		}
		else if (states[s] == FOREIGN) foreign++;
		else if (states[s] != BLANK) bad++;
	}
	/* Nothing of ours to vouch for them, so a sector that is not ours stays as it is. With a good
	   sector of projects on the chip, it is a save that was cut off in its erase: the first save
	   found all four blank or ours, or was asked to clear them, and the sample bank check above is
	   what would show anyone else had taken one since. */
	if (foreign && !valid) return POCKET_STORE_OCCUPIED;
	/* one bad sector is what a power cut leaves, and nothing was lost that had been saved */
	return bad > 1u || (bad && !valid) ? POCKET_STORE_DAMAGED : POCKET_STORE_OK;
}

int pocket_store_open(void)
{
	const int r = examine();
	closed = (int8_t)(r < 0 ? r : 0);
	return r;
}

void pocket_store_peek(unsigned project, void* data, unsigned bytes)
{
	const int s = current[project / PER_SECTOR];
	uint8_t* d = data;
	if (s < 0 || fm1_flash_read(sector_offset((unsigned)s) + offsetof(struct sector, projects) + project % PER_SECTOR * POCKET_STORE_PROJECT_BYTES, d, bytes))
		while (bytes--) *d++ = 0;
}

uint8_t* pocket_store_project(unsigned project)
{
	const unsigned page = project / PER_SECTOR;
	pageInRam = (uint8_t)page;
	if (current[page] >= 0)
	{
		if (fm1_flash_read(sector_offset((unsigned)current[page]), &ram, sizeof ram)) return 0;
	}
	else
		for (uint8_t* p = (uint8_t*)&ram; p < (uint8_t*)(&ram + 1); p++) *p = 0;
	return ram.projects[project % PER_SECTOR];
}

/* The sector the next save goes to: blank if there is one, or the oldest that nothing depends on.
 * Never a current copy, so never the sector last written. */
static int free_sector(void)
{
	int best = -1;
	for (unsigned s = 0; s < SECTORS; s++)
	{
		int taken = 0;
		for (unsigned p = 0; p < PAGES; p++) taken |= (int)s == current[p];
		if (taken) continue;
		if (states[s] == BLANK) return (int)s;
		if (best < 0 || states[s] != VALID || (states[best] == VALID && (int32_t)(generations[s] - generations[best]) < 0)) best = (int)s;
	}
	return best;
}

int pocket_store_commit(void)
{
	const int target = free_sector();
	const uint32_t committed = 0;
	uint32_t found, format;
	if (closed) return closed;
	if (target < 0) return POCKET_STORE_FAILED;
	const uint32_t offset = sector_offset((unsigned)target);
	if (states[target] != BLANK && fm1_flash_erase(offset)) return POCKET_STORE_FAILED;
	states[target] = OURS; /* whatever follows, it is not blank, and it was ours to erase */
	ram.magic = MAGIC;
	ram.commit = ~0u;
	ram.format = FORMAT(pageInRam);
	ram.generation = newest + 1u;
	for (unsigned i = 0; i < POCKET_STORE_SETTINGS_BYTES; i++) ram.settings[i] = settings[i];
	ram.padding[0] = ram.padding[1] = 0;
	ram.crc = pocket_crc32(0, (const uint8_t*)&ram + CRC_FROM, sizeof ram - CRC_FROM);
	if (fm1_flash_write(offset, &ram, sizeof ram) || scan((unsigned)target, &found, &format) != WRITTEN) return POCKET_STORE_FAILED;
	if (fm1_flash_write(offset + offsetof(struct sector, commit), &committed, sizeof committed)) return POCKET_STORE_FAILED;
	if (scan((unsigned)target, &found, &format) != VALID) return POCKET_STORE_FAILED;
	newest++;
	ram.commit = 0;
	states[target] = VALID;
	generations[target] = newest;
	current[pageInRam] = (int8_t)target;
	return POCKET_STORE_OK;
}

/* No examine(): nothing about where to save is decided here, and the sample bank is not ours to read.
 * A sector of ours is VALID only with our magic, a good CRC and its commit word. */
int pocket_store_settings(uint8_t* data)
{
	uint8_t found[POCKET_STORE_SETTINGS_BYTES];
	uint32_t generation, format, best = 0;
	int newest_sector = -1;
	if (fm1_flash_size() != PARTITION_BYTES) return 0;
	for (unsigned s = 0; s < SECTORS; s++)
		if (scan(s, &generation, &format) == VALID && (newest_sector < 0 || (int32_t)(generation - best) > 0))
		{
			newest_sector = (int)s;
			best = generation;
		}
	if (newest_sector < 0 || fm1_flash_read(sector_offset((unsigned)newest_sector) + offsetof(struct sector, settings), found, sizeof found)) return 0;
	for (unsigned i = 0; i < sizeof found; i++) data[i] = found[i];
	return 1;
}

void pocket_store_set_settings(const uint8_t* data)
{
	for (unsigned i = 0; i < POCKET_STORE_SETTINGS_BYTES; i++) settings[i] = data[i];
}

int pocket_store_commit_settings(void)
{
	const int r = pocket_store_open();
	unsigned page = 0;
	if (r < 0) return r;
	for (unsigned p = 0; p < PAGES; p++)
		if (current[p] >= 0 && generations[current[p]] == newest) page = p;
	if (!pocket_store_project(page * PER_SECTOR)) return POCKET_STORE_FAILED;
	return pocket_store_commit();
}

int pocket_store_claim(void)
{
	if (examine() != POCKET_STORE_OCCUPIED) return POCKET_STORE_FAILED;
	for (unsigned s = 0; s < SECTORS; s++)
		if (states[s] == FOREIGN && fm1_flash_erase(sector_offset(s))) return POCKET_STORE_FAILED;
	return pocket_store_open();
}
