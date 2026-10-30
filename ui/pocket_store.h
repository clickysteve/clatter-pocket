/*
 * pocket_store.h - saved kits and patterns, in four sectors of the FM-1's user Flash. The audit
 * behind it, and why these sectors, are in docs/POCKET-STORAGE.md.
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Control context only, and only when asked: nothing here runs at start-up or in SAFE MODE.
 *
 * Each of the two stores, kits and patterns, is one bank of POCKET_STORE_SLOTS slots, kept in a
 * pair of sectors. A save writes the whole bank to the older sector, reads it back, and only then
 * sets its commit word, so the sector before it stays whole until the new one is. A power cut at any
 * moment loses at most the save in progress. The slots' contents are the caller's.
 *
 * Nothing that is not ours is ever erased or written: a sector is used only if it is blank, or is
 * one of ours, and a store whose sectors hold anything else refuses to open.
 */
#ifndef POCKET_STORE_H
#define POCKET_STORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define POCKET_STORE_SLOTS 12
#define POCKET_STORE_SLOT_BYTES 256

enum { POCKET_STORE_KITS, POCKET_STORE_PATTERNS, POCKET_STORE_KINDS };

enum {
	POCKET_STORE_OK = 0,
	POCKET_STORE_DAMAGED = 1,      /* opened empty: neither sector held a good bank */
	POCKET_STORE_UNAVAILABLE = -1, /* the user partition is not the size the sectors assume */
	POCKET_STORE_OCCUPIED = -2,    /* something else is in these sectors: they stay untouched */
	POCKET_STORE_FAILED = -3       /* Flash refused, or read back wrong: the last good bank stands */
};

/* Reads a store into the one bank held in RAM. A slot of an empty store is all zero. Any result
 * below zero leaves nothing to save to. */
int pocket_store_open(unsigned kind);
/* The open bank's slot, to read or to fill. A change stays in RAM until pocket_store_commit. */
uint8_t* pocket_store_slot(unsigned slot);
/* Writes the open bank to Flash, safely. Blocks for the length of an erase, and silences audio
 * while it does. After a failure, open the store again: RAM no longer matches Flash. */
int pocket_store_commit(void);

#ifdef __cplusplus
}
#endif

#endif
