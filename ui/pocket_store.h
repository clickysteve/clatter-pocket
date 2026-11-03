/*
 * pocket_store.h - saved projects, in four sectors of the FM-1's user Flash. The audit behind it,
 * why these sectors, and how saves from before projects are kept are in docs/POCKET-STORAGE.md.
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Control context only, and only when asked. At start-up, and never in SAFE MODE, one read of the
 * four sectors finds GLOBAL's settings, and writes nothing (pocket_store_settings).
 *
 * Three of the four sectors hold three projects each, and one is always free. A save rewrites the
 * sector of its project into a free one, reads it back, and only then sets its commit word; the
 * sector it replaces becomes free. So the copy before stays whole until the new one is, and a power
 * cut at any moment loses at most the save in progress. The records' contents are the caller's.
 *
 * Nothing that is not ours is erased or written until PROJECTS opens: a sector is used only if
 * it is blank, or is one of ours, and a store whose sectors hold anything else refuses to open until
 * pocket_store_claim, which opening PROJECTS, stopped, calls. On a used FM-1 these sectors can hold
 * what stock V15 left there. A sample bank is never claimed.
 *
 * Every sector a save writes also carries GLOBAL's settings, in its header: what was last given to
 * pocket_store_set_settings. The newest good sector has the newest settings.
 */
#ifndef POCKET_STORE_H
#define POCKET_STORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define POCKET_STORE_PROJECTS 9
#define POCKET_STORE_PROJECT_BYTES 1354
#define POCKET_STORE_SETTINGS 8 /* the caller's bytes, kept in a sector's header */

enum {
	POCKET_STORE_OK = 0,
	POCKET_STORE_DAMAGED = 1,      /* opened, but a good copy of something is gone */
	POCKET_STORE_UNAVAILABLE = -1, /* the user partition is not the size the sectors assume */
	POCKET_STORE_OCCUPIED = -2,    /* something else is in these sectors: they stay untouched */
	POCKET_STORE_FAILED = -3,      /* Flash refused, or read back wrong: the last good copy stands */
	POCKET_STORE_SAMPLES = -4      /* a stock sample bank reaches these sectors: they stay untouched */
};

/* Looks at all four sectors. Writes nothing. Any result below zero leaves nothing to save to. */
int pocket_store_open(void);
/* `bytes` of a project from byte `at`, all zero if it is empty. */
void pocket_store_peek(unsigned project, unsigned at, void* data, unsigned bytes);
/* Reads a project's sector into the one held in RAM, and returns its record there, to read or to
 * fill: all zero if it is empty. NULL if Flash refused. A change stays in RAM until _commit. */
uint8_t* pocket_store_project(unsigned project);
/* Writes the sector in RAM to Flash, safely. Blocks for the length of an erase, and silences audio
 * while it does. After a failure, open the store again: RAM no longer matches Flash. */
int pocket_store_commit(void);
/* Asked for, on POCKET_STORE_OCCUPIED only: erases what is in the way, and opens the store. Blocks
 * for each erase and silences audio. A cut leaves what is left in the way, to be asked again. */
int pocket_store_claim(void);

/* At start-up: reads our four sectors, writes nothing, and copies the settings of the newest good
 * one to data. 0 if it has none, as every sector before 0.9.2, or none could be read: data is then
 * not to be used. Needs no _open. */
int pocket_store_settings(uint8_t* data);
/* What every following commit writes as the settings. NULL: none, as before 0.9.2. */
void pocket_store_set_settings(const uint8_t* data);
/* The settings alone: opens the store and writes its newest page again, at most one erase, as any
 * save. A result below zero, as _open's, and nothing written. */
int pocket_store_commit_settings(void);

#ifdef __cplusplus
}
#endif

#endif
