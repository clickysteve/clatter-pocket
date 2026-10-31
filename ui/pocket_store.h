/*
 * pocket_store.h - saved projects, in four sectors of the FM-1's user Flash. The audit behind it,
 * why these sectors, and how saves from before projects are kept are in docs/POCKET-STORAGE.md.
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Control context only, and only when asked: nothing here runs at start-up or in SAFE MODE.
 *
 * Three of the four sectors hold three projects each, and one is always free. A save rewrites the
 * sector of its project into a free one, reads it back, and only then sets its commit word; the
 * sector it replaces becomes free. So the copy before stays whole until the new one is, and a power
 * cut at any moment loses at most the save in progress. The records' contents are the caller's.
 *
 * Nothing that is not ours is ever erased or written: a sector is used only if it is blank, or is
 * one of ours, and a store whose sectors hold anything else refuses to open. Banks of kits and
 * patterns from firmware before projects are never written either, and their space is only given
 * up by pocket_store_forget_old.
 */
#ifndef POCKET_STORE_H
#define POCKET_STORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define POCKET_STORE_PROJECTS 9
#define POCKET_STORE_PROJECT_BYTES 1354
/* Firmware before projects: 12 kits and 12 patterns, a bank each, 256 bytes a slot. */
#define POCKET_STORE_OLD_SLOTS 12
#define POCKET_STORE_OLD_SLOT_BYTES 256
enum { POCKET_STORE_OLD_KITS, POCKET_STORE_OLD_PATTERNS, POCKET_STORE_OLD_KINDS };

enum {
	POCKET_STORE_OK = 0,
	POCKET_STORE_DAMAGED = 1,      /* opened, but a good copy of something is gone */
	POCKET_STORE_UNAVAILABLE = -1, /* the user partition is not the size the sectors assume */
	POCKET_STORE_OCCUPIED = -2,    /* something else is in these sectors: they stay untouched */
	POCKET_STORE_FAILED = -3       /* Flash refused, or read back wrong: the last good copy stands */
};

/* Looks at all four sectors. Writes nothing. Any result below zero leaves nothing to save to. */
int pocket_store_open(void);
/* How many projects there is room for: all of them, unless old banks are holding their sectors. */
unsigned pocket_store_room(void);
/* The first `bytes` of a project, all zero if it is empty. */
void pocket_store_peek(unsigned project, void* data, unsigned bytes);
/* Reads a project's sector into the one held in RAM, and returns its record there, to read or to
 * fill: all zero if it is empty. NULL if Flash refused. A change stays in RAM until _commit. */
uint8_t* pocket_store_project(unsigned project);
/* Writes the sector in RAM to Flash, safely. Blocks for the length of an erase, and silences audio
 * while it does. After a failure, open the store again: RAM no longer matches Flash. */
int pocket_store_commit(void);
/* The old bank of a kind: is there one, and the first `bytes` of a slot of it, all zero if empty. */
int pocket_store_old(unsigned kind);
void pocket_store_old_peek(unsigned kind, unsigned slot, void* data, unsigned bytes);
/* Gives up the old banks: their sectors go back to holding projects. Asked for, never assumed. */
int pocket_store_forget_old(void);

#ifdef __cplusplus
}
#endif

#endif
