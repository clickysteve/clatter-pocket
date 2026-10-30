/*
 * fake_flash.c - see fake_flash.h.
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <string.h>
#include "fake_flash.h"

#define SECTOR 4096u
#define PAGE 256u
enum { OK = 0, IO = -2, NEEDS_ERASE = -4 };

uint8_t fake_flash[FAKE_FLASH_BYTES];
uint32_t fake_flash_size = FAKE_FLASH_BYTES;
long fake_flash_ops, fake_flash_cut_at = -1, fake_flash_fail_at = -1, fake_flash_reads;
static int powered = 1;

void fake_flash_reset(void)
{
	memset(fake_flash, 0xff, sizeof fake_flash);
	fake_flash_size = FAKE_FLASH_BYTES;
	fake_flash_ops = fake_flash_reads = 0;
	fake_flash_cut_at = fake_flash_fail_at = -1;
	powered = 1;
}

uint32_t fm1_flash_size(void) { return fake_flash_size; }

int fm1_flash_read(uint32_t offset, void* data, size_t bytes)
{
	if (offset + bytes > FAKE_FLASH_BYTES) return -1;
	fake_flash_reads++;
	memcpy(data, fake_flash + offset, bytes);
	return OK;
}

void fake_flash_power_on(void) { powered = 1; }

/* Whether this operation runs: returns 1 to do all of it, 0 to do half, -1 to do none. */
static int begin(void)
{
	if (!powered) return -1;
	const long op = fake_flash_ops++;
	if (op == fake_flash_cut_at)
	{
		powered = 0;
		return 0;
	}
	return op == fake_flash_fail_at ? -1 : 1;
}

int fm1_flash_erase(uint32_t offset)
{
	if (offset % SECTOR || offset >= FAKE_FLASH_BYTES) return -1;
	const int run = begin();
	if (run < 0) return IO;
	memset(fake_flash + offset, 0xff, run ? SECTOR : SECTOR / 2);
	return run ? OK : IO;
}

int fm1_flash_write(uint32_t offset, const void* data, size_t bytes)
{
	const uint8_t* in = data;
	if (offset + bytes > FAKE_FLASH_BYTES) return -1;
	while (bytes)
	{
		size_t n = PAGE - offset % PAGE;
		if (n > bytes) n = bytes;
		const int run = begin();
		if (run < 0) return IO;
		for (size_t i = 0; i < (run ? n : n / 2); i++)
		{
			if (in[i] & ~fake_flash[offset + i]) return NEEDS_ERASE;
			fake_flash[offset + i] &= in[i];
		}
		if (!run) return IO;
		offset += (uint32_t)n;
		in += n;
		bytes -= n;
	}
	return OK;
}
