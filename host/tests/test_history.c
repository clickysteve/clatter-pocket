/*
 * test_history.c - the undo ring (ui/pocket_history.c) against a model of what it should restore,
 * with AddressSanitizer: random actions, undos, redos and overflow, checked after every one.
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdio.h>
#include <string.h>
#include "pocket_history.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define CELLS 448
#define VALUES 247
struct state {
	uint32_t cells[CELLS];
	uint8_t params[VALUES], settings[VALUES];
};
static struct state now;

static void apply(void* context, unsigned kind, unsigned index, uint32_t value)
{
	(void)context;
	if (kind == POCKET_HISTORY_CELL) now.cells[index] ^= value;
	else if (kind == POCKET_HISTORY_PARAM) now.params[index] = (uint8_t)value;
	else now.settings[index] = (uint8_t)value;
}

static void set_cell(unsigned i, uint32_t v)
{
	pocket_history_cell(i, now.cells[i] ^ v);
	now.cells[i] = v;
}

static void set_value(unsigned kind, unsigned i, unsigned v)
{
	uint8_t* row = kind == POCKET_HISTORY_PARAM ? now.params : now.settings;
	pocket_history_value(kind, i, row[i], v);
	row[i] = (uint8_t)v;
}

static uint32_t rng = 12345;
static uint32_t draw(uint32_t n)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return (rng >> 8) % n;
}

static void test_order(void)
{
	memset(&now, 0, sizeof now);
	pocket_history_reset();
	CHECK(!pocket_history_undo(apply, 0), "nothing to undo at first");
	CHECK(!pocket_history_redo(apply, 0), "nothing to redo at first");
	/* a kit, a pattern, a sound: back in that order, and forward */
	pocket_history_begin(0); set_value(POCKET_HISTORY_PARAM, 8, 40); set_value(POCKET_HISTORY_SETTING, 1, 3); pocket_history_end();
	pocket_history_begin(0); set_cell(10, 0x12345); set_cell(447, 0xfffff); pocket_history_end();
	pocket_history_begin(0); set_value(POCKET_HISTORY_PARAM, 8, 41); pocket_history_end();
	CHECK(pocket_history_undo(apply, 0) && now.params[8] == 40 && now.cells[10] == 0x12345, "the sound goes first");
	CHECK(pocket_history_undo(apply, 0) && !now.cells[10] && !now.cells[447] && now.params[8] == 40, "then the pattern");
	CHECK(pocket_history_undo(apply, 0) && !now.params[8] && !now.settings[1], "then the kit");
	CHECK(!pocket_history_undo(apply, 0), "then nothing");
	CHECK(pocket_history_redo(apply, 0) && now.params[8] == 40 && now.settings[1] == 3 && !now.cells[10], "forward in the same order");
	CHECK(pocket_history_redo(apply, 0) && now.cells[447] == 0xfffff, "the pattern");
	CHECK(pocket_history_redo(apply, 0) && now.params[8] == 41, "the sound");
	CHECK(!pocket_history_redo(apply, 0), "and no further");

	/* a new edit after an undo forgets the redo */
	pocket_history_undo(apply, 0);
	pocket_history_begin(0); set_cell(1, 9); pocket_history_end();
	CHECK(!pocket_history_redo(apply, 0), "redo is gone");
	/* one that changed nothing does not */
	pocket_history_undo(apply, 0);
	pocket_history_begin(0); pocket_history_end();
	CHECK(pocket_history_redo(apply, 0) && now.cells[1] == 9, "an empty action leaves redo alone");
	const unsigned used = pocket_history_used();
	pocket_history_begin(7); pocket_history_end();
	CHECK(pocket_history_used() == used, "and costs nothing");

	/* merging: a knob turned is one undo of one word */
	memset(&now, 0, sizeof now);
	pocket_history_reset();
	for (unsigned v = 1; v < 50; v++)
	{
		pocket_history_begin(9);
		set_value(POCKET_HISTORY_PARAM, 20, v);
		pocket_history_end();
	}
	CHECK(pocket_history_used() == 2, "a mark and a word: %u", pocket_history_used());
	pocket_history_close();
	pocket_history_begin(9); set_value(POCKET_HISTORY_PARAM, 20, 60); pocket_history_end();
	CHECK(pocket_history_undo(apply, 0) && now.params[20] == 49, "closed: the next is its own");
	CHECK(pocket_history_undo(apply, 0) && !now.params[20], "and the turn is one undo");
	/* a cell merged by XOR, and hits of a take on several cells */
	memset(&now, 0, sizeof now);
	pocket_history_reset();
	for (unsigned h = 0; h < 40; h++)
	{
		pocket_history_begin(2);
		set_cell(h % 7, h + 1);
		pocket_history_end();
	}
	pocket_history_close();
	CHECK(pocket_history_undo(apply, 0), "a take");
	int left = 0;
	for (int i = 0; i < CELLS; i++) left += now.cells[i] != 0;
	CHECK(!left, "goes in one: %d cells left", left);
}

