/*
 * clatter_pocket_fm1.c - CLATTER Pocket inside the FM-1 Open firmware. See the header.
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * NOT YET RUN ON HARDWARE. Built against the fm1_open SDK headers; see BUILDING.md and
 * HARDWARE.md for what has and has not been verified.
 *
 * Timing: the audio hook measures its own cost with the board's audio timer (the counter
 * fm1_audio_irq uses; it ticks at fm1_device_clock() Hz), against the block period, and the
 * panel shows the mean and the worst block of the last half second.
 */
#include "clatter_pocket_fm1.h"
#include "fm1_audio.h"
#include "fm1_device.h"
#include "pocket_lxr.h"
#include "pocket_panel.h"

#define STATUS_MS 500U
#define RINGOUT_MS 3000U   /* keep rendering after leaving, so hits ring out */
#define BANDS_PER_POLL 4U  /* bound the control task's LCD time per poll */

/* control -> audio */
static volatile uint8_t render_enabled;
/* audio -> control (read and reset with interrupts held) */
static volatile uint32_t ticks_sum, ticks_peak, ticks_blocks;

static uint8_t initialised, active, entering;
static uint32_t block_ticks, exit_ms, status_ms;
static struct pocket_panel panel;
static struct pocket_panel_status status;
static uint16_t band_pixels[POCKET_SCREEN_W * POCKET_BAND_ROWS] __attribute__((aligned(32)));

static inline __attribute__((always_inline)) uint32_t elapsed(uint32_t start, uint32_t end)
{
	return end >= start ? end - start : end + FM1_AUDIO_TIMER_PERIOD + 1U - start;
}

FM1_AUDIO_CODE void clatter_pocket_fm1_render(int32_t* output)
{
	if (!render_enabled) return;
	const uint32_t start = FM1_AUDIO_TIMER_COUNT;
	pocket_lxr_render_q23(output, FM1_BLOCK_FRAMES, 1);
	const uint32_t used = elapsed(start, FM1_AUDIO_TIMER_COUNT);
	ticks_sum += used;
	ticks_blocks++;
	if (used > ticks_peak) ticks_peak = used;
}

static void update_status(uint32_t now)
{
	if (now - status_ms < STATUS_MS) return;
	status_ms = now;
	uint32_t flags = fm1_device_irq_save();
	const uint32_t sum = ticks_sum, peak = ticks_peak, blocks = ticks_blocks;
	ticks_sum = ticks_peak = ticks_blocks = 0;
	struct pocket_lxr_stats engine;
	pocket_lxr_stats_read(&engine);
	fm1_device_irq_restore(flags);
	struct fm1_audio_stats dma;
	fm1_audio_stats_read(&dma);
	struct pocket_panel_status next = status;
	if (blocks && block_ticks)
	{
		next.cpu_permille = (uint16_t)((uint64_t)sum * 1000U / blocks / block_ticks);
		next.cpu_peak_permille = (uint16_t)((uint64_t)peak * 1000U / block_ticks);
	}
	next.overruns = dma.overruns;
	next.clipped = engine.clipped_samples;
	next.dropped = engine.events_dropped;
	next.state = POCKET_STATE_READY;
	pocket_panel_update_status(&panel, &status, &next);
}

static void paint(void)
{
	unsigned drawn = 0;
	for (unsigned b = 0; b < POCKET_BANDS && drawn < BANDS_PER_POLL; b++)
	{
		if (!(panel.dirty & (1u << b))) continue;
		if (fm1_screen_pending()) return; /* a stock DMA transfer is still running */
		pocket_panel_draw_band(&panel, &status, b, band_pixels);
		if (fm1_screen_blit(0, b * POCKET_BAND_ROWS, POCKET_SCREEN_W, POCKET_BAND_ROWS, band_pixels,
		                    POCKET_SCREEN_W * POCKET_BAND_ROWS) == 0)
			panel.dirty &= (uint16_t)~(1u << b);
		drawn++;
	}
}

static void enter(uint32_t now)
{
	if (!initialised)
	{
		/* The audio hook does nothing until render_enabled, so the engine can be set up
		   here, on the control task, while the stock instrument plays. */
		uint32_t clock = fm1_device_clock();
		block_ticks = (uint32_t)((uint64_t)clock * FM1_BLOCK_FRAMES / FM1_AUDIO_HARDWARE_RATE);
		pocket_lxr_init(now ? now : 1);
		pocket_lxr_set_dc_block(1);
		initialised = 1;
	}
	pocket_panel_init(&panel);
	status.state = POCKET_STATE_READY;
	status_ms = now - STATUS_MS;
	active = 1;
	fm1_device_fence();
	render_enabled = 1;
}

int clatter_pocket_fm1_control(const struct fm1_controls* controls, uint32_t previous_buttons)
{
	const uint32_t now = fm1_millis();
	if (!active)
	{
		if (render_enabled && now - exit_ms >= RINGOUT_MS) render_enabled = 0;
		if (POCKET_PANEL_TOGGLE(previous_buttons, controls->buttons)) entering = 1;
		if (!entering) return 0;
		/* take the panel only when the stock UI's LCD transfer has finished */
		if (fm1_screen_pending()) return 1;
		entering = 0;
		enter(now);
	}
	struct pocket_panel_input in;
	in.keys = controls->keys;
	in.buttons = controls->buttons;
	for (unsigned i = 0; i < POCKET_ENCODERS; i++) in.encoders[i] = controls->encoders[i];
	if (pocket_panel_apply(&panel, &in))
	{
		active = 0;
		exit_ms = now;
		return 2;
	}
	update_status(now);
	paint();
	return 1;
}
