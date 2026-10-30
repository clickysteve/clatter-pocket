/*
 * pocket_panel.h - CLATTER Pocket's hardware interface: pages for the FM-1's 240x240 screen,
 * driven from its keys, buttons and encoders (docs/POCKET-UI.md). Platform-independent: the
 * FM-1 adapter (firmware/fm1/) feeds it the panel state and blits the bands it draws; the
 * host tests drive it directly.
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Control context only. It talks to the engine through pocket_lxr.h's event calls.
 */
#ifndef POCKET_PANEL_H
#define POCKET_PANEL_H

#include <stdint.h>
#include "pocket_lxr.h"
#include "pocket_seq.h"

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
/* The encoders as the panel labels them. Matrix order: fm1_open's app/screen.c uses 0 as its
 * main selector, the emulator's panel (from Felucca's controls.jpg) names them all. */
enum {
	POCKET_ENC_SELECT, POCKET_ENC_ALGORITHM, POCKET_ENC_KNOB1, POCKET_ENC_KNOB2, POCKET_ENC_KNOB3,
	POCKET_ENC_KNOB4, POCKET_ENC_PRESETS
};
#define POCKET_FW_VERSION "0.4.0 UNVERIFIED"

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
	uint32_t midi_messages;     /* MIDI messages received since start */
	uint32_t midi_hits;         /* voices triggered from MIDI since start */
	uint8_t master;             /* MASTER knob's output level, 0..127 */
	uint32_t heap_bytes;        /* runtime heap at link time (the SDK reports no free heap) */
	char build[9];              /* build identity, 8 hex digits, or empty */
};
enum { POCKET_STATE_STARTING, POCKET_STATE_READY, POCKET_STATE_SAFE, POCKET_STATE_ERROR };

enum { POCKET_PAGE_MAIN, POCKET_PAGE_PATTERN, POCKET_PAGE_SOUND, POCKET_PAGE_MIXER, POCKET_PAGE_GLOBAL, POCKET_PAGE_SYSTEM, POCKET_PAGES };

/* The keyboard: every key its own instrument, or all of them the selected one, chromatically. */
enum { POCKET_KEYS_DRUM, POCKET_KEYS_PITCH };
#define POCKET_KEY_LOWEST 53    /* key 0 is F3; C4 (60) sounds a voice's programmed pitch */
#define POCKET_OCTAVE_MIN (-3)
#define POCKET_OCTAVE_MAX 3
/* Recording while playing: a hit goes to the step sounding, or the nearest. */
enum { POCKET_QUANTISE_BEHIND, POCKET_QUANTISE_NEAREST };
#define POCKET_KITS 17          /* INIT, then the characters of pocket_kit.h */
#define POCKET_NOTICE_MS 1200U  /* what a control just did, on the status line */

/* SOUND's controls, KNOB1..KNOB4, and MIXER's, KNOB1..KNOB2. */
enum { POCKET_CTL_PITCH, POCKET_CTL_DECAY, POCKET_CTL_FILTER, POCKET_CTL_DRIVE, POCKET_CTL_LEVEL, POCKET_CTL_PAN, POCKET_CTLS };

#define POCKET_FLASH_MS 120U    /* a voice's activity cell after a trigger */
#define POCKET_MIDI_FLASH_MS 200U
#define POCKET_ALERT_MS 3000U   /* the warning after an audio overrun */
#define POCKET_GENERATING_MS 400U /* GENERATING KIT, before the kit's name */

