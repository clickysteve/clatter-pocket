/*
 * pocket_history.h - one chronological undo and redo for everything an edit can change: steps,
 * sound parameters and the settings around them (docs/POCKET-CONTROLS.md).
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Control context only, RAM only: nothing here reaches Flash or survives a restart.
 *
 * A ring of change words, not of snapshots. An action is a mark followed by one word per thing it
 * changed, so turning a knob costs a word and a new 64-step pattern costs what actually differed.
 * What an action did is recorded, never how it was done: undoing a generated pattern restores it
 * without running a generator. When the ring is full the oldest actions go.
 */
#ifndef POCKET_HISTORY_H
#define POCKET_HISTORY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define POCKET_HISTORY_WORDS 4096u /* power of two: 16 KB */

enum { POCKET_HISTORY_CELL, POCKET_HISTORY_PARAM, POCKET_HISTORY_SETTING };

/* What undo and redo call for each word. A cell's value is XORed into it (one word serves both
 * directions); a parameter or setting is set to the value. */
typedef void (*pocket_history_apply)(void* context, unsigned kind, unsigned index, uint32_t value);

void pocket_history_reset(void);
/* Opens an action. A non-zero `merge` equal to the open action's continues it: turning one knob,
 * or the hits of a take, is one action. Returns 1 if it continued. Forgets what redo held. */
int pocket_history_begin(unsigned merge);
/* The action is complete: one that recorded nothing leaves no trace. */
void pocket_history_end(void);
/* The next action starts afresh, whatever its `merge`. */
void pocket_history_close(void);
void pocket_history_cell(unsigned index, uint32_t difference);
void pocket_history_value(unsigned kind, unsigned index, unsigned was, unsigned now);
/* The last action back, or the next one forward. Return 0 if there was none. */
int pocket_history_undo(pocket_history_apply apply, void* context);
int pocket_history_redo(pocket_history_apply apply, void* context);
unsigned pocket_history_used(void); /* words held, for the tests */

#ifdef __cplusplus
}
#endif

#endif
