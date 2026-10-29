/*
 * clatter_pocket_app.c - CLATTER Pocket as the FM-1's instrument (firmware/fm1/build.sh
 * --replace). Replaces fm1_open's instrument application (app/screen.c, project.c,
 * presets.c, samples.c, transfer.c, midi_router.c, fm_library.c, fm_factory.c) and keeps
 * its board layer (app/board/: audio DMA, panel scan, LCD, device) and output mixing
 * (app/audio_output.c: master gain, USB audio in/out, Bluetooth audio).
 *
 * The binary SDK's control loop calls the application through the groove_* hooks below.
 * CLATTER Pocket implements the panel, audio and MIDI hooks; the project, sample, transfer
 * and DX7-library hooks are inert. Nothing here reads or writes the user Flash partition:
 * the stock instrument's projects, samples and voices stay where they are, untouched, for
 * a later return to the stock firmware. Firmware updates are handled by the SDK before
 * fm1_app_sysex is called and are unaffected.
 *
 * NOT RUN ON HARDWARE. See BUILDING.md, "Replacement build".
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 */
#include <string.h>
#include "fm1.h"
#include "fm1_audio.h"
#include "fm1_device.h"
#include "instrument.h"
#include "fm_patch.h"
#include "clatter_pocket_app.h"
#include "pocket_lxr.h"
#include "pocket_panel.h"

/* MIDI ring ordering, as the engine's event ring (pocket_fm1_config.h) */
#ifndef POCKET_BARRIER
#define POCKET_BARRIER() __asm__ volatile("csync" ::: "memory")
#endif

#define STATUS_MS 500U
#define BANDS_PER_POLL 4U
#define MIDI_RING 64U /* power of two */

/* control -> audio */
static volatile uint8_t render_enabled, output_level;
/* audio -> control (read and reset with interrupts held) */
static volatile uint32_t ticks_sum, ticks_peak, ticks_blocks;

static uint32_t block_ticks, status_ms;
/* Bluetooth as the stock firmware's defaults: on, music and MIDI, BLE Peripheral.
   2: configure pending, 1: role pending, 0: done (retried while the stack is busy). */
static uint8_t radio_pending = 2;
static unsigned knob_level = ~0U;
static struct pocket_panel panel;
static struct pocket_panel_status status;
static uint16_t band_pixels[POCKET_SCREEN_W * POCKET_BAND_ROWS] __attribute__((aligned(32)));

/* MIDI routing task -> control task (single producer, single consumer) */
static uint8_t midi_trigger[MIDI_RING], midi_velocity[MIDI_RING], midi_note[MIDI_RING];
static volatile uint32_t midi_head, midi_tail;

static inline __attribute__((always_inline)) uint32_t elapsed(uint32_t start, uint32_t end)
{
	return end >= start ? end - start : end + FM1_AUDIO_TIMER_PERIOD + 1U - start;
}

/* ---- audio (I2S interrupt) ---- */

FM1_AUDIO_CODE int clatter_pocket_render(int32_t* output)
{
	if (!render_enabled)
	{
		for (unsigned i = 0; i < FM1_BLOCK_WORDS; i++) output[i] = 0;
		return 0;
	}
	const uint32_t start = FM1_AUDIO_TIMER_COUNT;
	pocket_lxr_render_q23(output, FM1_BLOCK_FRAMES, 0);
	const uint32_t used = elapsed(start, FM1_AUDIO_TIMER_COUNT);
	ticks_sum += used;
	ticks_blocks++;
	if (used > ticks_peak) ticks_peak = used;
	return 0;
}

FM1_AUDIO_CODE unsigned clatter_pocket_output_level(void) { return output_level; }

/* ---- panel (control task) ---- */

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
	if (memcmp(&next, &status, sizeof next)) panel.dirty |= POCKET_PANEL_STATUS_BANDS;
	status = next;
}

static void paint(void)
{
	unsigned drawn = 0;
	for (unsigned b = 0; b < POCKET_BANDS && drawn < BANDS_PER_POLL; b++)
	{
		if (!(panel.dirty & (1u << b))) continue;
		if (fm1_screen_pending()) return;
		pocket_panel_draw_band(&panel, &status, b, band_pixels);
		if (fm1_screen_blit(0, b * POCKET_BAND_ROWS, POCKET_SCREEN_W, POCKET_BAND_ROWS, band_pixels,
		                    POCKET_SCREEN_W * POCKET_BAND_ROWS) == 0)
			panel.dirty &= (uint16_t)~(1u << b);
		drawn++;
	}
}

/* MASTER knob, ADC 0..1023 -> output level 0..127 (audio_output.c ramps to it), with a
   small dead band so ADC noise does not move the level. */
static void master(unsigned adc)
{
	if (adc > 1023U) adc = 1023U;
	if (knob_level != ~0U && (adc + 4U > knob_level && adc < knob_level + 4U)) return;
	knob_level = adc;
	output_level = (uint8_t)((adc * 127U + 511U) / 1023U);
}

static void radio(void)
{
	if (radio_pending == 2 && !fm1_bluetooth_configure(FM1_BT_AUDIO | FM1_BT_MIDI)) radio_pending = 1;
	if (radio_pending == 1)
	{
		const int r = fm1_ble_set_role(FM1_BLE_PERIPHERAL);
		if (!r || (r != -2 && r != -3)) radio_pending = 0;
	}
}

