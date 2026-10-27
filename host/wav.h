/*
 * wav.h - WAV output and simple signal measurements for the host tools.
 * Copyright (C) 2026 The CLATTER contributors. SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef POCKET_HOST_WAV_H
#define POCKET_HOST_WAV_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* The FM-1's audio format (sdk/include/fm1_block.h). */
#define HOST_SAMPLE_RATE 44100
#define HOST_BLOCK_FRAMES 128

static void put16(FILE* f, uint32_t v) { fputc(v & 0xff, f); fputc((v >> 8) & 0xff, f); }
static void put32(FILE* f, uint32_t v) { put16(f, v & 0xffff); put16(f, v >> 16); }

/* Write interleaved stereo Q23 as a 24-bit PCM WAV. Returns 0 on success. */
static int wav_write_q23(const char* path, const int32_t* pcm, size_t frames)
{
	FILE* f = fopen(path, "wb");
	if (!f) return -1;
	const uint32_t data = (uint32_t)(frames * 2 * 3);
	fwrite("RIFF", 1, 4, f); put32(f, 36 + data); fwrite("WAVE", 1, 4, f);
	fwrite("fmt ", 1, 4, f); put32(f, 16); put16(f, 1); put16(f, 2);
	put32(f, HOST_SAMPLE_RATE); put32(f, HOST_SAMPLE_RATE * 6); put16(f, 6); put16(f, 24);
	fwrite("data", 1, 4, f); put32(f, data);
	for (size_t i = 0; i < frames * 2; i++)
	{
		const uint32_t s = (uint32_t)pcm[i];
		fputc(s & 0xff, f); fputc((s >> 8) & 0xff, f); fputc((s >> 16) & 0xff, f);
	}
	return fclose(f);
}

/* Largest |left| in [from, to). */
static int32_t peak_left(const int32_t* pcm, size_t from, size_t to)
{
	int32_t p = 0;
	for (size_t i = from; i < to; i++)
	{
		const int32_t a = pcm[2 * i] < 0 ? -pcm[2 * i] : pcm[2 * i];
		if (a > p) p = a;
	}
	return p;
}

/* The last frame whose |left| exceeds `threshold`, or 0. */
static size_t last_above(const int32_t* pcm, size_t frames, int32_t threshold)
{
	size_t last = 0;
	for (size_t i = 0; i < frames; i++)
		if (pcm[2 * i] > threshold || pcm[2 * i] < -threshold) last = i;
	return last;
}

/* Frequency of the left channel in [from, to) from its rising zero crossings (Hz). */
static double zero_cross_hz(const int32_t* pcm, size_t from, size_t to)
{
	size_t first = 0, last = 0, n = 0;
	for (size_t i = from + 1; i < to; i++)
		if (pcm[2 * (i - 1)] < 0 && pcm[2 * i] >= 0)
		{
			if (!n) first = i;
			last = i;
			n++;
		}
	if (n < 2) return 0.0;
	return (double)(n - 1) * HOST_SAMPLE_RATE / (double)(last - first);
}

#endif