/* Overflow: the oldest actions go, whole, and what is left still replays exactly. */
static void test_overflow(void)
{
	memset(&now, 0, sizeof now);
	pocket_history_reset();
	static struct state first;
	for (unsigned a = 0; a < 200; a++)
	{
		pocket_history_begin(0);
		for (unsigned c = 0; c < CELLS; c++) set_cell(c, a * 1000u + c + 1u);
		pocket_history_end();
		if (a == 190) first = now;
	}
	CHECK(pocket_history_used() <= POCKET_HISTORY_WORDS, "the ring is bounded: %u", pocket_history_used());
	unsigned levels = 0;
	while (pocket_history_undo(apply, 0))
	{
		levels++;
		if (levels == 9) CHECK(!memcmp(&now, &first, sizeof now), "nine actions back is what it was");
	}
	CHECK(levels == POCKET_HISTORY_WORDS / (CELLS + 1), "whole patterns kept: %u", levels);
	int torn = 0;
	for (unsigned c = 1; c < CELLS; c++) torn += now.cells[c] - now.cells[0] != c;
	CHECK(!torn, "the last one undone is a whole pattern: %d", torn);
}

/* Anything, in any order, against a shadow of every state the model has been in. */
#define MODEL 64
static struct state states[MODEL];
static void test_random(void)
{
	for (unsigned round = 0; round < 40; round++)
	{
		memset(&now, 0, sizeof now);
		pocket_history_reset();
		unsigned at = 0, top = 0, floor = 0, words = 0;
		static unsigned cost[MODEL];
		states[0] = now;
		for (unsigned step = 0; step < 6000; step++)
		{
			const unsigned what = draw(10);
			if (what < 3 && pocket_history_undo(apply, 0))
			{
				CHECK(at > floor, "undo past the oldest");
				at--;
				CHECK(!memcmp(&now, &states[at % MODEL], sizeof now), "undo %u", step);
			}
			else if (what < 5 && at < top)
			{
				CHECK(pocket_history_redo(apply, 0), "redo");
				at++;
				CHECK(!memcmp(&now, &states[at % MODEL], sizeof now), "redo %u", step);
			}
			else if (what >= 5)
			{
				unsigned n = 1 + draw(what == 9 ? 300 : 6), changes = 0;
				pocket_history_begin(0);
				for (unsigned i = 0; i < n; i++)
				{
					const unsigned kind = draw(3);
					if (kind == 0)
					{
						const unsigned c = draw(CELLS);
						const uint32_t v = draw(1u << 20);
						if (v != now.cells[c]) { set_cell(c, v); changes++; }
					}
					else
					{
						const unsigned p = draw(VALUES), v = draw(128);
						set_value(kind == 1 ? POCKET_HISTORY_PARAM : POCKET_HISTORY_SETTING, p, v);
						changes++;
					}
				}
				pocket_history_end();
				if (!changes) continue;
				at++;
				top = at;
				states[at % MODEL] = now;
				cost[at % MODEL] = changes + 1u;
				if (at - floor >= MODEL - 1u) floor = at - (MODEL - 2u);
				(void)words;
			}
		}
	}
}

int main(void)
{
	test_order();
	test_overflow();
	test_random();
	printf("history: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