static void drain_midi(void)
{
	uint32_t tail = midi_tail;
	const uint32_t head = midi_head;
	POCKET_BARRIER();
	while (tail != head)
	{
		const unsigned i = tail & (MIDI_RING - 1U);
		pocket_lxr_trigger(midi_trigger[i], midi_velocity[i], midi_note[i]);
		tail++;
	}
	POCKET_BARRIER();
	midi_tail = tail;
}

void groove_screen_init(void)
{
	fm1_controls_init();
	for (unsigned key = 0; key < 41; key++) fm1_led_set(key, 0);
	fm1_screen_init();
	const uint32_t now = fm1_millis();
	block_ticks = (uint32_t)((uint64_t)fm1_device_clock() * FM1_BLOCK_FRAMES / FM1_AUDIO_HARDWARE_RATE);
	pocket_lxr_init(now ? now : 1);
	pocket_lxr_set_dc_block(1);
	pocket_panel_init(&panel);
	status_ms = now - STATUS_MS;
	fm1_device_fence();
	render_enabled = 1;
	paint();
}

void groove_screen_control(void)
{
	struct fm1_controls controls;
	fm1_controls_read(&controls);
	master(controls.knob);
	radio();
	drain_midi();
	struct pocket_panel_input in;
	in.keys = controls.keys;
	in.buttons = controls.buttons;
	for (unsigned i = 0; i < POCKET_ENCODERS; i++) in.encoders[i] = controls.encoders[i];
	(void)pocket_panel_apply(&panel, &in); /* HOME+ARP has nowhere to go back to */
	update_status(fm1_millis());
	paint();
}

/* ---- MIDI ---- */

/* General MIDI drum notes on any channel -> the seven LXR triggers. */
static int midi_drum_trigger(unsigned note)
{
	switch (note)
	{
	case 35: case 36: return 0;           /* DRUM 1 */
	case 41: case 43: case 45: return 1;  /* DRUM 2 (low toms) */
	case 47: case 48: case 50: return 2;  /* DRUM 3 (high toms) */
	case 37: case 38: case 39: case 40: return 3; /* SNARE (rim, snare, clap) */
	case 49: case 51: case 52: case 55: case 57: case 59: return 4; /* CYMBAL */
	case 42: case 44: return 5;           /* CLOSED HAT */
	case 46: return 6;                    /* OPEN HAT */
	default: return -1;
	}
}

/* Routing task: queue only (no engine access, no blocking). */
void fm1_app_midi_received(unsigned source, const uint8_t* m, size_t n)
{
	(void)source;
	if (n < 3 || (m[0] & 0xf0) != 0x90 || !m[2]) return;
	const int trigger = midi_drum_trigger(m[1]);
	if (trigger < 0) return;
	const uint32_t head = midi_head;
	if (head - midi_tail >= MIDI_RING) return;
	const unsigned i = head & (MIDI_RING - 1U);
	midi_trigger[i] = (uint8_t)trigger;
	midi_velocity[i] = m[2];
	midi_note[i] = 0; /* the voice's programmed pitch */
	POCKET_BARRIER();
	midi_head = head + 1U;
	fm1_control_request();
}

/* Application SysEx: none. (The SDK handles firmware update messages before this hook.) */
void fm1_app_sysex(const uint8_t* message, size_t bytes)
{
	(void)message;
	(void)bytes;
}

/* The SDK's sequencer and MIDI clock call these from the audio context. CLATTER Pocket does
   not start that sequencer; they are inert. */
FM1_DSP_CODE unsigned groove_midi_mode(unsigned track) { (void)track; return 0; }
FM1_DSP_CODE void groove_midi_note(unsigned track, unsigned note, unsigned velocity) { (void)track; (void)note; (void)velocity; }
FM1_DSP_CODE void groove_midi_track_reset(unsigned track) { (void)track; }
FM1_DSP_CODE void groove_midi_transport(unsigned owner, uint8_t s) { (void)owner; (void)s; }
FM1_DSP_CODE void groove_midi_clock(unsigned owner, unsigned tempo) { (void)owner; (void)tempo; }
FM1_DSP_CODE void groove_fm_program(uint8_t patch[156], unsigned program)
{
	(void)program;
	for (unsigned i = 0; i < 156; i++) patch[i] = 0;
}
void groove_midi_init(void) {}
void groove_midi_poll(void) {}
void groove_midi_panic(void) {}
void groove_midi_clock_reset(void) {}

/* ---- stock instrument storage: inert, user Flash untouched ---- */

static struct groove_project scratch;
struct groove_project* groove_project_scratch(void) { return &scratch; }
void groove_project_init(void) {}
void groove_project_poll(void) {}
void groove_project_metadata(const struct groove_project* p) { (void)p; }
void groove_project_name(char name[13]) { memcpy(name, "CLATTER     ", 13); }
void groove_samples_init(void) {}
void groove_samples_poll(void) {}
void groove_transfer_init(void) {}
void groove_transfer_poll(void) {}
void groove_fm_library_init(void) {}
