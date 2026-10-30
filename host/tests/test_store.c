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

static void put(unsigned slot, unsigned value)
{
	memset(pocket_store_slot(slot), (int)value, POCKET_STORE_SLOT_BYTES);
}

static int has(unsigned slot, unsigned value)
{
	const uint8_t* p = pocket_store_slot(slot);
	for (unsigned i = 0; i < POCKET_STORE_SLOT_BYTES; i++)
		if (p[i] != (uint8_t)value) return 0;
	return 1;
}

static void test_basics(void)
{
	fake_flash_reset();
	fill_outside();
	CHECK(pocket_store_open(POCKET_STORE_KITS) == POCKET_STORE_OK, "blank opens");
	CHECK(fake_flash_ops == 0, "opening writes nothing");
	CHECK(has(0, 0) && has(POCKET_STORE_SLOTS - 1, 0), "a blank store is empty");
	put(0, 11);
	CHECK(pocket_store_commit() == POCKET_STORE_OK, "a save");
	put(3, 33);
	CHECK(pocket_store_commit() == POCKET_STORE_OK, "a second save");
	CHECK(pocket_store_open(POCKET_STORE_PATTERNS) == POCKET_STORE_OK, "the other store");
	CHECK(has(0, 0), "stores are separate");
	put(0, 99);
	CHECK(pocket_store_commit() == POCKET_STORE_OK, "a pattern save");
	CHECK(pocket_store_open(POCKET_STORE_KITS) == POCKET_STORE_OK && has(0, 11) && has(3, 33) && has(1, 0), "both saves are read back");
	CHECK(pocket_store_open(POCKET_STORE_PATTERNS) == POCKET_STORE_OK && has(0, 99), "the pattern is");
	CHECK(outside_intact(), "only our sectors were written");

	/* many saves: alternating sectors, generations in order, past a wrap of neither sector */
	pocket_store_open(POCKET_STORE_KITS);
	long before = fake_flash_ops;
	for (unsigned i = 0; i < 300; i++)
	{
		put(5, i & 255u);
		CHECK(pocket_store_commit() == POCKET_STORE_OK, "save %u", i);
		pocket_store_open(POCKET_STORE_KITS);
		CHECK(has(5, i & 255u) && has(0, 11), "save %u reads back", i);
	}
	CHECK((fake_flash_ops - before) / 300 < 20, "a save is a page count, not a sweep: %ld", (fake_flash_ops - before) / 300);
	CHECK(outside_intact(), "still only ours");
}

/* What refuses, and that refusing touches nothing. */
static void test_refusals(void)
{
	fake_flash_reset();
	fill_outside();
	fake_flash_size = 0;
	CHECK(pocket_store_open(POCKET_STORE_KITS) == POCKET_STORE_UNAVAILABLE, "no reservation");
	fake_flash_size = 512000;
	CHECK(pocket_store_open(POCKET_STORE_KITS) == POCKET_STORE_UNAVAILABLE, "another layout");
	fake_flash_size = FAKE_FLASH_BYTES;

	/* a byte anywhere in a sector is someone else's data: nothing of ours may erase it */
	const uint32_t spots[] = { 0, 1, 40, 2000, 4095 };
	for (unsigned s = 0; s < 5; s++)
		for (unsigned sector = 0; sector < 2; sector++)
		{
			memset(fake_flash + FIRST, 0xff, SECTORS);
			fake_flash[FIRST + sector * SECTOR + spots[s]] = 0x12;
			const long ops = fake_flash_ops;
			CHECK(pocket_store_open(POCKET_STORE_KITS) == POCKET_STORE_OCCUPIED, "foreign data in sector %u at %u", sector, spots[s]);
			CHECK(pocket_store_open(POCKET_STORE_PATTERNS) == POCKET_STORE_OK, "the other store is unaffected");
			CHECK(pocket_store_open(POCKET_STORE_KITS) == POCKET_STORE_OCCUPIED, "and this one stays closed");
			CHECK(fake_flash_ops == ops && fake_flash[FIRST + sector * SECTOR + spots[s]] == 0x12, "nothing written");
		}
	/* a sample bank long enough to cover our sectors, even if they are blank in it */
	memset(fake_flash + FIRST, 0xff, SECTORS);
	uint32_t bank[3] = { 0x53314d46u, 1, 400000 };
	memcpy(fake_flash + SAMPLE, bank, sizeof bank);
	CHECK(pocket_store_open(POCKET_STORE_KITS) == POCKET_STORE_OCCUPIED, "a bank that reaches us");
	bank[2] = 360448; /* exactly up to them */
	memcpy(fake_flash + SAMPLE, bank, sizeof bank);
	CHECK(pocket_store_open(POCKET_STORE_KITS) == POCKET_STORE_OK, "a bank that stops short");

	/* a failure Flash reports is not a save */
	fake_flash_reset();
	fill_outside();
	pocket_store_open(POCKET_STORE_KITS);
	put(0, 5);
	fake_flash_fail_at = fake_flash_ops + 3;
	CHECK(pocket_store_commit() == POCKET_STORE_FAILED, "Flash reports a failure");
	fake_flash_fail_at = -1;
	CHECK(pocket_store_open(POCKET_STORE_KITS) == POCKET_STORE_DAMAGED || pocket_store_open(POCKET_STORE_KITS) == POCKET_STORE_OK, "and it opens after");
	put(0, 6);
	CHECK(pocket_store_commit() == POCKET_STORE_OK, "and a later save works");
	CHECK(pocket_store_open(POCKET_STORE_KITS) == POCKET_STORE_OK && has(0, 6), "and stands");
}

