/*
 * test_panel.c - host tests of the Pocket panel (ui/pocket_panel.c) on the real engine, and a
 * screenshot of the panel as the FM-1 would show it (build/host/panel.ppm).
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdio.h>
#include <string.h>
#include "pocket_lxr.h"
#include "pocket_params.h"
#include "pocket_panel.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int32_t out[2 * 128];

static void poll(struct pocket_panel* p, uint32_t keys, uint32_t buttons, int e0, int e1, int e2, int e6, int* exited)
{
	struct pocket_panel_input in = { keys, buttons, { (int16_t)e0, (int16_t)e1, (int16_t)e2, 0, 0, 0, (int16_t)e6 } };
	const int r = pocket_panel_apply(p, &in);
	if (exited) *exited = r;
}

static int32_t render_peak(int blocks)
{
	int32_t peak = 0;
	for (int b = 0; b < blocks; b++)
	{
		pocket_lxr_render_q23(out, 128, 0);
		for (int i = 0; i < 256; i++) { const int32_t a = out[i] < 0 ? -out[i] : out[i]; if (a > peak) peak = a; }
	}
	return peak;
}

int main(int argc, char** argv)
{
	struct pocket_panel panel;
	int exited = 0;
	pocket_lxr_init(1);
	pocket_panel_init(&panel);

	/* buttons held on entry (HOME, ARP of the entry gesture) do nothing */
	poll(&panel, 0, (1u << POCKET_BTN_HOME) | (1u << POCKET_BTN_ARP), 0, 0, 0, 0, &exited);
	CHECK(!exited, "entry gesture read as exit");
	poll(&panel, 0, 0, 0, 0, 0, 0, &exited);
	CHECK(render_peak(10) == 0, "panel made sound without a hit");

	/* PLAY triggers the selected voice (drum 1) */
	poll(&panel, 0, 1u << POCKET_BTN_PLAY, 0, 0, 0, 0, &exited);
	CHECK(render_peak(20) > POCKET_Q23_MAX / 20, "PLAY did not trigger");
	poll(&panel, 0, 1u << POCKET_BTN_PLAY, 0, 0, 0, 0, &exited); /* held: no retrigger */
	CHECK(panel.hits == 1, "held PLAY retriggered (%u hits)", panel.hits);

	/* encoders 1-3 step pitch, decay, level of drum 1 */
	const int pitch = pocket_lxr_get_param(POCKET_PAR_COARSE1), decay = pocket_lxr_get_param(POCKET_PAR_VELOD1);
	poll(&panel, 0, 0, 5, -3, -200, 0, &exited);
	CHECK(pocket_lxr_get_param(POCKET_PAR_COARSE1) == pitch + 5, "encoder 1 does not set pitch");
	CHECK(pocket_lxr_get_param(POCKET_PAR_VELOD1) == decay - 3, "encoder 2 does not set decay");
	CHECK(pocket_lxr_get_param(POCKET_PAR_VOL1) == 0, "encoder 3 does not clamp level at 0");
	render_peak(400); /* let the first hit die away */
	CHECK(render_peak(10) == 0, "first hit still sounding");
	poll(&panel, 0, 0, 0, 0, 0, 0, &exited);
	poll(&panel, 0, 1u << POCKET_BTN_PLAY, 0, 0, 0, 0, &exited);
	CHECK(panel.hits == 2, "second PLAY did not trigger");
	CHECK(render_peak(20) == 0, "level 0 still sounds");
	poll(&panel, 0, 0, 0, 0, 100, 0, &exited);

	/* encoder 7 selects the voice; keys play it chromatically */
	poll(&panel, 0, 0, 0, 0, 0, 3, &exited);
	CHECK(panel.trigger == POCKET_TRIG_SNARE, "encoder 7 selected trigger %d", panel.trigger);
	CHECK(pocket_panel_param(&panel, POCKET_CTL_DECAY) == POCKET_PAR_VELOD4, "snare decay parameter");
	poll(&panel, 0, 0, 0, 0, 0, -4, &exited);
	CHECK(panel.trigger == POCKET_TRIG_OPEN_HAT, "encoder 7 does not wrap (%d)", panel.trigger);
	CHECK(pocket_panel_param(&panel, POCKET_CTL_DECAY) == POCKET_PAR_VELOD6_OPEN, "open hat decay parameter");
	poll(&panel, 0, 0, 0, 0, 0, 1, &exited);
	const uint32_t hits = panel.hits;
	poll(&panel, (1u << 0) | (1u << 12) | (1u << 26), 0, 0, 0, 0, 0, &exited);
	CHECK(panel.hits == hits + 3, "three keys gave %u hits", panel.hits - hits);

	/* HOME + ARP leaves */
	poll(&panel, 0, 1u << POCKET_BTN_HOME, 0, 0, 0, 0, &exited);
	CHECK(!exited, "HOME alone exits");
	poll(&panel, 0, (1u << POCKET_BTN_HOME) | (1u << POCKET_BTN_ARP), 0, 0, 0, 0, &exited);
	CHECK(exited, "HOME+ARP does not exit");

	/* screenshot */
	pocket_panel_init(&panel);
	panel.hits = 42;
	struct pocket_panel_status status = { 183, 241, 0, 0, 0 };
	static uint16_t band[POCKET_SCREEN_W * POCKET_BAND_ROWS];
	const char* path = argc > 1 ? argv[1] : "build/host/panel.ppm";
	FILE* f = fopen(path, "wb");
	if (f)
	{
		fprintf(f, "P6\n%d %d\n255\n", POCKET_SCREEN_W, POCKET_SCREEN_H);
		for (unsigned b = 0; b < POCKET_BANDS; b++)
		{
			pocket_panel_draw_band(&panel, &status, b, band);
			for (int i = 0; i < POCKET_SCREEN_W * POCKET_BAND_ROWS; i++)
			{
				const uint16_t c = band[i];
				const unsigned char rgb[3] = { (unsigned char)((c >> 11) << 3), (unsigned char)(((c >> 5) & 63) << 2), (unsigned char)((c & 31) << 3) };
				fwrite(rgb, 3, 1, f);
			}
		}
		fclose(f);
		printf("  panel screenshot: %s\n", path);
	}
	printf("panel: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
