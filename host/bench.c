/*
 * bench.c - host timing of CLATTER Pocket's LXR engine per FM-1 audio block.
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Renders FM-1-sized blocks (128 frames, 2.902 ms at 44.1 kHz) in several load scenarios and
 * reports the time per block on THIS machine. It is a relative measure (which scenario costs
 * what, whether a change made the engine slower); it says nothing direct about the FM-1,
 * whose CPU, FPU, caches and memory differ. Device timing comes from the firmware's own
 * counters (firmware/fm1/clatter_pocket_fm1.c).
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "pocket_lxr.h"
#include "pocket_params.h"

#define FRAMES 128
#define BLOCK_NS (1e9 * FRAMES / 44100.0)
#define SECONDS 20

static uint64_t now_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static int cmp(const void* a, const void* b)
{
	const uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
	return x < y ? -1 : x > y;
}

static uint64_t samples[SECONDS * 44100 / FRAMES];

/* voices: bit mask of triggers to retrigger every `period` blocks; long decays keep them sounding */
static void run(const char* name, unsigned voices, int period)
{
	pocket_lxr_init(1);
	pocket_lxr_set_dc_block(1);
	pocket_lxr_set_param(POCKET_PAR_VELOD1, 90);
	pocket_lxr_set_param(POCKET_PAR_VELOD2, 90);
	pocket_lxr_set_param(POCKET_PAR_VELOD3, 90);
	pocket_lxr_set_param(POCKET_PAR_VELOD4, 90);
	pocket_lxr_set_param(POCKET_PAR_VELOD5, 90);
	pocket_lxr_set_param(POCKET_PAR_VELOD6_OPEN, 90);
	static int32_t out[2 * FRAMES];
	const int n = (int)(sizeof samples / sizeof *samples);
	for (int b = 0; b < n; b++)
	{
		if (voices && b % period == 0)
			for (int t = 0; t < POCKET_LXR_TRIGGERS; t++)
				if (voices & (1u << t)) pocket_lxr_trigger(t, 120, 60);
		const uint64_t t0 = now_ns();
		pocket_lxr_render_q23(out, FRAMES, 0);
		samples[b] = now_ns() - t0;
	}
	uint64_t sum = 0;
	for (int b = 0; b < n; b++) sum += samples[b];
	qsort(samples, (size_t)n, sizeof *samples, cmp);
	const double mean = (double)sum / n;
	printf("%-34s mean %6.1f us  p99 %6.1f us  max %6.1f us  = %5.2f %% of real time (mean)\n", name,
	       mean / 1e3, samples[n * 99 / 100] / 1e3, samples[n - 1] / 1e3, 100.0 * mean / BLOCK_NS);
}

int main(void)
{
	printf("host engine time per FM-1 block (%d frames, %.0f us real time), %d s each\n", FRAMES, BLOCK_NS / 1e3, SECONDS);
	run("idle (no voice sounding)", 0, 1);
	run("kick (drum 1)", 1u << POCKET_TRIG_DRUM1, 172);
	run("drums 1-3", 7u, 172);
	run("all six voices (open hat)", 0x5fu, 86);
	run("all six voices, retriggered 1/16", 0x5fu, 22);
	return 0;
}
