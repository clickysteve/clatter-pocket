/*
 * pocket_history.c - see pocket_history.h.
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * A word's top two bits are its kind:
 *
 *   cell     index 29:21, XOR of the cell 19:0 (pocket_seq.h: cells use 20 bits)
 *   value    index 23:16, was 15:8, now 7:0
 *   mark     the start of an action
 *
 * Words are found by counters that only ever grow; `oldest <= present <= newest`. Everything from
 * `present` to `newest` is what redo can still replay.
 */
#include "pocket_history.h"

#define MARK 3u
#define KIND(word) ((word) >> 30)
#define AT(i) words[(i) & (POCKET_HISTORY_WORDS - 1u)]

_Static_assert((POCKET_HISTORY_WORDS & (POCKET_HISTORY_WORDS - 1u)) == 0, "a power of two");

static uint32_t words[POCKET_HISTORY_WORDS];
static uint32_t oldest, present, newest;
static unsigned open; /* the open action's merge, or 0 */
static uint8_t unmarked; /* an action has begun and recorded nothing: it costs nothing yet */

void pocket_history_reset(void)
{
	oldest = present = newest = 0;
	open = 0;
	unmarked = 0;
}

static void push(uint32_t word)
{
	if (present - oldest == POCKET_HISTORY_WORDS)
		do oldest++; while (oldest != present && KIND(AT(oldest)) != MARK);
	AT(present) = word;
	newest = ++present;
}

/* The first word of an action brings its mark, and that is what forgets the redo. */
static void put(uint32_t word)
{
	if (unmarked) push(MARK << 30);
	unmarked = 0;
	push(word);
}

int pocket_history_begin(unsigned merge)
{
	if (merge && merge == open) return 1;
	open = merge;
	unmarked = 1;
	return 0;
}

void pocket_history_end(void)
{
	if (unmarked) open = 0;
	unmarked = 0;
}

void pocket_history_close(void)
{
	open = 0;
}

void pocket_history_cell(unsigned index, uint32_t difference)
{
	const uint32_t word = (uint32_t)POCKET_HISTORY_CELL << 30 | (uint32_t)index << 21;
	if (!unmarked && present != oldest && (AT(present - 1u) ^ word) >> 21 == 0) AT(present - 1u) ^= difference;
	else put(word | difference);
}

void pocket_history_value(unsigned kind, unsigned index, unsigned was, unsigned now)
{
	const uint32_t word = (uint32_t)kind << 30 | (uint32_t)index << 16;
	if (!unmarked && present != oldest && (AT(present - 1u) ^ word) >> 16 == 0) AT(present - 1u) = (AT(present - 1u) & ~0xffu) | now;
	else put(word | was << 8 | now);
}

static void replay(uint32_t word, int forward, pocket_history_apply apply, void* context)
{
	const unsigned kind = KIND(word);
	if (kind == POCKET_HISTORY_CELL) apply(context, kind, word >> 21 & 511u, word & 0xfffffu);
	else apply(context, kind, word >> 16 & 255u, forward ? word & 255u : word >> 8 & 255u);
}

int pocket_history_undo(pocket_history_apply apply, void* context)
{
	if (present == oldest) return 0;
	open = 0;
	while (present != oldest)
	{
		const uint32_t word = AT(--present);
		if (KIND(word) == MARK) break;
		replay(word, 0, apply, context);
	}
	return 1;
}

int pocket_history_redo(pocket_history_apply apply, void* context)
{
	if (present == newest) return 0;
	open = 0;
	present++; /* its mark */
	while (present != newest && KIND(AT(present)) != MARK) replay(AT(present++), 1, apply, context);
	return 1;
}

unsigned pocket_history_used(void)
{
	return newest - oldest;
}
