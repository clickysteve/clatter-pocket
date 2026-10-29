/*
 * pocket_panel.h - CLATTER Pocket's first panel: one LXR voice, played and shaped from the
 * FM-1's controls. Platform-independent: the FM-1 adapter (firmware/fm1/) feeds it the
 * panel state and blits the bands it draws; the host tests drive it directly.
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Control context only. It talks to the engine through pocket_lxr.h's event calls.
 */
#ifndef POCKET_PANEL_H
#define POCKET_PANEL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The FM-1 panel as fm1_controls_read reports it (sdk/include/fm1.h). Button numbers are the
 * stock firmware's (app/screen.c: OCT_DOWN .. REC). */
enum {
	POCKET_BTN_OCT_DOWN, POCKET_BTN_OCT_UP, POCKET_BTN_FX, POCKET_BTN_SCL, POCKET_BTN_ENV,
	POCKET_BTN_LFO, POCKET_BTN_EDIT, POCKET_BTN_GLO, POCKET_BTN_HOME, POCKET_BTN_SAVE,
	POCKET_BTN_ARP, POCKET_BTN_SEQ, POCKET_BTN_PLAY, POCKET_BTN_REC
};
#define POCKET_KEYS 27      /* note keys F3..G5, bits 0..26 */
#define POCKET_ENCODERS 7

struct pocket_panel_input {
	uint32_t keys;                      /* held note keys */
	uint32_t buttons;                   /* held buttons */
	int16_t encoders[POCKET_ENCODERS];  /* detents since the last call */
};

/* Measurements the adapter supplies for the status lines. */
struct pocket_panel_status {
	uint16_t cpu_permille;      /* engine time per block / block period, mean of the last second */
	uint16_t cpu_peak_permille; /* worst block in the last second */
	uint32_t overruns;          /* audio blocks missed since start (FM-1 DMA statistics) */
	uint32_t clipped;           /* output samples saturated since start */
	uint32_t dropped;           /* engine events refused (ring full) since start */
	/* hardware-test diagnostics (all optional: zero shows as "starting", "waiting", empty) */
	uint8_t state;              /* POCKET_STATE_* */
	uint8_t level;              /* output peak of the last status period, 0..100 % of full scale */
	uint8_t midi_last;          /* last MIDI note-on note number */
	uint32_t midi_messages;     /* MIDI messages received since start */
	uint32_t midi_hits;         /* voices triggered from MIDI since start */
	char build[9];              /* build identity, 8 hex digits, or empty */
};
enum { POCKET_STATE_STARTING, POCKET_STATE_READY, POCKET_STATE_SAFE, POCKET_STATE_ERROR };

/* The voice's three controls of this milestone. */
enum { POCKET_CTL_PITCH, POCKET_CTL_DECAY, POCKET_CTL_LEVEL, POCKET_CTLS };

struct pocket_panel {
	uint8_t trigger;                    /* selected POCKET_TRIG_* */
	uint32_t previous_keys, previous_buttons;
	uint16_t dirty;                     /* bands to redraw, bit per band */
	uint32_t hits;                      /* triggers sent */
	const char* footer;                 /* footer text (pocket_panel_init sets the overlay's) */
};

/* The screen is drawn in bands of POCKET_BAND_ROWS rows of POCKET_SCREEN_W pixels, RGB565. */
#define POCKET_SCREEN_W 240
#define POCKET_SCREEN_H 240
#define POCKET_BAND_ROWS 16
#define POCKET_BANDS (POCKET_SCREEN_H / POCKET_BAND_ROWS)

/* Exit/enter gesture: HOME held, ARP pressed. */
#define POCKET_PANEL_TOGGLE(previous, now) \
	(((now) & (1u << POCKET_BTN_HOME)) && ((now) & ~(previous) & (1u << POCKET_BTN_ARP)))

void pocket_panel_init(struct pocket_panel* panel);
/* Apply one control poll: triggers on key/PLAY presses, parameter steps from encoders.
 * Returns 1 when the exit gesture was made (the caller leaves Pocket mode). */
int pocket_panel_apply(struct pocket_panel* panel, const struct pocket_panel_input* in);
/* The parameter (PAR_*) behind control c for the selected trigger. */
int pocket_panel_param(const struct pocket_panel* panel, int c);
/* Draw band b (0..POCKET_BANDS-1) into pixels[POCKET_SCREEN_W * POCKET_BAND_ROWS]. */
void pocket_panel_draw_band(const struct pocket_panel* panel, const struct pocket_panel_status* status,
                            unsigned band, uint16_t* pixels);
/* Bands whose content depends on the status lines (redrawn when measurements change). */
#define POCKET_PANEL_STATUS_BANDS ((1u << 1) | (1u << 3) | (1u << 10) | (1u << 11) | (1u << 12) | (1u << 13))

#ifdef __cplusplus
}
#endif

#endif
