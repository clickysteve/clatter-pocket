/*
 * test_fm1_libm.c - the FM-1 build's sqrtf and fmaf (firmware/fm1/pocket_fm1_libm.c)
 * against the host's libm: sqrtf bit-exact (correctly rounded) on every special value,
 * every exponent and random inputs; fmaf identical to CLATTER's shim definition.
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define sqrtf pocket_sqrtf
#define fmaf pocket_fmaf
#include "../../firmware/fm1/pocket_fm1_libm.c"
#undef sqrtf
#undef fmaf

static uint32_t bitsOf(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float floatOf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint32_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }

static int failures, checks;
static void check(int ok, const char* what)
{
	checks++;
	if (!ok) { failures++; printf("FAIL %s\n", what); }
}

static int sameSqrt(uint32_t u)
{
	float x = floatOf(u), a = pocket_sqrtf(x), b = sqrtf(x);
	if (isnan(b)) return isnan(a);
	return bitsOf(a) == bitsOf(b);
}

int main(void)
{
	const uint32_t specials[] = { 0, 0x80000000, 1, 2, 3, 0x7fffff, 0x800000, 0x800001, 0x3f800000,
		0x40000000, 0x40800000, 0x3f7fffff, 0x7f7fffff, 0x7f800000, 0xff800000, 0x7fc00000,
		0xbf800000, 0x80000001, 0x4b800000 };
	int bad = 0;
	for (unsigned i = 0; i < sizeof specials / sizeof *specials; i++) bad += !sameSqrt(specials[i]);
	check(bad == 0, "sqrtf special values");

	bad = 0;
	for (uint32_t ex = 0; ex < 255; ex++)
		for (uint32_t j = 0; j < 4096; j++)
			bad += !sameSqrt((ex << 23) | (next() & 0x7fffff));
	check(bad == 0, "sqrtf every exponent, 4096 mantissas each");

	bad = 0;
	for (uint32_t j = 0; j < 20000000; j++) bad += !sameSqrt(next() & 0x7fffffff);
	check(bad == 0, "sqrtf 20M random positive floats");

	bad = 0;
	for (uint32_t j = 0; j < 10000000; j++)
	{
		float a = floatOf(next() % 0x7f000000), b = floatOf(next() % 0x7f000000), c = floatOf(next());
		if (next() & 1) a = -a;
		float ref = (float)((double)a * (double)b + (double)c);
		float got = pocket_fmaf(a, b, c);
		if (!(bitsOf(got) == bitsOf(ref) || (isnan(got) && isnan(ref)))) bad++;
	}
	check(bad == 0, "fmaf equals CLATTER's shim definition (10M random)");

	printf("%d/%d checks passed (FM-1 libm)\n", checks - failures, checks);
	return failures != 0;
}
