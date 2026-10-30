/*
 * pocket_panel.c - CLATTER Pocket's hardware interface. See pocket_panel.h; the control map
 * and the screens are in docs/POCKET-UI.md and docs/POCKET-CONTROLS.md.
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Only what the firmware can do is on screen: every knob moves an engine parameter that the
 * host tests hear change, or the sequencer. Nothing here touches Flash; a pattern and a kit can be
 * replaced, and SAVE+OCT- and SAVE+OCT+ bring them back.
 *
 * Drawing: the Slop Factory 1-bit look (black on white, inverted title and footer), a 3x5
 * pixel font at twice its size, a band at a time so a page costs one 240x16 buffer.
 */
#include <stddef.h>
#include "pocket_panel.h"
#include "pocket_params.h"
#include "pocket_gen.h"
#include "pocket_kit.h"
#include "pocket_midi.h"

static const char* const triggerNames[POCKET_LXR_TRIGGERS] = {
	"DRUM 1", "DRUM 2", "DRUM 3", "SNARE", "CYMBAL", "CL HAT", "OP HAT"
};
static const char* const cellNames[POCKET_LXR_TRIGGERS] = { "D1", "D2", "D3", "SN", "CY", "CH", "OH" };
static const char* const pageNames[POCKET_PAGES] = { "MAIN", "PATTERN", "SOUND", "MIXER", "GLOBAL", "SYSTEM" };
static const char* const pageFooters[POCKET_PAGES] = {
	"ENV KIT ARP GEN LFO MUTATE",
	"K1 VEL K2 BPM K3 SWG K4 STEP",
	"SEL VOICE  KNOBS EDIT",
	"SEL VOICE  KNOBS EDIT",
	"K1 ROW K2 VALUE GLO SYSTEM",
	"GLO BACK  BOOT+HOME SAFE"
};
static const char* const mutateNames[POCKET_MUTATE_STRENGTHS] = { "LOW", "MED", "BIG" };
static const uint8_t controlParams[POCKET_CTLS][POCKET_LXR_TRIGGERS] = {
	{ POCKET_PAR_COARSE1, POCKET_PAR_COARSE2, POCKET_PAR_COARSE3, POCKET_PAR_COARSE4,
	  POCKET_PAR_COARSE5, POCKET_PAR_COARSE6, POCKET_PAR_COARSE6 },
	{ POCKET_PAR_VELOD1, POCKET_PAR_VELOD2, POCKET_PAR_VELOD3, POCKET_PAR_VELOD4,
	  POCKET_PAR_VELOD5, POCKET_PAR_VELOD6_CLOSED, POCKET_PAR_VELOD6_OPEN },
	{ POCKET_PAR_FILTER_FREQ_1, POCKET_PAR_FILTER_FREQ_2, POCKET_PAR_FILTER_FREQ_3, POCKET_PAR_FILTER_FREQ_4,
	  POCKET_PAR_FILTER_FREQ_5, POCKET_PAR_FILTER_FREQ_6, POCKET_PAR_FILTER_FREQ_6 },
	{ POCKET_PAR_DRIVE1, POCKET_PAR_DRIVE2, POCKET_PAR_DRIVE3, POCKET_PAR_SNARE_DISTORTION,
	  POCKET_PAR_CYMBAL_DISTORTION, POCKET_PAR_HAT_DISTORTION, POCKET_PAR_HAT_DISTORTION },
	{ POCKET_PAR_VOL1, POCKET_PAR_VOL2, POCKET_PAR_VOL3, POCKET_PAR_VOL4,
	  POCKET_PAR_VOL5, POCKET_PAR_VOL6, POCKET_PAR_VOL6 },
	{ POCKET_PAR_PAN1, POCKET_PAR_PAN2, POCKET_PAR_PAN3, POCKET_PAR_PAN4,
	  POCKET_PAR_PAN5, POCKET_PAR_PAN6, POCKET_PAR_PAN6 }
};
static const char* const controlNames[POCKET_CTLS] = { "PITCH", "DECAY", "FILTER", "DRIVE", "LEVEL", "PAN" };

/* The twelve keys of an octave in DRUM, from C: the same at every octave of the keyboard. The ten
 * instruments of browser CLATTER fold into the LXR's seven triggers as its default project routes
 * them (clap with the snare, percussion with the high tom, fx with the cymbal), and A# and B give
 * a second hat and kick, for two-finger rolls: no key is dead. */
static const uint8_t drumKeys[12] = {
	POCKET_TRIG_DRUM1, POCKET_TRIG_SNARE, POCKET_TRIG_SNARE, POCKET_TRIG_CLOSED_HAT, POCKET_TRIG_OPEN_HAT,
	POCKET_TRIG_DRUM2, POCKET_TRIG_DRUM3, POCKET_TRIG_CYMBAL, POCKET_TRIG_DRUM3, POCKET_TRIG_CYMBAL,
	POCKET_TRIG_CLOSED_HAT, POCKET_TRIG_DRUM1
};
/* DRUM has no octaves to change, so OCT sets how hard the keys hit: they report no velocity. */
static const uint8_t keyVelocities[4] = { 40, 70, 100, 127 };

/* The frame of every page; the page's own bands are B_BODY..B_ACTIVITY-1. */
enum { B_TITLE = 0, B_STATE = 1, B_VOICE = 2, B_MIDI = 3, B_BODY = 4, B_ACTIVITY = 12, B_LEVEL = 13, B_FOOTER = 14 };
#define ALL_BANDS ((uint16_t)((1u << POCKET_BANDS) - 1))
#define BODY_BANDS ((uint16_t)(((1u << B_ACTIVITY) - 1) & ~((1u << B_BODY) - 1)))
/* PATTERN's bands within the body, and MAIN's lines that follow the pattern and the kit. */
enum { L_TRANSPORT = 0, L_RULER = 1, L_GRID = 2, L_GRID_LAST = 6, L_LEGEND = 7, L_MAIN_HITS = 4, L_MAIN_RULER = 6, L_MAIN_KIT = 7 };
#define GRID_BANDS ((uint16_t)(((2u << (B_BODY + L_GRID_LAST)) - 1) & ~((1u << (B_BODY + L_GRID)) - 1)))
#define PATTERN_CHANGED ((uint16_t)(GRID_BANDS | (1u << (B_BODY + L_RULER)) | (1u << (B_BODY + L_MAIN_RULER)) | (1u << (B_BODY + L_LEGEND))))
#define BTN(name) (1u << POCKET_BTN_##name)

/* A kit is applied a little at a time, from here, while everything else keeps running. */
#define KIT_PER_POLL 12
static uint8_t kitTarget[POCKET_SOUND_PARAMS], kitUndo[POCKET_SOUND_PARAMS];

void pocket_panel_init(struct pocket_panel* panel)
{
	*panel = (struct pocket_panel){ 0 };
	panel->trigger = POCKET_TRIG_DRUM1;
	panel->midi_trigger = 0xff;
	panel->previous_buttons = ~0u; /* buttons held on entry do not act */
	panel->dirty = ALL_BANDS;
	panel->footer = "";
	panel->key_velocity = 127;
	panel->follow = 1;
	panel->quantise = POCKET_QUANTISE_NEAREST;
	panel->kit_wild = POCKET_KIT_WILD_DEFAULT;
	panel->density = 50;
	panel->complexity = 30;
	panel->seed = 1;
	panel->mutate_strength = POCKET_MUTATE_MEDIUM;
	panel->shown_position = POCKET_SEQ_NOT_PLAYING;
}