/* A corrupted sector is passed over: the older bank is there. Both, and nothing is shown. */
static void test_corruption(void)
{
	fake_flash_reset();
	fill_outside();
	pocket_store_open(POCKET_STORE_KITS);
	put(0, 1);
	pocket_store_commit();
	put(0, 2);
	pocket_store_commit();
	for (uint32_t at = 0; at < 3200; at += 37)
	{
		for (int s = 0; s < 2; s++)
		{
			const uint8_t was = fake_flash[FIRST + (uint32_t)s * SECTOR + at];
			fake_flash[FIRST + (uint32_t)s * SECTOR + at] = (uint8_t)(was ^ 0x10);
			const int r = pocket_store_open(POCKET_STORE_KITS);
			CHECK(r >= 0, "a flipped bit at %u of sector %d: %d", at, s, r);
			CHECK(has(0, 1) || has(0, 2), "a bank is read whole, never a mixture");
			fake_flash[FIRST + (uint32_t)s * SECTOR + at] = was;
		}
	}
	/* both corrupted: nothing is believed, and a save is still possible */
	fake_flash[FIRST + 100] ^= 1;
	fake_flash[FIRST + SECTOR + 100] ^= 1;
	CHECK(pocket_store_open(POCKET_STORE_KITS) == POCKET_STORE_DAMAGED, "both bad");
	CHECK(has(0, 0), "shows empty, not damaged data");
	put(0, 7);
	CHECK(pocket_store_commit() == POCKET_STORE_OK, "they are ours to replace");
	CHECK(pocket_store_open(POCKET_STORE_KITS) == POCKET_STORE_OK && has(0, 7), "the new save stands");
	CHECK(outside_intact(), "still only ours");
}

/* A power cut at every operation of a save, of several histories. After it: the old bank or the
 * new one, whole, and the store still works. */
static void cut_everywhere(unsigned saves_before)
{
	/* how many operations a save takes from this state */
	fake_flash_reset();
	fill_outside();
	pocket_store_open(POCKET_STORE_KITS);
	for (unsigned i = 0; i < saves_before; i++) { put(0, 10 + i); put(1, 50); pocket_store_commit(); }
	const long from = fake_flash_ops;
	put(0, 200);
	pocket_store_commit();
	const long ops = fake_flash_ops - from;
	CHECK(ops >= 2, "a save has operations");
	int older = 0, newer = 0;
	for (long cut = 0; cut < ops + 1; cut++)
	{
		fake_flash_reset();
		fill_outside();
		pocket_store_open(POCKET_STORE_KITS);
		for (unsigned i = 0; i < saves_before; i++) { put(0, 10 + i); put(1, 50); pocket_store_commit(); }
		fake_flash_cut_at = fake_flash_ops + cut;
		put(0, 200);
		(void)pocket_store_commit();
		/* power comes back */
		fake_flash_cut_at = -1;
		fake_flash_power_on();
		const int r = pocket_store_open(POCKET_STORE_KITS);
		const unsigned expected = saves_before ? 9 + saves_before : 0;
		if (saves_before) CHECK(r == POCKET_STORE_OK, "cut %ld of %ld: opens: %d", cut, ops, r);
		else CHECK(r >= 0, "cut %ld of %ld of the first save: opens: %d", cut, ops, r);
		const int isnew = has(0, 200), isold = has(0, expected);
		CHECK(isnew || isold, "cut %ld: the old bank or the new one", cut);
		older += isold;
		newer += isnew;
		if (saves_before) CHECK(has(1, 50) , "cut %ld: the rest of the bank is there", cut);
		CHECK(outside_intact(), "cut %ld: nothing else touched", cut);
		put(0, 201);
		CHECK(pocket_store_commit() == POCKET_STORE_OK, "cut %ld: and it saves again", cut);
		CHECK(pocket_store_open(POCKET_STORE_KITS) == POCKET_STORE_OK && has(0, 201), "cut %ld: that stands", cut);
	}
	CHECK(older > 0 && newer > 0, "some cuts land before the commit, and some after: %d %d", older, newer);
}

int main(void)
{
	test_basics();
	test_refusals();
	test_corruption();
	for (unsigned h = 0; h < 4; h++) cut_everywhere(h);
	printf("store: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
