/*
 * dump.c - render host/compare/script.txt on CLATTER Pocket's engine and write every voice's
 * output, as CLATTER's engine reports it, to stdout (for compare_clatter.mjs).
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Output: for each 64-sample block, for each of the six voices, 64 left then 64 right
 * float32 samples computed exactly as CLATTER's voiceToOutput (lxr/bridge/clatter_lxr.c):
 * (float)(int16_t)(sample * pan) / 32768.
 */
#include <stdio.h>
#include <string.h>
#include "pocket_lxr.h"

#define MAX_OPS 256

struct op { int block, kind, a, b, c, d, e, f, g; };

int main(int argc, char** argv)
{
	const char* path = argc > 1 ? argv[1] : "host/compare/script.txt";
	FILE* in = fopen(path, "r");
	if (!in) { perror(path); return 1; }
	static struct op ops[MAX_OPS];
	int n = 0, blocks = 0;
	char line[256];
	while (fgets(line, sizeof line, in))
	{
		struct op o = { 0 };
		char kind[16];
		if (line[0] == '#' || line[0] == '\n') continue;
		if (sscanf(line, "end %d", &blocks) == 1) continue;
		const int k = sscanf(line, "%d %15s %d %d %d %d %d %d %d", &o.block, kind, &o.a, &o.b, &o.c, &o.d, &o.e, &o.f, &o.g);
		if (k < 4 || n == MAX_OPS) { fprintf(stderr, "bad line: %s", line); return 1; }
		o.kind = !strcmp(kind, "param") ? 0 : !strcmp(kind, "trig") ? 1 : !strcmp(kind, "lock") ? 2 : -1;
		if (o.kind < 0) { fprintf(stderr, "bad op: %s", line); return 1; }
		ops[n++] = o;
	}
	fclose(in);

	pocket_lxr_init(1); /* CLATTER's clatter_init(1) */
	int32_t out[2 * POCKET_LXR_BLOCK];
	float voice[2][POCKET_LXR_BLOCK];
	int next = 0;
	for (int b = 0; b < blocks; b++)
	{
		for (; next < n && ops[next].block == b; next++)
		{
			const struct op* o = &ops[next];
			if (o->kind == 0) pocket_lxr_set_param(o->a, o->b);
			else if (o->kind == 1) pocket_lxr_trigger(o->a, o->b, o->c);
			else pocket_lxr_trigger_locked(o->a, o->b, o->c, o->d, o->e, o->f, o->g);
		}
		pocket_lxr_render_q23(out, POCKET_LXR_BLOCK, 0);
		for (int v = 0; v < POCKET_LXR_VOICES; v++)
		{
			float pl, pr;
			const int16_t* d = pocket_lxr_voice_block(v, &pl, &pr);
			for (int i = 0; i < POCKET_LXR_BLOCK; i++)
			{
				voice[0][i] = (float)(int16_t)(d[i] * pl) * (1.f / 32768.f);
				voice[1][i] = (float)(int16_t)(d[i] * pr) * (1.f / 32768.f);
			}
			fwrite(voice, sizeof voice, 1, stdout);
		}
	}
	return 0;
}
