/*
 * render.c - render demo WAVs from CLATTER Pocket's LXR engine (24-bit, 44.1 kHz stereo).
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 *   render <dir>    writes <dir>/kick_sweep.wav and <dir>/kit_demo.wav
 *
 * kick_sweep: the first milestone's voice (drum 1, init kit) at three pitches, decays and
 * levels, the controls the FM-1 build exposes. kit_demo: all seven triggers in a short
 * pattern. Rendered through the same calls and block size as the firmware.
 */
#include <stdio.h>
#include <string.h>
#include "pocket_lxr.h"
#include "pocket_params.h"
#include "wav.h"

#define MAX_FRAMES (44100 * 12)
static int32_t pcm[MAX_FRAMES * 2];

static size_t render_until(size_t at, size_t until)
{
	for (; at + HOST_BLOCK_FRAMES <= until; at += HOST_BLOCK_FRAMES)
		pocket_lxr_render_q23(pcm + 2 * at, HOST_BLOCK_FRAMES, 0);
	return at;
}

static void report(const char* path, size_t frames)
{
	struct pocket_lxr_stats s;
	pocket_lxr_stats_read(&s);
	printf("%s: %.2f s, peak %.1f dBFS, %u samples clipped\n", path, (double)frames / HOST_SAMPLE_RATE,
	       s.peak_q23 ? 20.0 * __builtin_log10((double)s.peak_q23 / POCKET_Q23_MAX) : -999.0, s.clipped_samples);
}

int main(int argc, char** argv)
{
	const char* dir = argc > 1 ? argv[1] : ".";
	char path[1024];

	pocket_lxr_init(1);
	pocket_lxr_set_dc_block(1);
	size_t at = 0;
	const int pitch[3] = { 40, 52, 64 }, decay[3] = { 25, 45, 70 }, level[3] = { 127, 90, 60 };
	for (int i = 0; i < 9; i++)
	{
		pocket_lxr_set_param(POCKET_PAR_COARSE1, pitch[i % 3]);
		pocket_lxr_set_param(POCKET_PAR_VELOD1, decay[(i / 3) % 3]);
		pocket_lxr_set_param(POCKET_PAR_VOL1, level[i / 3]);
		pocket_lxr_trigger(POCKET_TRIG_DRUM1, 127, 60);
		at = render_until(at, at + HOST_SAMPLE_RATE * 6 / 10);
	}
	snprintf(path, sizeof path, "%s/kick_sweep.wav", dir);
	if (wav_write_q23(path, pcm, at)) { perror(path); return 1; }
	report(path, at);

	/* 16-step pattern at 120 BPM: 1 kick, 2 drum 2, 3 drum 3, S snare, C cymbal, h/o hats */
	static const char* pattern = "1-h-S-ho1-h-S2h3";
	pocket_lxr_init(2);
	pocket_lxr_set_dc_block(1);
	pocket_lxr_set_master(POCKET_GAIN_UNITY * 3 / 4);
	const size_t step = HOST_SAMPLE_RATE * 60 / 120 / 4;
	at = 0;
	for (int bar = 0; bar < 4; bar++)
		for (int s = 0; s < 16; s++)
		{
			const char c = pattern[s];
			const int vel = (s % 4 == 0) ? 127 : 90;
			if (c == '1') pocket_lxr_trigger(POCKET_TRIG_DRUM1, vel, 60);
			if (c == '2') pocket_lxr_trigger(POCKET_TRIG_DRUM2, vel, 60);
			if (c == '3') pocket_lxr_trigger(POCKET_TRIG_DRUM3, vel, 60);
			if (c == 'S') pocket_lxr_trigger(POCKET_TRIG_SNARE, vel, 60);
			if (c == 'h') pocket_lxr_trigger(POCKET_TRIG_CLOSED_HAT, vel, 60);
			if (c == 'o') pocket_lxr_trigger(POCKET_TRIG_OPEN_HAT, vel, 60);
			if (bar == 3 && s == 0) pocket_lxr_trigger(POCKET_TRIG_CYMBAL, 127, 60);
			at = render_until(at, (size_t)(bar * 16 + s + 1) * step);
		}
	at = render_until(at, at + HOST_SAMPLE_RATE);
	snprintf(path, sizeof path, "%s/kit_demo.wav", dir);
	if (wav_write_q23(path, pcm, at)) { perror(path); return 1; }
	report(path, at);
	return 0;
}