struct pocket_panel {
	uint8_t trigger;                    /* selected POCKET_TRIG_* */
	uint8_t page;                       /* POCKET_PAGE_* */
	uint8_t midi_trigger;               /* last voice MIDI triggered, or 0xff */
	uint8_t midi_note;                  /* the note that triggered it */
	uint32_t previous_keys, previous_buttons;
	uint16_t dirty;                     /* bands to redraw, bit per band */
	uint32_t hits;                      /* triggers sent */
	uint16_t flash_ms[POCKET_LXR_TRIGGERS]; /* time left on each voice's activity cell */
	uint16_t midi_flash_ms, alert_ms;
	uint32_t alert_overruns;            /* overruns behind the warning */
	const char* footer;                 /* message of the start-up, SAFE MODE and error screens */
	/* The keyboard */
	int8_t key_mode;                    /* POCKET_KEYS_*; the int8s are GLOBAL's rows */
	int8_t octave;                      /* of the keys, in PITCH */
	int8_t key_velocity;                /* the keys report no velocity: this is theirs */
	int8_t follow;                      /* DRUM keys and MIDI move the selected voice */
	/* Recording */
	int8_t recording;
	int8_t quantise;                    /* POCKET_QUANTISE_* */
	int8_t auto_step;                   /* steps the cursor moves after a stopped entry, or 0 */
	uint8_t entered;                    /* a stopped entry this poll */
	/* MIDI */
	int8_t midi_channel;                /* 0: all, otherwise 1-16 */
	int8_t midi_map;                    /* drums: 0 the General MIDI notes, 1 as the keys */
	/* What is held */
	uint8_t save_down;                  /* SAVE is held and has been since a press seen here */
	uint8_t save_chorded;               /* and has modified something: its release does nothing */
	/* GLOBAL */
	uint8_t global_row;
	/* Kits: what is sounding is the engine's (pocket_lxr_get_param). */
	uint8_t kit;                        /* 0: the init kit, else character + 1 */
	int8_t kit_wild;                    /* 0-100 */
	uint8_t kit_generated;              /* not a factory kit: browsing away keeps it for SAVE+OCT+ */
	uint8_t kit_edited;                 /* a knob or a one-voice generate has changed it */
	uint8_t kit_settings_changed;       /* KIT SEED or WILD moved since the last ENV */
	uint8_t kit_announce;               /* its name follows GENERATING KIT */
	uint16_t kit_seed;                  /* 0-9999 */
	uint16_t kit_cursor;                /* next parameter of a kit being applied, or 0 */
	uint16_t kit_takes;                 /* one-voice sounds drawn since ENV */
	uint16_t notice_ms;
	char notice[28];
	/* The pattern, tempo, swing and transport are the sequencer's (pocket_seq.h). */
	uint8_t step;                       /* selected step */
	int8_t density, complexity;         /* 0-100 */
	int8_t style;                       /* pocket_gen.h */
	int8_t mutate_strength;             /* POCKET_MUTATE_* */
	uint8_t locks;                      /* tracks GENERATE and MUTATE leave alone, bit per track */
	uint8_t settings_changed;           /* a generator setting moved since the last GENERATE */
	uint16_t seed;                      /* 0-9999 */
	uint16_t mutations;                 /* since the last GENERATE: each one differs */
	uint16_t track_takes;               /* one-track patterns drawn: each one differs */
	int16_t shown_position;             /* what the screen and the cells last showed */
	uint32_t shown_steps;
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
/* The first pattern, once the sequencer is initialised and the engine runs. */
void pocket_panel_start_sequencer(struct pocket_panel* panel);
/* Apply one control poll: navigation, the transport, recording, generation, triggers on key
 * presses, parameter steps from encoders. Returns 1 when the exit gesture was made (the caller leaves Pocket mode). */
int pocket_panel_apply(struct pocket_panel* panel, const struct pocket_panel_input* in);
/* The parameter (PAR_*) behind control c for the selected trigger. */
int pocket_panel_param(const struct pocket_panel* panel, int c);
/* A note-on from MIDI, channel 0-15: the same path as the keys, so it plays the same way in DRUM
 * and PITCH and is recorded. Returns 1 if it played a voice. */
int pocket_panel_midi(struct pocket_panel* panel, unsigned channel, unsigned note, unsigned velocity);
/* The buttons to light (bit per POCKET_BTN_*): PLAY, REC, SCL in PITCH, SAVE on a locked track. */
uint32_t pocket_panel_leds(const struct pocket_panel* panel);
/* Time passing: follows the sequencer, applies a kit a little at a time, ends indications. */
void pocket_panel_tick(struct pocket_panel* panel, uint32_t elapsed_ms);
/* New measurements: stores them, redraws what shows them, raises the overrun warning. */
void pocket_panel_update_status(struct pocket_panel* panel, struct pocket_panel_status* status,
                                const struct pocket_panel_status* next);
/* Draw band b (0..POCKET_BANDS-1) into pixels[POCKET_SCREEN_W * POCKET_BAND_ROWS]. */
void pocket_panel_draw_band(const struct pocket_panel* panel, const struct pocket_panel_status* status,
                            unsigned band, uint16_t* pixels);

#ifdef __cplusplus
}
#endif

#endif
