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

/* A bank of firmware before projects, as it was written. */
static void old_bank(unsigned kind, unsigned copy, uint32_t generation, unsigned value)
{
	static uint8_t sector[SECTOR];
	const uint32_t magic = 0x31535043u, commit = 0, format = 1u << 8 | kind;
	memset(sector, 0xff, sizeof sector);
	memset(sector + 20, 0, 12);
	memcpy(sector, &magic, 4);
	memcpy(sector + 4, &commit, 4);
	memcpy(sector + 12, &format, 4);
	memcpy(sector + 16, &generation, 4);
	memset(sector + 32, (int)value, 12 * 256);
	const uint32_t crc = pocket_crc32(0, sector + 12, 3104 - 12);
	memcpy(sector + 8, &crc, 4);
	memcpy(fake_flash + FIRST + (2 * kind + copy) * SECTOR, sector, sizeof sector);
}

static void test_basics(void)
{
	fake_flash_reset();
	fill_outside();
	CHECK(pocket_store_open() == POCKET_STORE_OK, "blank opens");
	CHECK(fake_flash_ops == 0, "opening writes nothing");
	CHECK(pocket_store_room() == POCKET_STORE_PROJECTS, "all of it is free: %u", pocket_store_room());
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
	CHECK(pocket_store_open() == POCKET_STORE_OCCUPIED, "a bank that reaches us");
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

/* Saves from before projects. */
static void old_chip(void)
{
	fake_flash_reset();
	fill_outside();
	old_bank(0, 0, 3, 0x21);
	old_bank(0, 1, 4, 0x22); /* the newer of its two */
	old_bank(1, 1, 9, 0x33);
}

static int old_intact(void)
{
	uint8_t a, b;
	pocket_store_old_peek(POCKET_STORE_OLD_KITS, 5, &a, 1);
	pocket_store_old_peek(POCKET_STORE_OLD_PATTERNS, 11, &b, 1);
	return a == 0x22 && b == 0x33;
}

static void test_old_banks(void)
{
	static uint8_t chip[SECTORS];
	old_chip();
	CHECK(pocket_store_open() == POCKET_STORE_OK, "old banks open");
	CHECK(pocket_store_old(POCKET_STORE_OLD_KITS) && pocket_store_old(POCKET_STORE_OLD_PATTERNS), "both are found");
	CHECK(old_intact(), "the newer copy of each is what is read");
	CHECK(pocket_store_room() == 3, "room for one sector of projects: %u", pocket_store_room());
	memcpy(chip, fake_flash + FIRST, sizeof chip);
	/* saves go only where nothing would be lost */
	for (unsigned i = 0; i < 50; i++)
	{
		CHECK(save(i % 3, 60 + i) == POCKET_STORE_OK, "a project beside old banks");
		CHECK(!memcmp(chip + SECTOR, fake_flash + FIRST + SECTOR, SECTOR) && !memcmp(chip + 3 * SECTOR, fake_flash + FIRST + 3 * SECTOR, SECTOR), "the current old banks are byte for byte as they were");
		pocket_store_open();
		CHECK(has(i % 3, 60 + i) && old_intact(), "both read");
	}
	/* a bad old bank is not believed, and one of a pair standing is enough */
	old_chip();
	fake_flash[FIRST + SECTOR + 700] ^= 1;
	CHECK(pocket_store_open() == POCKET_STORE_OK, "the newer copy corrupted: %d", pocket_store_open());
	uint8_t a;
	pocket_store_old_peek(POCKET_STORE_OLD_KITS, 5, &a, 1);
	CHECK(a == 0x21, "the older copy is read: %x", a);
	/* a cut at every operation of a save beside them */
	old_chip();
	pocket_store_open();
	save(0, 9);
	const long from = fake_flash_ops;
	save(0, 10);
	const long ops = fake_flash_ops - from;
	for (long cut = 0; cut <= ops; cut++)
	{
		old_chip();
		pocket_store_open();
		save(0, 9);
		fake_flash_cut_at = fake_flash_ops + cut;
		save(0, 10);
		fake_flash_cut_at = -1;
		fake_flash_power_on();
		CHECK(pocket_store_open() >= 0, "beside old banks, cut %ld: opens", cut);
		CHECK(has(0, 9) || has(0, 10), "cut %ld: whole", cut);
		CHECK(old_intact(), "cut %ld: old banks stand", cut);
		CHECK(save(0, 11) == POCKET_STORE_OK, "cut %ld: saves", cut);
		CHECK(outside_intact(), "cut %ld: nothing else", cut);
	}
	/* forgetting them, cut at every operation, with and without a project to vouch */
	for (unsigned projects = 0; projects < 2; projects++)
	{
		old_chip();
		pocket_store_open();
		if (projects) save(1, 44);
		const long start = fake_flash_ops;
		CHECK(pocket_store_forget_old() == POCKET_STORE_OK, "forgotten");
		const long steps = fake_flash_ops - start;
		CHECK(pocket_store_open() == POCKET_STORE_OK && !pocket_store_old(0) && !pocket_store_old(1), "gone");
		CHECK(pocket_store_room() == POCKET_STORE_PROJECTS, "every project is free: %u", pocket_store_room());
		if (projects) CHECK(has(1, 44), "and a project beside them stays");
		for (long cut = 0; cut <= steps; cut++)
		{
			old_chip();
			pocket_store_open();
			if (projects) save(1, 44);
			fake_flash_cut_at = fake_flash_ops + cut;
			(void)pocket_store_forget_old();
			fake_flash_cut_at = -1;
			fake_flash_power_on();
			CHECK(pocket_store_open() >= 0, "forgetting, cut %ld: opens", cut);
			if (projects) CHECK(has(1, 44), "cut %ld: the project stands", cut);
			CHECK(pocket_store_forget_old() == POCKET_STORE_OK, "cut %ld: and it can finish", cut);
			CHECK(pocket_store_open() == POCKET_STORE_OK && pocket_store_room() == POCKET_STORE_PROJECTS, "cut %ld: finished: %u", cut, pocket_store_room());
			CHECK(save(8, 3) == POCKET_STORE_OK, "cut %ld: every project saves", cut);
			CHECK(outside_intact(), "cut %ld: nothing else", cut);
		}
	}
	/* something foreign beside old banks is still foreign */
	old_chip();
	fake_flash[FIRST + 2 * SECTOR + 9] = 0x12; /* the pattern bank's other sector is its partner: a cut save */
	CHECK(pocket_store_open() >= 0, "a sector beside its bank's good copy is that bank's cut save");
	memset(fake_flash + FIRST, 0xff, SECTORS);
	old_bank(1, 0, 1, 0x33);
	fake_flash[FIRST + 9] = 0x12;
	CHECK(pocket_store_open() == POCKET_STORE_OCCUPIED, "a sector with nothing of ours beside it is not");
}

int main(void)
{
	test_basics();
	test_refusals();
	test_corruption();
	for (unsigned h = 0; h < 12; h++)
		for (unsigned p = 0; p < POCKET_STORE_PROJECTS; p += 4) cut_everywhere(h, p);
	test_old_banks();
	printf("store: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
