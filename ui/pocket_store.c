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
#define PARTITION_BYTES 536576u  /* as the stock layout, or these offsets mean something else */
/* Flash 0xE5000..0xE8FFF: above the stock projects, below V15's BTIF and the vendor's USR area */
#define FIRST_SECTOR 430080u
/* The stock sample bank: its header, and how long it must be to reach FIRST_SECTOR. */
#define SAMPLE_OFFSET 69632u
#define SAMPLE_MAGIC 0x53314d46u
#define SAMPLE_REACH (FIRST_SECTOR - SAMPLE_OFFSET)

#define BANK_MAGIC 0x31535043u   /* "CPS1" */
#define BANK_VERSION 1u

struct bank {
	uint32_t magic;
	uint32_t commit;     /* all ones as written, zero once the sector is whole */
	uint32_t crc;        /* of everything after it */
	uint32_t format;     /* version << 8 | kind */
	uint32_t generation; /* the higher of two good banks is current */
	uint32_t reserved[3];
	uint8_t slots[POCKET_STORE_SLOTS][POCKET_STORE_SLOT_BYTES];
};
_Static_assert(sizeof(struct bank) <= SECTOR_BYTES, "one bank per sector");
#define CRC_FROM offsetof(struct bank, format)

enum { BLANK, VALID, WRITTEN, OURS, FOREIGN }; /* WRITTEN: whole, not yet committed. OURS: not whole */

static struct bank bank;
static uint8_t kindOpen, states[2];
static int8_t live;           /* the sector of the bank in RAM, or -1 */
static uint32_t generation;

static uint32_t sector_offset(unsigned kind, unsigned sector)
{
	return FIRST_SECTOR + (2u * kind + sector) * SECTOR_BYTES;
}

/* What a sector holds. Every byte is read: a sector is blank only if all of it is. */
static unsigned scan(unsigned kind, unsigned sector, uint32_t* found)
{
	union { uint32_t words[16]; uint8_t bytes[64]; } chunk; /* on the stack: keep it this small */
	const uint32_t offset = sector_offset(kind, sector);
	uint32_t commit = 0, stored = 0, crc = 0, ours = 0, format = 0, blank = ~0u;
	for (uint32_t at = 0; at < SECTOR_BYTES; at += sizeof chunk.bytes)
	{
		if (fm1_flash_read(offset + at, chunk.bytes, sizeof chunk.bytes)) return FOREIGN;
		for (unsigned i = 0; i < 16; i++) blank &= chunk.words[i];
		if (!at)
		{
			ours = chunk.words[offsetof(struct bank, magic) / 4] == BANK_MAGIC;
			commit = chunk.words[offsetof(struct bank, commit) / 4];
			stored = chunk.words[offsetof(struct bank, crc) / 4];
			format = chunk.words[offsetof(struct bank, format) / 4];
			*found = chunk.words[offsetof(struct bank, generation) / 4];
			crc = pocket_crc32(0, chunk.bytes + CRC_FROM, sizeof chunk.bytes - CRC_FROM);
		}
		else if (at < sizeof(struct bank))
		{
			const uint32_t left = sizeof(struct bank) - at;
			crc = pocket_crc32(crc, chunk.bytes, left < sizeof chunk.bytes ? left : sizeof chunk.bytes);
		}
	}
	if (blank == ~0u) return BLANK;
	if (!ours) return FOREIGN;
	if (crc != stored || format != (BANK_VERSION << 8 | kind)) return OURS;
	return commit == 0 ? VALID : commit == ~0u ? WRITTEN : OURS;
}

int pocket_store_open(unsigned kind)
{
	uint32_t found[2] = { 0, 0 }, sample[3];
	live = -1;
	generation = 0;
	kindOpen = (uint8_t)kind;
	if (fm1_flash_size() != PARTITION_BYTES) return POCKET_STORE_UNAVAILABLE;
	/* a sample bank can have a blank sector in it, so being blank here does not make it free */
	if (fm1_flash_read(SAMPLE_OFFSET, sample, sizeof sample)) return POCKET_STORE_FAILED;
	if (sample[0] == SAMPLE_MAGIC && sample[2] > SAMPLE_REACH) return POCKET_STORE_OCCUPIED;
	for (unsigned s = 0; s < 2; s++) states[s] = (uint8_t)scan(kind, s, &found[s]);
	for (unsigned s = 0; s < 2; s++)
		if (states[s] == VALID && (live < 0 || (int32_t)(found[s] - found[live]) > 0)) live = (int8_t)s;
	if (live >= 0)
	{
		generation = found[live];
		return fm1_flash_read(sector_offset(kind, (unsigned)live), &bank, sizeof bank) ? POCKET_STORE_FAILED : POCKET_STORE_OK;
	}
	/* Nothing of ours to vouch for them, so a sector that is not ours stays as it is. With a good
	   bank beside it, that sector is a save that was cut off: the first save found it blank or
	   ours, and the sample bank check above is what would show anyone else had taken it since. */
	if (states[0] == FOREIGN || states[1] == FOREIGN) return POCKET_STORE_OCCUPIED;
	for (uint8_t* p = (uint8_t*)&bank; p < (uint8_t*)(&bank + 1); p++) *p = 0;
	return states[0] == OURS || states[1] == OURS || states[0] == WRITTEN || states[1] == WRITTEN ? POCKET_STORE_DAMAGED : POCKET_STORE_OK;
}

uint8_t* pocket_store_slot(unsigned slot)
{
	return bank.slots[slot];
}

int pocket_store_commit(void)
{
	const unsigned target = live >= 0 ? 1u - (unsigned)live : states[0] == BLANK ? 0u : states[1] == BLANK ? 1u : 0u;
	const uint32_t offset = sector_offset(kindOpen, target);
	const uint32_t committed = 0;
	uint32_t found;
	if (live < 0 && states[target] == FOREIGN) return POCKET_STORE_OCCUPIED;
	if (states[target] != BLANK && fm1_flash_erase(offset)) return POCKET_STORE_FAILED;
	states[target] = OURS; /* whatever follows, it is not blank, and it was ours to erase */
	bank.magic = BANK_MAGIC;
	bank.commit = ~0u;
	bank.format = BANK_VERSION << 8 | kindOpen;
	bank.generation = generation + 1u;
	bank.crc = pocket_crc32(0, (const uint8_t*)&bank + CRC_FROM, sizeof bank - CRC_FROM);
	if (fm1_flash_write(offset, &bank, sizeof bank) || scan(kindOpen, target, &found) != WRITTEN) return POCKET_STORE_FAILED;
	if (fm1_flash_write(offset + offsetof(struct bank, commit), &committed, sizeof committed)) return POCKET_STORE_FAILED;
	if (scan(kindOpen, target, &found) != VALID) return POCKET_STORE_FAILED;
	generation++;
	bank.commit = 0;
	states[target] = VALID;
	live = (int8_t)target;
	return POCKET_STORE_OK;
}
