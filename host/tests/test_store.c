/*
 * test_store.c - saved banks on a model of the Flash chip (host/tests/fake_flash.c): what is
 * refused, what survives, and a power cut at every single operation of a save.
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fake_flash.h"
#include "pocket_store.h"
#include "pocket_lxr.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define FIRST 430080u
#define SECTOR 4096u
#define SAMPLE 69632u
#define SECTORS (4 * SECTOR)

static uint8_t outside[FAKE_FLASH_BYTES];

/* Everything but our four sectors has this pattern, and must keep it. */
static void fill_outside(void)
{
	for (uint32_t i = 0; i < FAKE_FLASH_BYTES; i++)
		if (i < FIRST || i >= FIRST + SECTORS) fake_flash[i] = (uint8_t)(i * 7u + (i >> 9));
	fake_flash[SAMPLE] = 0; /* no sample magic */
	memcpy(outside, fake_flash, sizeof outside);
}

static int outside_intact(void)
{
	return !memcmp(fake_flash, outside, FIRST) && !memcmp(fake_flash + FIRST + SECTORS, outside + FIRST + SECTORS, FAKE_FLASH_BYTES - FIRST - SECTORS);
}

static void put(unsigned project, unsigned value)
{
	memset(pocket_store_project(project), (int)value, POCKET_STORE_PROJECT_BYTES);
}

static int save(unsigned project, unsigned value)
{
	put(project, value);
	return pocket_store_commit();
}

static int has(unsigned project, unsigned value)
{
	const uint8_t* p = pocket_store_project(project);
	if (!p) return 0;
	for (unsigned i = 0; i < POCKET_STORE_PROJECT_BYTES; i++)
		if (p[i] != (uint8_t)value) return 0;
	return 1;
}

static void test_basics(void)
{
	fake_flash_reset();
	fill_outside();
	CHECK(pocket_store_open() == POCKET_STORE_OK, "blank opens");
	CHECK(fake_flash_ops == 0, "opening writes nothing");
	CHECK(POCKET_STORE_PROJECTS == POCKET_STORE_PROJECTS, "all of it is free: %u", POCKET_STORE_PROJECTS);
	CHECK(has(0, 0) && has(POCKET_STORE_PROJECTS - 1, 0), "a blank store is empty");
	CHECK(save(0, 11) == POCKET_STORE_OK, "a save");
	CHECK(save(1, 12) == POCKET_STORE_OK, "beside it, in its sector");
	CHECK(pocket_store_open() == POCKET_STORE_OK && has(0, 11) && has(1, 12) && has(2, 0), "both are read back");
	/* every project, each its own, and from any order */
	for (unsigned p = 0; p < POCKET_STORE_PROJECTS; p++) CHECK(save(POCKET_STORE_PROJECTS - 1 - p, 100 + p) == POCKET_STORE_OK, "project %u", p);
	CHECK(pocket_store_open() == POCKET_STORE_OK, "and open");
	for (unsigned p = 0; p < POCKET_STORE_PROJECTS; p++) CHECK(has(POCKET_STORE_PROJECTS - 1 - p, 100 + p), "project %u reads back", p);
	uint8_t head[4];
	pocket_store_peek(4, head, sizeof head);
	CHECK(head[0] == 104 && head[3] == 104, "peeking at one reads it without loading its sector");
	CHECK(outside_intact(), "only our sectors were written");

	/* many saves, to one project and to others: nothing moves, and it stays a page count */
	long before = fake_flash_ops;
	for (unsigned i = 0; i < 300; i++)
	{
		CHECK(save(i % 2 ? 7 : 4, i & 255u) == POCKET_STORE_OK, "save %u", i);
		pocket_store_open();
		CHECK(has(i % 2 ? 7 : 4, i & 255u) && has(0, 108), "save %u reads back", i);
	}
	CHECK((fake_flash_ops - before) / 300 < 24, "a save is a page count, not a sweep: %ld", (fake_flash_ops - before) / 300);
	CHECK(outside_intact(), "still only ours");

	/* the generation carries on past a restart, so the newest copy stays the newest */
	save(0, 7);
	pocket_store_open();
	save(0, 8);
	pocket_store_open();
	CHECK(has(0, 8), "the later of two saves across a restart");
}

