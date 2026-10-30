/*
 * fake_flash.h - the SDK's fm1_flash_* API on a model of the FM-1's NOR chip, for the host tests:
 * programming only clears bits, erasing sets a sector's, and a power cut can be put at any
 * operation, leaving it half done as tools/fm1-emu's FM1_FLASH_CUT does.
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef FAKE_FLASH_H
#define FAKE_FLASH_H
#include <stddef.h>
#include <stdint.h>

#define FAKE_FLASH_BYTES 536576u
extern uint8_t fake_flash[FAKE_FLASH_BYTES];
extern uint32_t fake_flash_size;   /* what fm1_flash_size reports */
extern long fake_flash_ops;        /* erases and page programs so far */
extern long fake_flash_cut_at;     /* this op stops half way and none after it arrive; -1 never */
extern long fake_flash_fail_at;    /* this op reports FM1_FLASH_IO, doing nothing; -1 never */
extern long fake_flash_reads;      /* read calls so far */

void fake_flash_reset(void);       /* all erased */
void fake_flash_power_on(void);    /* after a cut */

uint32_t fm1_flash_size(void);
int fm1_flash_read(uint32_t offset, void* data, size_t bytes);
int fm1_flash_write(uint32_t offset, const void* data, size_t bytes);
int fm1_flash_erase(uint32_t offset);
#endif