static int limited(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static int wrapped(int v, int size)
{
	v %= size;
	return v < 0 ? v + size : v;
}

static char* append(char* d, const char* s)
{
	while (*s) *d++ = *s++;
	*d = 0;
	return d;
}

/* decimal into buf, returns buf */
static char* number(char* buf, uint32_t v, int width)
{
	char tmp[12];
	int n = 0;
	do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
	int i = 0;
	while (n < width--) buf[i++] = ' ';
	while (n) buf[i++] = tmp[--n];
	buf[i] = 0;
	return buf;
}

static char* append_number(char* d, uint32_t v)
{
	char a[12];
	return append(d, number(a, v, 0));
}

/* What a control just did, in place of the status line for a moment. */
static void say(struct pocket_panel* panel, const char* a, const char* b)
{
	append(append(panel->notice, a), b);
	panel->notice_ms = POCKET_NOTICE_MS;
	panel->dirty |= 1u << B_STATE;
}

static struct pocket_gen_settings generator(const struct pocket_panel* panel)
{
	const struct pocket_gen_settings settings = { panel->seed, (uint8_t)panel->density, (uint8_t)panel->complexity, (uint8_t)panel->style, panel->locks };
	return settings;
}

static void generate(struct pocket_panel* panel)
{
	const struct pocket_gen_settings settings = generator(panel);
	pocket_gen_generate(pocket_seq_edit_begin(), &settings);
	pocket_seq_edit_commit();
	panel->mutations = 0;
	panel->settings_changed = 0;
	panel->dirty |= PATTERN_CHANGED;
}

void pocket_panel_start_sequencer(struct pocket_panel* panel)
{
	/* the LXR's velocity gain is a per-voice switch, off in the init kit: without it every step
	   sounds at full level. 127, the keys' velocity, is unity, so a key sounds as it did. */
	for (int v = 0; v < POCKET_LXR_VOICES; v++) pocket_lxr_set_param(POCKET_PAR_VOLUME_MOD_ON_OFF1 + v, 1);
	generate(panel);
	(void)pocket_seq_edit_begin(); /* nothing to undo yet: both patterns the same */
}

int pocket_panel_param(const struct pocket_panel* panel, int c)
{
	return controlParams[c][panel->trigger];
}

/* ------------------------------------------------------------------ playing and recording */
/* While playing a hit overdubs: it is never a reason to take a note away. A hit that lands ahead
 * of the playhead was already heard from the key, so that step does not play it again. Stopped,
 * the selected step is the insertion point: the same note again removes it, another note on the
 * track replaces it, and no other track is touched. */
static void record(struct pocket_panel* panel, int track, int note, int velocity)
{
	if (note == POCKET_SEQ_NOTE_PROGRAMMED) note = 0;
	if (pocket_seq_playing())
	{
		int ahead;
		const int step = pocket_seq_quantise(panel->quantise == POCKET_QUANTISE_NEAREST, &ahead);
		pocket_seq_set_step(track, step, velocity, note);
		if (ahead) pocket_seq_skip(track, step);
	}
	else
	{
		const unsigned was = pocket_seq_pattern()->cell[track][panel->step];
		const int same = was && POCKET_CELL_NOTE(was) == (unsigned)note;
		pocket_seq_set_step(track, panel->step, same ? 0 : velocity, note);
		panel->entered |= !same;
	}
	panel->dirty |= PATTERN_CHANGED | (1u << B_VOICE); /* the voice line shows the step's note */
}

/* A hit from a key or from MIDI; note 0 is the instrument's programmed pitch. */
static int play(struct pocket_panel* panel, int trigger, int note, int velocity)
{
	if (pocket_lxr_trigger(trigger, velocity, note ? note : POCKET_SEQ_NOTE_PROGRAMMED)) return -1;
	panel->flash_ms[trigger] = POCKET_FLASH_MS;
	panel->dirty |= 1u << B_ACTIVITY;
	if (panel->recording) record(panel, trigger, note, velocity);
	return 0;
}

static void select_voice(struct pocket_panel* panel, int trigger)
{
	if (trigger == panel->trigger) return;
	panel->trigger = (uint8_t)trigger;
	panel->dirty |= (1u << B_VOICE) | BODY_BANDS;
}

static void key_hit(struct pocket_panel* panel, int key)
{
	const int note = POCKET_KEY_LOWEST + key;
	const int drum = panel->key_mode == POCKET_KEYS_DRUM;
	const int trigger = drum ? drumKeys[note % 12] : panel->trigger;
	if (play(panel, trigger, drum ? 0 : note + 12 * panel->octave, panel->key_velocity)) return;
	if (drum && panel->follow) select_voice(panel, trigger);
	panel->hits++;
	panel->dirty |= 1u << (B_BODY + L_MAIN_HITS + 1);
}

int pocket_panel_midi(struct pocket_panel* panel, unsigned channel, unsigned note, unsigned velocity)
{
	if (panel->midi_channel && channel + 1u != (unsigned)panel->midi_channel) return 0;
	const int pitch = panel->key_mode == POCKET_KEYS_PITCH;
	const int trigger = pitch ? panel->trigger : panel->midi_map ? drumKeys[note % 12u] : pocket_midi_drum_trigger(note);
	if (trigger < 0 || play(panel, trigger, pitch ? (int)note : 0, (int)velocity)) return 0;
	if (!pitch && panel->follow) select_voice(panel, trigger);
	panel->midi_trigger = (uint8_t)trigger;
	panel->midi_note = (uint8_t)note;
	panel->midi_flash_ms = POCKET_MIDI_FLASH_MS;
	panel->dirty |= 1u << B_MIDI;
	return 1;
}

uint32_t pocket_panel_leds(const struct pocket_panel* panel)
{
	return (pocket_seq_playing() ? BTN(PLAY) : 0u) | (panel->recording ? BTN(REC) : 0u)
		| (panel->key_mode == POCKET_KEYS_PITCH ? BTN(SCL) : 0u)
		| (panel->locks >> panel->trigger & 1u ? BTN(SAVE) : 0u);
}

/* ------------------------------------------------------------------ kits */
static void kit_text(const struct pocket_panel* panel, char* d)
{
	d = append(d, panel->kit ? pocket_kit_name(panel->kit - 1u) : "INIT");
	if (panel->kit_generated) d = append_number(append(d, " "), panel->kit_seed);
	if (panel->kit_edited) append(d, "+");
}

static void say_kit(struct pocket_panel* panel)
{
	char name[20];
	kit_text(panel, name);
	say(panel, "KIT ", name);
	panel->dirty |= 1u << (B_BODY + L_MAIN_KIT);
}

/* The kit as it sounds, so that it can come back. Half way through applying another it would
 * be neither, and the one kept already is the better. */
static void kit_keep(const struct pocket_panel* panel)
{
	if (panel->kit_cursor) return;
	for (int p = 1; p < POCKET_SOUND_PARAMS; p++) kitUndo[p] = (uint8_t)pocket_lxr_get_param(p);
}

static void kit_design(struct pocket_panel* panel, unsigned wild)
{
	for (int p = 1; p < POCKET_SOUND_PARAMS; p++) kitTarget[p] = (uint8_t)pocket_lxr_init_kit_value(p);
	for (int v = 0; v < POCKET_LXR_VOICES; v++) kitTarget[POCKET_PAR_VOLUME_MOD_ON_OFF1 + v] = 1;
	if (panel->kit) pocket_kit_generate(kitTarget, panel->kit - 1u, panel->kit_seed, wild);
	panel->kit_takes = 0;
	panel->kit_cursor = 1;
}

/* PRESETS. A factory kit can always be browsed back to, so only an unsaved one is kept. */
static void kit_browse(struct pocket_panel* panel, int by)
{
	if (panel->kit_generated || panel->kit_edited) kit_keep(panel);
	panel->kit = (uint8_t)wrapped(panel->kit + by, POCKET_KITS);
	panel->kit_seed = (uint16_t)(panel->kit * 37u);
	panel->kit_generated = panel->kit_edited = panel->kit_settings_changed = 0;
	kit_design(panel, POCKET_KIT_WILD_DEFAULT);
	say_kit(panel);
}

/* ENV: if KIT SEED or WILD moved, from those; otherwise from the next seed. In the character
 * PRESETS has chosen; from the init kit, CLASSIC. */
static void kit_generate(struct pocket_panel* panel)
{
	kit_keep(panel);
	if (!panel->kit) panel->kit = 1;
	if (!panel->kit_settings_changed) panel->kit_seed = (uint16_t)wrapped(panel->kit_seed + 1, 10000);
	panel->kit_generated = 1;
	panel->kit_edited = panel->kit_settings_changed = 0;
	kit_design(panel, (unsigned)panel->kit_wild);
	say(panel, "GENERATING KIT", "");
	panel->notice_ms = POCKET_GENERATING_MS;
	panel->kit_announce = 1;
}

/* SAVE+ENV: the selected voice only. */
static void kit_voice(struct pocket_panel* panel)
{
	kit_keep(panel);
	for (int p = 1; p < POCKET_SOUND_PARAMS; p++) kitTarget[p] = panel->kit_cursor ? kitTarget[p] : (uint8_t)pocket_lxr_get_param(p);
	if (!panel->kit) panel->kit = 1;
	pocket_kit_voice(kitTarget, panel->kit - 1u, panel->kit_seed, (unsigned)panel->kit_wild,
	                 panel->trigger > POCKET_TRIG_CLOSED_HAT ? POCKET_TRIG_CLOSED_HAT : panel->trigger, ++panel->kit_takes);
	panel->kit_edited = 1;
	panel->kit_cursor = 1;
	say(panel, "NEW SOUND ", triggerNames[panel->trigger]);
	panel->dirty |= 1u << (B_BODY + L_MAIN_KIT);
}

/* SAVE+OCT+: back to the kit that was replaced; again, forward. */
static void kit_undo(struct pocket_panel* panel)
{
	if (panel->kit_cursor) return;
	for (int p = 1; p < POCKET_SOUND_PARAMS; p++)
	{
		kitTarget[p] = kitUndo[p];
		kitUndo[p] = (uint8_t)pocket_lxr_get_param(p);
	}
	panel->kit_edited = 1;
	panel->kit_cursor = 1;
	say(panel, "KIT UNDONE", "");
}

static void kit_pump(struct pocket_panel* panel)
{
	int p = panel->kit_cursor, sent = 0;
	if (!p) return;
	for (; p < POCKET_SOUND_PARAMS && sent < KIT_PER_POLL; p++)
	{
		if (kitTarget[p] == pocket_lxr_get_param(p)) continue;
		if (pocket_lxr_set_param(p, kitTarget[p])) break;
		sent++;
	}
	panel->kit_cursor = p < POCKET_SOUND_PARAMS ? (uint16_t)p : 0;
	if (!panel->kit_cursor) panel->dirty |= BODY_BANDS; /* SOUND and MIXER show the new values */
}

/* ------------------------------------------------------------------ time */
static uint16_t less(uint16_t left, uint32_t elapsed)
{
	return left > elapsed ? (uint16_t)(left - elapsed) : 0;
}

/* What the audio context played since the last look. The screen follows it; it cannot lag it. */
static void follow_sequencer(struct pocket_panel* panel)
{
	const int position = pocket_seq_position();
	if (position != panel->shown_position)
	{
		panel->shown_position = (int16_t)position;
		panel->dirty |= panel->page == POCKET_PAGE_PATTERN ? 1u << (B_BODY + L_RULER) : 1u << (B_BODY + L_MAIN_RULER);
	}
	const uint32_t steps = pocket_seq_steps_played();
	if (steps != panel->shown_steps)
	{
		panel->shown_steps = steps;
		const unsigned tracks = pocket_seq_last_tracks();
		for (int t = 0; t < POCKET_LXR_TRIGGERS; t++)
			if (tracks & (1u << t)) panel->flash_ms[t] = POCKET_FLASH_MS;
		if (tracks) panel->dirty |= (1u << B_ACTIVITY) | (panel->page == POCKET_PAGE_MAIN ? 1u << (B_BODY + L_MAIN_HITS) : 0u);
	}
}

void pocket_panel_tick(struct pocket_panel* panel, uint32_t elapsed_ms)
{
	for (int t = 0; t < POCKET_LXR_TRIGGERS; t++)
	{
		if (!panel->flash_ms[t]) continue;
		panel->flash_ms[t] = less(panel->flash_ms[t], elapsed_ms);
		if (!panel->flash_ms[t]) panel->dirty |= 1u << B_ACTIVITY;
	}
	if (panel->midi_flash_ms)
	{
		panel->midi_flash_ms = less(panel->midi_flash_ms, elapsed_ms);
		if (!panel->midi_flash_ms) panel->dirty |= 1u << B_MIDI;
	}
	if (panel->alert_ms)
	{
		panel->alert_ms = less(panel->alert_ms, elapsed_ms);
		if (!panel->alert_ms) panel->dirty |= 1u << B_STATE;
	}
	if (panel->notice_ms)
	{
		panel->notice_ms = less(panel->notice_ms, elapsed_ms);
		if (!panel->notice_ms) panel->dirty |= 1u << B_STATE;
		if (!panel->notice_ms && panel->kit_announce)
		{
			panel->kit_announce = 0;
			say_kit(panel);
		}
	}
	kit_pump(panel);
	follow_sequencer(panel);
}

void pocket_panel_update_status(struct pocket_panel* panel, struct pocket_panel_status* status,
                                const struct pocket_panel_status* next)
{
	if (next->overruns > status->overruns)
	{
		panel->alert_ms = POCKET_ALERT_MS;
		panel->alert_overruns = next->overruns;
		panel->dirty |= 1u << B_STATE;
	}
	if (next->state != status->state) panel->dirty = ALL_BANDS;
	if (next->midi_messages != status->midi_messages) panel->dirty |= 1u << B_MIDI;
	if (next->level != status->level) panel->dirty |= 1u << B_LEVEL;
	if (panel->page == POCKET_PAGE_SYSTEM && (next->cpu_permille != status->cpu_permille
		|| next->cpu_peak_permille != status->cpu_peak_permille || next->overruns != status->overruns
		|| next->clipped != status->clipped || next->dropped != status->dropped))
		panel->dirty |= BODY_BANDS;
	if (panel->page == POCKET_PAGE_MIXER && next->master != status->master) panel->dirty |= BODY_BANDS;
	*status = *next;
}

/* ------------------------------------------------------------------ controls */
static void go(struct pocket_panel* panel, unsigned page)
{
	if (page == panel->page) return;
	panel->page = (uint8_t)page;
	panel->dirty = ALL_BANDS;
}

static void navigate(struct pocket_panel* panel, uint32_t pressed)
{
	if (pressed & BTN(HOME)) go(panel, POCKET_PAGE_MAIN);
	if (pressed & BTN(SEQ)) go(panel, POCKET_PAGE_PATTERN);
	if (pressed & BTN(EDIT)) go(panel, POCKET_PAGE_SOUND);
	if (pressed & BTN(FX)) go(panel, POCKET_PAGE_MIXER);
	if (pressed & BTN(GLO)) go(panel, panel->page == POCKET_PAGE_GLOBAL ? POCKET_PAGE_SYSTEM : POCKET_PAGE_GLOBAL);
}

static void edit(struct pocket_panel* panel, int control, int band, int delta)
{
	const int p = pocket_panel_param(panel, control);
	pocket_lxr_set_param(p, limited(pocket_lxr_get_param(p) + delta, 0, 127));
	panel->kit_edited = 1;
	panel->dirty |= (3u << band) | (1u << (B_BODY + L_MAIN_KIT));
}

/* GLOBAL. A row is one of the panel's int8 settings, or a number the sequencer keeps. */
enum { ROW_KEYS, ROW_OCTAVE, ROW_KEY_VEL, ROW_FOLLOW, ROW_QUANTISE, ROW_AUTO_STEP, ROW_TEMPO, ROW_SWING, ROW_STYLE, ROW_DENSITY,
	ROW_COMPLEXITY, ROW_SEED, ROW_MUTATE, ROW_KIT_SEED, ROW_WILD, ROW_MIDI_CHANNEL, ROW_MIDI_MAP, ROWS };
#define NO_FIELD 0xff
static const char* const offOn[] = { "OFF", "ON" };
static const char* const keyModes[] = { "DRUM", "PITCH" };
static const char* const quantises[] = { "BEHIND", "NEAREST" };
static const char* const midiMaps[] = { "GM", "KEYS" };
static const struct row {
	const char* name;
	uint8_t field;     /* offset in the panel, or NO_FIELD */
	int8_t lo, hi;
	const char* const* words;
} rows[ROWS] = {
	[ROW_KEYS] = { "KEYS", offsetof(struct pocket_panel, key_mode), 0, 1, keyModes },
	[ROW_OCTAVE] = { "OCTAVE", offsetof(struct pocket_panel, octave), POCKET_OCTAVE_MIN, POCKET_OCTAVE_MAX, 0 },
	[ROW_KEY_VEL] = { "KEY VEL", offsetof(struct pocket_panel, key_velocity), 1, 127, 0 },
	[ROW_FOLLOW] = { "FOLLOW", offsetof(struct pocket_panel, follow), 0, 1, offOn },
	[ROW_QUANTISE] = { "QUANTISE", offsetof(struct pocket_panel, quantise), 0, 1, quantises },
	[ROW_AUTO_STEP] = { "AUTO STEP", offsetof(struct pocket_panel, auto_step), 0, 4, 0 },
	[ROW_TEMPO] = { "TEMPO", NO_FIELD, 0, 0, 0 },
	[ROW_SWING] = { "SWING", NO_FIELD, 0, 0, 0 },
	[ROW_STYLE] = { "STYLE", offsetof(struct pocket_panel, style), 0, POCKET_GEN_STYLES - 1, 0 },
	[ROW_DENSITY] = { "DENSITY", offsetof(struct pocket_panel, density), 0, 100, 0 },
	[ROW_COMPLEXITY] = { "COMPLEX", offsetof(struct pocket_panel, complexity), 0, 100, 0 },
	[ROW_SEED] = { "SEED", NO_FIELD, 0, 0, 0 },
	[ROW_MUTATE] = { "MUTATE", offsetof(struct pocket_panel, mutate_strength), 0, POCKET_MUTATE_STRENGTHS - 1, mutateNames },
	[ROW_KIT_SEED] = { "KIT SEED", NO_FIELD, 0, 0, 0 },
	[ROW_WILD] = { "WILD", offsetof(struct pocket_panel, kit_wild), 0, 100, 0 },
	[ROW_MIDI_CHANNEL] = { "MIDI CH", offsetof(struct pocket_panel, midi_channel), 0, 16, 0 },
	[ROW_MIDI_MAP] = { "MIDI MAP", offsetof(struct pocket_panel, midi_map), 0, 1, midiMaps },
};

static void row_move(struct pocket_panel* panel, int row, int by)
{
	const struct row* r = &rows[row];
	if (r->field != NO_FIELD)
	{
		int8_t* v = (int8_t*)panel + r->field;
		*v = (int8_t)limited(*v + by, r->lo, r->hi);
	}
	else if (row == ROW_TEMPO)
	{
		pocket_seq_set_tempo(pocket_seq_tempo() + by);
		pocket_lxr_set_bpm(pocket_seq_tempo()); /* the engine's tempo-synced LFOs */
	}
	else if (row == ROW_SWING) pocket_seq_set_swing(pocket_seq_swing() + by);
	else if (row == ROW_SEED) panel->seed = (uint16_t)wrapped(panel->seed + by, 10000);
	else panel->kit_seed = (uint16_t)wrapped(panel->kit_seed + by, 10000);
	if (row >= ROW_STYLE && row <= ROW_SEED) panel->settings_changed = 1;
	if (row == ROW_KIT_SEED || row == ROW_WILD) panel->kit_settings_changed = 1;
	panel->dirty |= (1u << B_STATE) | BODY_BANDS;
}

static void row_text(const struct pocket_panel* panel, int row, char* b)
{
	const struct row* r = &rows[row];
	const int v = r->field != NO_FIELD ? *((const int8_t*)panel + r->field)
		: row == ROW_TEMPO ? pocket_seq_tempo() : row == ROW_SWING ? pocket_seq_swing()
		: row == ROW_SEED ? panel->seed : panel->kit_seed;
	if (row == ROW_STYLE) append(b, pocket_gen_style_name((unsigned)v));
	else if (r->words) append(b, r->words[v]);
	else if (row == ROW_MIDI_CHANNEL && !v) append(b, "ALL");
	else if (v < 0) append_number(append(b, "-"), (uint32_t)-v);
	else append_number(b, (uint32_t)v);
}

/* The knobs of PATTERN: the selected step's velocity, the tempo and swing, the selected step. */
static void pattern_knobs(struct pocket_panel* panel, const int16_t* knob)
{
	if (!(knob[0] | knob[1] | knob[2] | knob[3])) return;
	const unsigned c = pocket_seq_pattern()->cell[panel->trigger][panel->step];
	if (knob[0] && c)
	{
		pocket_seq_set_step(panel->trigger, panel->step, limited((int)POCKET_CELL_VELOCITY(c) + 4 * knob[0], 1, 127), (int)POCKET_CELL_NOTE(c));
		panel->dirty |= GRID_BANDS;
	}
	if (knob[1]) row_move(panel, ROW_TEMPO, knob[1]);
	if (knob[2]) row_move(panel, ROW_SWING, knob[2]);
	if (knob[3])
	{
		panel->step = (uint8_t)wrapped(panel->step + knob[3], POCKET_SEQ_STEPS);
		panel->dirty |= (1u << B_VOICE) | GRID_BANDS | (1u << (B_BODY + L_RULER));
	}
	panel->dirty |= (1u << (B_BODY + L_TRANSPORT)) | (1u << (B_BODY + L_LEGEND));
}

static void page_knobs(struct pocket_panel* panel, const int16_t* knob)
{
	switch (panel->page)
	{
	case POCKET_PAGE_PATTERN:
		pattern_knobs(panel, knob);
		break;
	case POCKET_PAGE_SOUND:
		for (int c = 0; c < 4; c++)
			if (knob[c]) edit(panel, POCKET_CTL_PITCH + c, B_BODY + 2 * c, knob[c]);
		break;
	case POCKET_PAGE_MIXER:
		for (int c = 0; c < 2; c++)
			if (knob[c]) edit(panel, POCKET_CTL_LEVEL + c, B_BODY + 2 * c, knob[c]);
		if (knob[0]) panel->dirty |= BODY_BANDS; /* the channel bars */
		break;
	case POCKET_PAGE_GLOBAL:
		if (knob[0])
		{
			panel->global_row = (uint8_t)limited(panel->global_row + knob[0], 0, ROWS - 1);
			panel->dirty |= BODY_BANDS;
		}
		if (knob[1]) row_move(panel, panel->global_row, knob[1]);
		break;
	default:
		break;
	}
}

/* SAVE+ARP and SAVE+LFO refuse a locked track: the lock says it stays. */
static int track_free(struct pocket_panel* panel)
{
	if (!(panel->locks >> panel->trigger & 1u)) return 1;
	say(panel, "LOCKED ", triggerNames[panel->trigger]);
	return 0;
}

static void generate_button(struct pocket_panel* panel, int one_track)
{
	if (one_track)
	{
		if (!track_free(panel)) return;
		const struct pocket_gen_settings settings = generator(panel);
		pocket_gen_track(pocket_seq_edit_begin(), &settings, panel->trigger, panel->track_takes++);
		pocket_seq_edit_commit();
		panel->mutations = 0;
		panel->dirty |= PATTERN_CHANGED;
		say(panel, "NEW PATTERN ", triggerNames[panel->trigger]);
		return;
	}
	/* a setting moved: that seed with that setting. Otherwise the next seed. */
	if (!panel->settings_changed) panel->seed = (uint16_t)wrapped(panel->seed + 1, 10000);
	generate(panel);
	say(panel, "NEW PATTERN ", pocket_gen_style_name((unsigned)panel->style));
}

static void mutate_button(struct pocket_panel* panel, int one_track)
{
	if (one_track && !track_free(panel)) return;
	struct pocket_pattern* next = pocket_seq_edit_begin();
	pocket_gen_mutate(next, ((uint32_t)panel->seed << 16) | panel->mutations++, (unsigned)panel->mutate_strength,
	                  one_track ? POCKET_GEN_ALL_TRACKS & ~(1u << panel->trigger) : panel->locks);
	pocket_seq_edit_commit();
	panel->dirty |= PATTERN_CHANGED;
	say(panel, "MUTATED ", one_track ? triggerNames[panel->trigger] : mutateNames[panel->mutate_strength]);
}

/* OCT-/OCT+. In PITCH, the octave; in DRUM, how hard the keys hit. Under SAVE, the undos. */
static void octave_button(struct pocket_panel* panel, int by, int held)
{
	if (held)
	{
		if (by > 0) kit_undo(panel);
		else
		{
			pocket_seq_undo();
			panel->dirty |= PATTERN_CHANGED;
			say(panel, "PATTERN UNDONE", "");
		}
		return;
	}
	if (panel->key_mode == POCKET_KEYS_PITCH)
		panel->octave = (int8_t)limited(panel->octave + by, POCKET_OCTAVE_MIN, POCKET_OCTAVE_MAX);
	else
	{
		int level = 0;
		while (level < 3 && keyVelocities[level] < panel->key_velocity) level++;
		panel->key_velocity = (int8_t)keyVelocities[limited(level + by, 0, 3)];
	}
	panel->dirty |= 1u << B_STATE;
}

/* Every chord with SAVE is a one-track or an undo, and its release then locks nothing. */
#define SAVE_CHORDS (BTN(ENV) | BTN(ARP) | BTN(LFO) | BTN(OCT_DOWN) | BTN(OCT_UP))

static void buttons(struct pocket_panel* panel, uint32_t pressed, uint32_t released, uint32_t held)
{
	const int save = (held & BTN(SAVE)) != 0;
	if (pressed & BTN(SAVE))
	{
		panel->save_down = 1;
		panel->save_chorded = 0;
		panel->dirty |= 1u << B_FOOTER;
	}
	if (pressed & SAVE_CHORDS) panel->save_chorded |= (uint8_t)save;
	if ((released & BTN(SAVE)) && panel->save_down)
	{
		panel->save_down = 0;
		panel->dirty |= 1u << B_FOOTER;
		if (!panel->save_chorded)
		{
			panel->locks ^= (uint8_t)(1u << panel->trigger);
			panel->dirty |= (1u << B_VOICE) | GRID_BANDS;
			say(panel, panel->locks >> panel->trigger & 1u ? "LOCKED " : "UNLOCKED ", triggerNames[panel->trigger]);
		}
	}
	if (pressed & BTN(PLAY))
	{
		pocket_seq_play(!pocket_seq_playing());
		panel->dirty |= 1u << B_BODY; /* MAIN's and PATTERN's first line */
	}
	if (pressed & BTN(SCL))
	{
		panel->key_mode = !panel->key_mode;
		say(panel, "KEYS: ", keyModes[panel->key_mode]);
	}
	if (pressed & BTN(REC))
	{
		panel->recording = !panel->recording;
		say(panel, "REC ", offOn[panel->recording]);
	}
	if (pressed & BTN(OCT_DOWN)) octave_button(panel, -1, save);
	if (pressed & BTN(OCT_UP)) octave_button(panel, 1, save);
	if (pressed & BTN(ENV))
	{
		if (save) kit_voice(panel);
		else kit_generate(panel);
	}
	if (pressed & BTN(ARP)) generate_button(panel, save);
	if (pressed & BTN(LFO)) mutate_button(panel, save);
}

int pocket_panel_apply(struct pocket_panel* panel, const struct pocket_panel_input* in)
{
	const uint32_t pressedButtons = in->buttons & ~panel->previous_buttons;
	const uint32_t releasedButtons = panel->previous_buttons & ~in->buttons;
	const uint32_t pressedKeys = in->keys & ~panel->previous_keys;
	const int exit = POCKET_PANEL_TOGGLE(panel->previous_buttons, in->buttons);
	panel->previous_buttons = in->buttons;
	panel->previous_keys = in->keys;
	if (exit) return 1;

	navigate(panel, pressedButtons);
	buttons(panel, pressedButtons, releasedButtons, in->buttons);
	if (in->encoders[POCKET_ENC_SELECT])
		select_voice(panel, wrapped(panel->trigger + in->encoders[POCKET_ENC_SELECT], POCKET_LXR_TRIGGERS));
	if (in->encoders[POCKET_ENC_ALGORITHM])
	{
		panel->style = (int8_t)wrapped(panel->style + in->encoders[POCKET_ENC_ALGORITHM], POCKET_GEN_STYLES);
		panel->settings_changed = 1;
		say(panel, "STYLE ", pocket_gen_style_name((unsigned)panel->style));
		panel->dirty |= BODY_BANDS;
	}
	if (in->encoders[POCKET_ENC_PRESETS]) kit_browse(panel, in->encoders[POCKET_ENC_PRESETS]);
	page_knobs(panel, &in->encoders[POCKET_ENC_KNOB1]);
	for (int k = 0; k < POCKET_KEYS; k++)
		if (pressedKeys & (1u << k)) key_hit(panel, k);
	if (panel->entered)
	{
		panel->entered = 0;
		if (panel->auto_step)
		{
			panel->step = (uint8_t)wrapped(panel->step + panel->auto_step, POCKET_SEQ_STEPS);
			panel->dirty |= (1u << B_VOICE) | GRID_BANDS | (1u << (B_BODY + L_RULER));
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ drawing */
#define INK 0x0000u
#define PAPER 0xffffu
#define ACCENT 0xfbe0u  /* amber: the start-up screen's second line */
#define ADVANCE 8
#define MARGIN 8

/* 3x5 glyphs, rows top to bottom, 3 bits per row (bit 2 = left), packed row 0 high. */
static uint16_t glyph(char c)
{
	/* generated from 3x5 row bitmaps (tools/gen-font.py) */
	static const uint16_t letters[26] = { 0x2bed, 0x6bae, 0x3923, 0x6b6e, 0x79a7, 0x79a4, 0x396b, 0x5bed, 0x7497, 0x126a, 0x5bad, 0x4927, 0x5fed, 0x6b6d, 0x2b6a, 0x6ba4, 0x2b73, 0x6bad, 0x388e, 0x7492, 0x5b6f, 0x5b6a, 0x5bfd, 0x5aad, 0x5a92, 0x72a7 };
	static const uint16_t digits[10] = { 0x7b6f, 0x2c97, 0x62a7, 0x628e, 0x5bc9, 0x798e, 0x39ef, 0x7292, 0x7bef, 0x7bce };
	if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
	if (c >= '0' && c <= '9') return digits[c - '0'];
	switch (c)
	{
	case '%': return 0x52a5;
	case '.': return 0x0002;
	case ':': return 0x0410;
	case '-': return 0x01c0;
	case '/': return 0x12a4;
	case '+': return 0x05d0;
	case ',': return 0x0014;
	default: return 0;
	}
}

static void fill(uint16_t* px, int x, int y, int w, int h, uint16_t color)
{
	for (int r = y < 0 ? 0 : y; r < y + h && r < POCKET_BAND_ROWS; r++)
		for (int c = x < 0 ? 0 : x; c < x + w && c < POCKET_SCREEN_W; c++) px[r * POCKET_SCREEN_W + c] = color;
}

/* Text at `scale` times the font; y may be outside the band, so text can span bands. */
static int text_scaled(uint16_t* px, int x, int y, const char* s, uint16_t color, int scale)
{
	for (; *s; s++, x += 4 * scale)
	{
		const uint16_t g = glyph(*s);
		for (int r = 0; r < 5; r++)
			for (int c = 0; c < 3; c++)
				if (g & (1u << (14 - 3 * r - c))) fill(px, x + c * scale, y + r * scale, scale, scale, color);
	}
	return x;
}

static int text(uint16_t* px, int x, int y, const char* s, uint16_t color)
{
	return text_scaled(px, x, y, s, color, 2);
}

static char* percent(char* buf, uint32_t permille)
{
	char* p = buf;
	number(p, permille / 10, 0);
	while (*p) p++;
	*p++ = '.';
	*p++ = (char)('0' + permille % 10);
	*p++ = '%';
	*p = 0;
	return buf;
}

static char* concat(char* dst, const char* a, const char* b)
{
	append(append(dst, a), b);
	return dst;
}

static void label_value(uint16_t* px, const char* label, const char* value)
{
	const int y = (POCKET_BAND_ROWS - 10) / 2;
	text(px, MARGIN, y, label, INK);
	text(px, 96, y, value, INK);
}

static void label_number(uint16_t* px, const char* label, uint32_t value)
{
	char a[12];
	label_value(px, label, number(a, value, 0));
}

/* Two bands of one control: name and value, then a bar. */
static void control(uint16_t* px, int control, int value, const char* knob, unsigned second)
{
	const int y = (POCKET_BAND_ROWS - 10) / 2;
	if (!second)
	{
		char a[12];
		text(px, MARGIN, y, knob, INK);
		text(px, MARGIN + 7 * ADVANCE, y, controlNames[control], INK);
		text(px, POCKET_SCREEN_W - MARGIN - 3 * ADVANCE, y, number(a, (uint32_t)value, 3), INK);
		return;
	}
	const int w = POCKET_SCREEN_W - 2 * MARGIN;
	fill(px, MARGIN, 2, w, POCKET_BAND_ROWS - 4, INK);
	fill(px, MARGIN + 1, 3, w - 2, POCKET_BAND_ROWS - 6, PAPER);
	if (control == POCKET_CTL_PAN)
	{
		const int mid = MARGIN + w / 2, at = MARGIN + 2 + (w - 5) * value / 127;
		fill(px, mid, 3, 1, POCKET_BAND_ROWS - 6, INK);
		fill(px, at - 1, 4, 3, POCKET_BAND_ROWS - 8, INK);
	}
	else
		fill(px, MARGIN + 2, 4, (w - 4) * value / 127, POCKET_BAND_ROWS - 8, INK);
}

/* The step grid: 16 steps fit the 240 pixels, so there are no pages. Beats are set apart. */
#define GRID_X 30
#define CELL_W 12
#define ROW_H 11
#define GRID_ROWS_TOP 1  /* in the first grid band */

static int step_x(int step)
{
	return GRID_X + step * CELL_W + (step / 4) * 2;
}

/* A step: a bar as tall as its velocity, amber if it plays a note of its own, a dot when off. */
static void cell(uint16_t* px, int x, int y, unsigned step, int selected)
{
	const int velocity = (int)POCKET_CELL_VELOCITY(step);
	if (selected)
	{
		fill(px, x, y, CELL_W - 1, ROW_H, INK);
		fill(px, x + 1, y + 1, CELL_W - 3, ROW_H - 2, PAPER);
	}
	if (velocity)
	{
		const int h = 1 + velocity * 7 / 127;
		fill(px, x + 2, y + ROW_H - 2 - h, CELL_W - 5, h, POCKET_CELL_NOTE(step) ? ACCENT : INK);
	}
	else
		fill(px, x + 4, y + 4, 2, 2, INK);
}

static void draw_grid(const struct pocket_panel* panel, unsigned line, uint16_t* px)
{
	const struct pocket_pattern* pattern = pocket_seq_pattern();
	const int top = GRID_ROWS_TOP - (int)line * POCKET_BAND_ROWS;
	for (int t = 0; t < POCKET_SEQ_TRACKS; t++)
	{
		const int y = top + t * ROW_H;
		if (y + ROW_H <= 0 || y >= POCKET_BAND_ROWS) continue;
		const int selected = t == panel->trigger;
		if (selected) fill(px, MARGIN - 2, y - 1, 20, ROW_H + 1, INK);
		text_scaled(px, MARGIN, y, cellNames[t], selected ? PAPER : INK, 2);
		if (panel->locks & (1u << t)) fill(px, GRID_X - 5, y + 1, 3, ROW_H - 3, ACCENT);
		for (int s = 0; s < POCKET_SEQ_STEPS; s++)
			cell(px, step_x(s), y, pattern->cell[t][s], selected && s == panel->step);
	}
}

/* A row of the pattern's activity: how many tracks play each step, the playhead over it. */
static void draw_ruler(const struct pocket_panel* panel, int cursor, uint16_t* px)
{
	const struct pocket_pattern* pattern = pocket_seq_pattern();
	const int position = pocket_seq_position();
	for (int s = 0; s < POCKET_SEQ_STEPS; s++)
	{
		const int x = step_x(s), here = s == position;
		int tracks = 0;
		for (int t = 0; t < POCKET_SEQ_TRACKS; t++) tracks += pattern->cell[t][s] != 0;
		if (here) fill(px, x, 1, CELL_W - 1, 13, ACCENT);
		if (tracks) fill(px, x + 2, 13 - 2 * tracks, CELL_W - 5, 2 * tracks, INK);
		if (!(s & 3)) fill(px, x, 13, CELL_W - 1, 1, INK);
		if (cursor && s == panel->step) fill(px, x, 14, CELL_W - 1, 2, INK);
	}
}

/* Stop or the inverted PLAY. Returns the x after it. */
static int transport(uint16_t* px, int y)
{
	const int playing = pocket_seq_playing();
	if (playing) fill(px, MARGIN - 2, 0, 4 * ADVANCE + 2, POCKET_BAND_ROWS, INK);
	return text(px, MARGIN, y, playing ? "PLAY" : "STOP", playing ? PAPER : INK);
}

static void draw_transport(uint16_t* px)
{
	const int y = (POCKET_BAND_ROWS - 10) / 2;
	char b[24];
	transport(px, y);
	append(append_number(b, (uint32_t)pocket_seq_tempo()), " BPM");
	text(px, 48, y, b, INK);
	append_number(append(b, "SWING "), (uint32_t)pocket_seq_swing());
	text(px, 128, y, b, INK);
}

/* The four knobs' fields, in knob order. */
static void draw_legend(const struct pocket_panel* panel, uint16_t* px)
{
	char b[40], *d = b;
	const unsigned v = POCKET_CELL_VELOCITY(pocket_seq_pattern()->cell[panel->trigger][panel->step]);
	d = append(d, "VEL");
	d = v ? append_number(d, v) : append(d, "--");
	d = append_number(append(d, " BPM"), (uint32_t)pocket_seq_tempo());
	d = append_number(append(d, " SWG"), (uint32_t)pocket_seq_swing());
	append_number(append(d, " STEP"), panel->step + 1u);
	text(px, MARGIN, (POCKET_BAND_ROWS - 10) / 2, b, INK);
}

static const char* const knobNames[4] = { "KNOB 1", "KNOB 2", "KNOB 3", "KNOB 4" };

static void draw_main(const struct pocket_panel* panel, unsigned line, uint16_t* px)
{
	char a[12], b[20];
	if (line == 0) transport(px, (POCKET_BAND_ROWS - 10) / 2);
	if (line == 1) label_value(px, "TEMPO", concat(b, number(a, (uint32_t)pocket_seq_tempo(), 0), " BPM"));
	if (line == 2) label_number(px, "SWING", (uint32_t)pocket_seq_swing());
	if (line == 3) label_value(px, "STYLE", pocket_gen_style_name((unsigned)panel->style));
	if (line == L_MAIN_HITS) label_number(px, "SEQ HITS", pocket_seq_triggers());
	if (line == 5) label_number(px, "HITS", panel->hits);
	if (line == L_MAIN_RULER) draw_ruler(panel, 0, px);
	if (line == L_MAIN_KIT)
	{
		kit_text(panel, b);
		label_value(px, "KIT", b);
	}
}

static void draw_pattern(const struct pocket_panel* panel, unsigned line, uint16_t* px)
{
	if (line == L_TRANSPORT) draw_transport(px);
	else if (line == L_RULER) draw_ruler(panel, 1, px);
	else if (line == L_LEGEND) draw_legend(panel, px);
	else draw_grid(panel, line - L_GRID, px);
}

static void draw_sound(const struct pocket_panel* panel, unsigned line, uint16_t* px)
{
	const int c = POCKET_CTL_PITCH + (int)line / 2;
	control(px, c, pocket_lxr_get_param(pocket_panel_param(panel, c)), knobNames[line / 2], line & 1);
}

/* A level bar per voice (the hats share theirs), selected voice underlined. */
static void draw_channels(const struct pocket_panel* panel, unsigned line, uint16_t* px)
{
	static const uint8_t volumes[6] = { POCKET_PAR_VOL1, POCKET_PAR_VOL2, POCKET_PAR_VOL3, POCKET_PAR_VOL4,
		POCKET_PAR_VOL5, POCKET_PAR_VOL6 };
	static const char* const names[6] = { "D1", "D2", "D3", "SN", "CY", "HH" };
	const int top = -(int)line * POCKET_BAND_ROWS, height = 2 * POCKET_BAND_ROWS - 2;
	const int selected = panel->trigger > 5 ? 5 : panel->trigger;
	for (int v = 0; v < 6; v++)
	{
		const int x = MARGIN + v * 38, h = height * pocket_lxr_get_param(volumes[v]) / 127;
		fill(px, x, top + height - h, 20, h, INK);
		text(px, x + 2, top + height + 2, names[v], INK);
		if (v == selected) fill(px, x, top + height + 13, 20, 2, INK);
	}
}

static void draw_mixer(const struct pocket_panel* panel, const struct pocket_panel_status* status, unsigned line, uint16_t* px)
{
	if (line < 4)
	{
		const int c = POCKET_CTL_LEVEL + (int)line / 2;
		control(px, c, pocket_lxr_get_param(pocket_panel_param(panel, c)), knobNames[line / 2], line & 1);
	}
	else if (line == 4)
		label_number(px, "MASTER", status->master);
	else
		draw_channels(panel, line - 5, px);
}

/* Eight rows of GLOBAL, scrolled to keep the selected one, which is inverted. */
static void draw_global(const struct pocket_panel* panel, unsigned line, uint16_t* px)
{
	const int top = panel->global_row < 8 ? 0 : panel->global_row - 7, row = top + (int)line, y = (POCKET_BAND_ROWS - 10) / 2;
	const uint16_t ink = row == panel->global_row ? PAPER : INK;
	char b[16];
	if (ink == PAPER) fill(px, 0, 0, POCKET_SCREEN_W, POCKET_BAND_ROWS, INK);
	text(px, MARGIN, y, rows[row].name, ink);
	row_text(panel, row, b);
	text(px, 112, y, b, ink);
}

static void draw_system(const struct pocket_panel_status* status, unsigned line, uint16_t* px)
{
	char a[12];
	if (line == 0) label_value(px, "VERSION", POCKET_FW_VERSION);
	if (line == 1) label_value(px, "BUILD", status->build);
	if (line == 2) label_value(px, "CPU MEAN", percent(a, status->cpu_permille));
	if (line == 3) label_value(px, "CPU PEAK", percent(a, status->cpu_peak_permille));
	if (line == 4) label_number(px, "OVERRUNS", status->overruns);
	if (line == 5) label_number(px, "CLIPPED", status->clipped);
	if (line == 6) label_number(px, "DROPPED", status->dropped);
	if (line == 7)
	{
		if (status->heap_bytes)
		{
			char b[24];
			label_value(px, "HEAP", concat(b, number(a, status->heap_bytes, 0), " LINKED"));
		}
		else
			label_value(px, "HEAP", "UNKNOWN");
	}
}

/* The start-up, SAFE MODE and error screen: unmistakably not the stock instrument. */
static void draw_identity(const struct pocket_panel* panel, const struct pocket_panel_status* status,
                          unsigned band, uint16_t* px)
{
	static const char* const states[] = { "STARTING", "LXR ENGINE READY", "SAFE MODE - ENGINE OFF",
		"ENGINE ERROR - TABLES" };
	const int top = -(int)band * POCKET_BAND_ROWS;
	fill(px, 0, 0, POCKET_SCREEN_W, POCKET_BAND_ROWS, INK);
	text_scaled(px, 36, top + 56, "CLATTER POCKET", PAPER, 3);
	text(px, 56, top + 84, "LXR DRUM MACHINE", ACCENT);
	text(px, MARGIN, top + 136, states[status->state < 4 ? status->state : 3], PAPER);
	text(px, MARGIN, top + 152, panel->footer, PAPER);
	text(px, MARGIN, top + 200, POCKET_FW_VERSION, PAPER);
	text(px, MARGIN, top + 216, status->build, PAPER);
}

void pocket_panel_draw_band(const struct pocket_panel* panel, const struct pocket_panel_status* status,
                            unsigned band, uint16_t* px)
{
	static const struct pocket_panel_status none = { 0 };
	const int y = (POCKET_BAND_ROWS - 10) / 2;
	char a[24], b[48];
	if (!status) status = &none;
	if (status->state != POCKET_STATE_READY)
	{
		draw_identity(panel, status, band, px);
		return;
	}
	fill(px, 0, 0, POCKET_SCREEN_W, POCKET_BAND_ROWS, PAPER);
	if (band == B_TITLE || band == B_FOOTER)
	{
		fill(px, 0, 0, POCKET_SCREEN_W, POCKET_BAND_ROWS, INK);
		if (band == B_TITLE)
		{
			const char* name = pageNames[panel->page];
			int n = 0;
			while (name[n]) n++;
			text(px, MARGIN, y, "CLATTER POCKET", PAPER);
			text(px, POCKET_SCREEN_W - MARGIN - n * ADVANCE, y, name, PAPER);
		}
		else
			text(px, MARGIN, y, panel->save_down ? "SAVE+ ENV ARP LFO: ONE VOICE" : pageFooters[panel->page], PAPER);
		return;
	}
	if (band == B_STATE)
	{
		if (panel->alert_ms)
		{
			fill(px, 0, 0, POCKET_SCREEN_W, POCKET_BAND_ROWS, INK);
			text(px, MARGIN, y, concat(b, "AUDIO OVERRUN ", number(a, panel->alert_overruns, 0)), PAPER);
		}
		else if (panel->notice_ms)
		{
			fill(px, 0, 0, POCKET_SCREEN_W, POCKET_BAND_ROWS, INK);
			text(px, MARGIN, y, panel->notice, PAPER);
		}
		else
		{
			char* d = append(append(b, "KEYS: "), keyModes[panel->key_mode]);
			if (panel->key_mode == POCKET_KEYS_PITCH)
			{
				d = append(d, panel->octave < 0 ? " OCT -" : " OCT ");
				d = append_number(d, (uint32_t)(panel->octave < 0 ? -panel->octave : panel->octave));
			}
			else
				d = append_number(append(d, " VEL "), (uint32_t)panel->key_velocity);
			if (panel->recording) append(d, "  REC ON");
			text(px, MARGIN, y, b, INK);
		}
		return;
	}
	if (band == B_VOICE)
	{
		const int x = text(px, MARGIN, y, "VOICE ", INK);
		fill(px, x - 2, 0, 6 * ADVANCE + 4, POCKET_BAND_ROWS, INK);
		text(px, x, y, triggerNames[panel->trigger], PAPER);
		if (panel->page == POCKET_PAGE_PATTERN)
		{
			const unsigned note = POCKET_CELL_NOTE(pocket_seq_pattern()->cell[panel->trigger][panel->step]);
			char* d = append_number(append(b, note ? "S" : "STEP "), panel->step + 1u);
			if (note) append_number(append(d, " N"), note);
			text(px, 128, y, b, INK);
			if (panel->locks & (1u << panel->trigger)) text(px, 200, y, "LOCK", INK);
		}
		return;
	}
	if (band == B_MIDI)
	{
		if (panel->midi_trigger < POCKET_LXR_TRIGGERS)
		{
			if (panel->midi_flash_ms) fill(px, 0, 0, POCKET_SCREEN_W, POCKET_BAND_ROWS, INK);
			const uint16_t ink = panel->midi_flash_ms ? PAPER : INK;
			const int x = text(px, MARGIN, y, concat(b, "MIDI NOTE ", number(a, panel->midi_note, 0)), ink);
			text(px, x + ADVANCE, y, triggerNames[panel->midi_trigger], ink);
		}
		else if (status->midi_messages)
			text(px, MARGIN, y, concat(b, "MIDI MESSAGES ", number(a, status->midi_messages, 0)), INK);
		else
			text(px, MARGIN, y, "MIDI WAITING", INK);
		return;
	}
	if (band == B_ACTIVITY)
	{
		for (int t = 0; t < POCKET_LXR_TRIGGERS; t++)
		{
			const int x = 4 + t * 33;
			const uint16_t ink = panel->flash_ms[t] ? PAPER : INK;
			fill(px, x, 0, 34, POCKET_BAND_ROWS - 1, INK);
			if (!panel->flash_ms[t]) fill(px, x + 1, 1, 32, POCKET_BAND_ROWS - 3, PAPER);
			text(px, x + 9, y, cellNames[t], ink);
		}
		return;
	}
	if (band == B_LEVEL)
	{
		const int x = text(px, MARGIN, y, "LEVEL", INK) + ADVANCE;
		const int w = POCKET_SCREEN_W - MARGIN - x;
		fill(px, x, 3, w, POCKET_BAND_ROWS - 6, INK);
		fill(px, x + 1, 4, w - 2, POCKET_BAND_ROWS - 8, PAPER);
		fill(px, x + 2, 5, (w - 4) * (status->level > 100 ? 100 : status->level) / 100, POCKET_BAND_ROWS - 10, INK);
		return;
	}
	if (band >= B_BODY && band < B_ACTIVITY)
	{
		const unsigned line = band - B_BODY;
		switch (panel->page)
		{
		case POCKET_PAGE_MAIN: draw_main(panel, line, px); break;
		case POCKET_PAGE_PATTERN: draw_pattern(panel, line, px); break;
		case POCKET_PAGE_SOUND: draw_sound(panel, line, px); break;
		case POCKET_PAGE_MIXER: draw_mixer(panel, status, line, px); break;
		case POCKET_PAGE_GLOBAL: draw_global(panel, line, px); break;
		default: draw_system(status, line, px); break;
		}
	}
}