/* What refuses, and that refusing touches nothing. */
static void test_refusals(void)
{
	fake_flash_reset();
	fill_outside();
	fake_flash_size = 0;
	CHECK(pocket_store_open() == POCKET_STORE_UNAVAILABLE, "no reservation");
	fake_flash_size = 512000;
	CHECK(pocket_store_open() == POCKET_STORE_UNAVAILABLE, "another layout");
	fake_flash_size = FAKE_FLASH_BYTES;

	/* a byte anywhere in any sector is someone else's data: nothing of ours may erase it */
	const uint32_t spots[] = { 0, 1, 40, 2000, 4095 };
	for (unsigned s = 0; s < 5; s++)
		for (unsigned sector = 0; sector < 4; sector++)
		{
			memset(fake_flash + FIRST, 0xff, SECTORS);
			fake_flash[FIRST + sector * SECTOR + spots[s]] = 0x12;
			const long ops = fake_flash_ops;
			CHECK(pocket_store_open() == POCKET_STORE_OCCUPIED, "foreign data in sector %u at %u", sector, spots[s]);
			put(0, 1);
			CHECK(pocket_store_commit() == POCKET_STORE_OCCUPIED, "and a save is refused");
			CHECK(fake_flash_ops == ops && fake_flash[FIRST + sector * SECTOR + spots[s]] == 0x12, "nothing written");
		}
	/* a sample bank long enough to cover our sectors, even if they are blank in it */
	memset(fake_flash + FIRST, 0xff, SECTORS);
	uint32_t bank[3] = { 0x53314d46u, 1, 400000 };
	memcpy(fake_flash + SAMPLE, bank, sizeof bank);
	CHECK(pocket_store_open() == POCKET_STORE_SAMPLES, "a bank that reaches us");
	CHECK(pocket_store_claim() == POCKET_STORE_FAILED, "and it cannot be claimed");
	bank[2] = 360448; /* exactly up to them */
	memcpy(fake_flash + SAMPLE, bank, sizeof bank);
	CHECK(pocket_store_open() == POCKET_STORE_OK, "a bank that stops short");

	/* a failure Flash reports is not a save */
	fake_flash_reset();
	fill_outside();
	pocket_store_open();
	put(0, 5);
	fake_flash_fail_at = fake_flash_ops + 3;
	CHECK(pocket_store_commit() == POCKET_STORE_FAILED, "Flash reports a failure");
	fake_flash_fail_at = -1;
	CHECK(pocket_store_open() >= 0, "and it opens after");
	CHECK(save(0, 6) == POCKET_STORE_OK, "and a later save works");
	CHECK(pocket_store_open() == POCKET_STORE_OK && has(0, 6), "and stands");
}

/* A corrupted sector is passed over: the copy before it is there. Both, and nothing is shown. */
static void test_corruption(void)
{
	fake_flash_reset();
	fill_outside();
	pocket_store_open();
	save(0, 1);
	save(0, 2);
	for (uint32_t at = 0; at < 4096; at += 37)
		for (unsigned s = 0; s < 2; s++)
		{
			const uint8_t was = fake_flash[FIRST + s * SECTOR + at];
			fake_flash[FIRST + s * SECTOR + at] = (uint8_t)(was ^ 0x10);
			const int r = pocket_store_open();
			CHECK(r >= 0, "a flipped bit at %u of sector %u: %d", at, s, r);
			CHECK(has(0, 1) || has(0, 2), "a sector is read whole, never a mixture");
			fake_flash[FIRST + s * SECTOR + at] = was;
		}
	/* both corrupted: nothing is believed, and a save is still possible */
	fake_flash[FIRST + 100] ^= 1;
	fake_flash[FIRST + SECTOR + 100] ^= 1;
	CHECK(pocket_store_open() == POCKET_STORE_DAMAGED, "both bad");
	CHECK(has(0, 0), "shows empty, not damaged data");
	CHECK(save(0, 7) == POCKET_STORE_OK, "they are ours to replace");
	CHECK(pocket_store_open() >= 0 && has(0, 7), "the new save stands");
	/* one sector's two copies gone, with other sectors good: said, and the others stand */
	fake_flash_reset();
	fill_outside();
	pocket_store_open();
	save(0, 1);
	save(0, 2);
	save(3, 3);
	save(3, 4);
	pocket_store_open();
	for (unsigned s = 0; s < 4; s++)
	{
		uint32_t format;
		memcpy(&format, fake_flash + FIRST + s * SECTOR + 12, 4);
		if ((format & 255u) == 0) fake_flash[FIRST + s * SECTOR + 100] ^= 1;
	}
	CHECK(pocket_store_open() == POCKET_STORE_DAMAGED, "two bad sectors are said");
	CHECK(has(3, 4) && has(0, 0), "the rest stands, and what is lost is empty");
	CHECK(outside_intact(), "still only ours");
}

/* A power cut at every operation of a save, of several histories. After it: the old sector or the
 * new one, whole, the other sectors as they were, and the store still works. */
static void history(unsigned saves)
{
	fake_flash_reset();
	fill_outside();
	pocket_store_open();
	for (unsigned i = 0; i < saves; i++)
	{
		save(i % 9, 10 + i); /* every sector in turn, so the free one moves */
		save(1, 50);
	}
}

