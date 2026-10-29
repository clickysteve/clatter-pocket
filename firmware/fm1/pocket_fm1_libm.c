/*
 * pocket_fm1_libm.c - the two math functions the PI32v2 FPU and the FM-1 SDK lack.
 *
 * CLATTER's shim (lxr/shim/libc/math.h) defines sqrtf and fmaf inline, but PI32v2 clang
 * recognises both names and emits calls to the library functions, which the SDK's libc.a
 * does not have. The FPU (-mfprev1) has single-precision add, multiply and divide only: no
 * square root, no fused multiply-add, no double precision (checked with the toolchain).
 *
 * fmaf: exactly the shim's definition, so results match CLATTER and the host build. Its
 * double arithmetic is software on the FM-1; with --single-precision it is float.
 * sqrtf: correctly rounded, integer arithmetic only. The LXR calls it at trigger time.
 * host/tests/test_fm1_libm.c checks both against the host's libm.
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdint.h>

float fmaf(float a, float b, float c)
{
	return (float)((double)a * (double)b + (double)c);
}

float sqrtf(float x)
{
	union { float f; uint32_t u; } v = { x };
	uint32_t bits = v.u;
	uint32_t expField = (bits >> 23) & 0xff;
	if ((bits & 0x7fffffff) == 0) return x;           /* +-0 */
	if (bits >> 31) { v.u = 0x7fc00000; return v.f; } /* negative: NaN */
	if (expField == 0xff) return x;                    /* +inf, NaN */

	/* x = m * 2^e, m in [2^23, 2^24) */
	uint32_t m;
	int e;
	if (expField)
	{
		m = (bits & 0x7fffff) | 0x800000;
		e = (int)expField - 150;
	}
	else
	{
		m = bits & 0x7fffff;
		e = -149;
		while (m < 0x800000) { m <<= 1; e--; }
	}
	/* x = n * 2^k with k even and n in [2^46, 2^48): sqrt(n) in [2^23, 2^24) */
	uint64_t n;
	int k;
	if ((e - 23) & 1) { n = (uint64_t)m << 24; k = e - 24; }
	else { n = (uint64_t)m << 23; k = e - 23; }

	/* r = floor(sqrt(n)), bit by bit */
	uint64_t r = 0, rem = n, bit = (uint64_t)1 << 46;
	while (bit)
	{
		if (rem >= r + bit)
		{
			rem -= r + bit;
			r = (r >> 1) + bit;
		}
		else
			r >>= 1;
		bit >>= 2;
	}
	/* round to nearest: n > (r + 1/2)^2 <=> rem > r (no ties for integer n) */
	if (rem > r) r++;
	int half = k / 2;
	if (r == 0x1000000) { r >>= 1; half++; }
	v.u = ((uint32_t)(half + 150) << 23) | ((uint32_t)r & 0x7fffff);
	return v.f;
}