static void cut_everywhere(unsigned saves, unsigned project)
{
	history(saves);
	const unsigned expected = pocket_store_project(project) ? pocket_store_project(project)[0] : 0, neighbour = project / 3 * 3 + (project % 3 + 1) % 3;
	const unsigned beside = pocket_store_project(neighbour)[0], elsewhere = pocket_store_project((project + 3) % 9)[0];
	const long from = fake_flash_ops;
	save(project, 200);
	const long ops = fake_flash_ops - from;
	CHECK(ops >= 2, "a save has operations");
	int older = 0, newer = 0;
	for (long cut = 0; cut < ops + 1; cut++)
	{
		history(saves);
		fake_flash_cut_at = fake_flash_ops + cut;
		save(project, 200);
		/* power comes back */
		fake_flash_cut_at = -1;
		fake_flash_power_on();
		const int r = pocket_store_open();
		CHECK(r >= 0, "cut %ld of %ld: opens: %d", cut, ops, r);
		const int isnew = has(project, 200), isold = has(project, expected);
		CHECK(isnew || isold, "cut %ld: the old sector or the new one", cut);
		older += isold;
		newer += isnew;
		CHECK(has(neighbour, beside), "cut %ld: the rest of its sector is there", cut);
		CHECK(has((project + 3) % 9, elsewhere), "cut %ld: and the other sectors", cut);
		CHECK(outside_intact(), "cut %ld: nothing else touched", cut);
		CHECK(save(project, 201) == POCKET_STORE_OK, "cut %ld: and it saves again", cut);
		CHECK(pocket_store_open() >= 0 && has(project, 201) && has(neighbour, beside), "cut %ld: that stands", cut);
	}
	CHECK(older > 0 && newer > 0, "some cuts land before the commit, and some after: %d %d", older, newer);
}

/* What V15 left in the sectors: asked for, it goes, and only that goes */
static void test_claim(void)
{
	fake_flash_reset();
	CHECK(pocket_store_claim() == POCKET_STORE_FAILED, "a blank store has nothing to claim");
	CHECK(fake_flash_ops == 0, "and nothing is erased for it");
	memset(fake_flash + FIRST, 0x42, SECTORS);
	memset(fake_flash + FIRST - SECTOR, 0x33, SECTOR);
	memset(fake_flash + FIRST + SECTORS, 0x44, SECTOR);
	CHECK(pocket_store_open() == POCKET_STORE_OCCUPIED, "held");
	CHECK(pocket_store_claim() == POCKET_STORE_OK, "claimed");
	int blank = 1;
	for (unsigned i = 0; i < SECTORS; i++) blank &= fake_flash[FIRST + i] == 0xff;
	CHECK(blank, "all four erased");
	CHECK(fake_flash[FIRST - 1] == 0x33 && fake_flash[FIRST - SECTOR] == 0x33 && fake_flash[FIRST + SECTORS] == 0x44
	      && fake_flash[FIRST + SECTORS + SECTOR - 1] == 0x44, "and nothing either side");
	put(0, 7);
	CHECK(pocket_store_commit() == POCKET_STORE_OK, "and a save then works");
	CHECK(pocket_store_open() == POCKET_STORE_OK, "and is found");
	CHECK(pocket_store_claim() == POCKET_STORE_FAILED, "a store of ours is not claimed");

	/* only the sectors in the way are touched */
	fake_flash_reset();
	memset(fake_flash + FIRST + SECTOR, 0x42, SECTOR);
	CHECK(pocket_store_claim() == POCKET_STORE_OK, "one sector");
	CHECK(fake_flash_ops == 1, "one erase: %ld", fake_flash_ops);

	/* a bank from firmware 0.5.0 is not read any more: to this store it is someone else's */
	fake_flash_reset();
	const uint32_t older = 0x31535043u;
	memcpy(fake_flash + FIRST, &older, 4);
	CHECK(pocket_store_open() == POCKET_STORE_OCCUPIED, "held like anything else");

	/* a power cut anywhere, a refusal anywhere: asked again, it finishes */
	for (long cut = 0; cut < 4; cut++)
	{
		fake_flash_reset();
		memset(fake_flash + FIRST, 0x42, SECTORS);
		fake_flash_cut_at = cut;
		pocket_store_claim();
		fake_flash_power_on();
		fake_flash_cut_at = -1;
		CHECK(pocket_store_open() == POCKET_STORE_OCCUPIED, "cut at %ld: still held", cut);
		CHECK(pocket_store_claim() == POCKET_STORE_OK, "cut at %ld: and claimed after", cut);
		fake_flash_reset();
		memset(fake_flash + FIRST, 0x42, SECTORS);
		fake_flash_fail_at = cut;
		CHECK(pocket_store_claim() == POCKET_STORE_FAILED, "refused at %ld", cut);
		fake_flash_fail_at = -1;
	}
}

int main(void)
{
	test_basics();
	test_refusals();
	test_corruption();
	for (unsigned h = 0; h < 12; h++)
		for (unsigned p = 0; p < POCKET_STORE_PROJECTS; p += 4) cut_everywhere(h, p);
	test_claim();
	printf("store: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
