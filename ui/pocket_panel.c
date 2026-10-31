/*
 * pocket_panel.c - CLATTER Pocket's hardware interface. See pocket_panel.h; the control map
 * and the screens are in docs/POCKET-UI.md and docs/POCKET-CONTROLS.md.
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Only what the firmware can do is on screen: every knob moves an engine parameter that the
 * host tests hear change, or the sequencer. The STORE page is the one way to Flash
 * (pocket_store.h). Everything else that changes the music can be taken back: SAVE+OCT- and
 * SAVE+OCT+ walk one history, in the order things were done (pocket_history.h).
 *
 * Drawing: white on black, orange for what is selected, sounding or recording, a 3x5 pixel font
 * at twice its size, a band at a time so a page costs one 240x16 buffer.
 */
#include <stddef.h>
#include "pocket_panel.h"
#include "pocket_params.h"
#include "pocket_gen.h"
#include "pocket_kit.h"
#include "pocket_midi.h"
#include "pocket_store.h"
#include "pocket_history.h"

static const char triggerNames[POCKET_LXR_TRIGGERS][7] = {
	"DRUM 1", "DRUM 2", "DRUM 3", "SNARE", "CYMBAL", "CL HAT", "OP HAT"
};
static const char cellNames[POCKET_LXR_TRIGGERS][3] = { "D1", "D2", "D3", "SN", "CY", "CH", "OH" };
static const char pageNames[POCKET_PAGES][9] = { "MAIN", "PATTERN", "SOUND", "MIXER", "GLOBAL", "SYSTEM", "PROJECTS" };
static const char pageFooters[POCKET_PAGES][30] = {
	"ENV KIT ARP GEN LFO MUTATE",
	"K1 VEL K2 BPM K3 SWG K4 STEP",
	"SEL VOICE  KNOBS EDIT",
	"SEL VOICE  KNOBS EDIT",
	"K1 ROW K2 VALUE GLO SYSTEM",
	"GLO BACK  BOOT+HOME SAFE",
	"SAVE STORE OCT- LOAD SEL SLOT"
};
static const char mutateNames[POCKET_MUTATE_STRENGTHS][4] = { "LOW", "MED", "BIG" };
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
static const char controlNames[POCKET_CTLS][7] = { "PITCH", "DECAY", "FILTER", "DRIVE", "LEVEL", "PAN" };

/* A DRUM bank: nine keys, the same nine sounds in every bank. There are more sounds than voices:
 * PERC and CLAP are the voices of DRUM 3 and SNARE moved for a hit, so those two pairs cannot sound
 * at once, and each cuts the other as any hit on its voice does. */
static const uint8_t bankTracks[POCKET_BANK_KEYS] = {
	POCKET_TRIG_DRUM1, POCKET_TRIG_SNARE, POCKET_TRIG_CLOSED_HAT, POCKET_TRIG_OPEN_HAT, POCKET_TRIG_DRUM2,
	POCKET_TRIG_DRUM3, POCKET_TRIG_CYMBAL, POCKET_TRIG_DRUM3, POCKET_TRIG_SNARE
};
static const char bankNames[POCKET_BANK_KEYS][8] = { "KICK", "SNARE", "CL HAT", "OP HAT", "LOW TOM", "HI TOM", "CYMBAL", "PERC", "CLAP" };
#define BANK_ALT 7 /* the positions from here are their voice's second sound */
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
static uint8_t kitTarget[POCKET_SOUND_PARAMS], kitNext[POCKET_SOUND_PARAMS];

/* Actions that continue while they are the last thing done: a tag each. */
enum { MERGE_NONE, MERGE_ENTRY, MERGE_TAKE, MERGE_BROWSE, MERGE_LENGTH, MERGE_PARAM, MERGE_CELL = MERGE_PARAM + POCKET_SOUND_PARAMS };

/* The settings that belong to what an action changes. Bytes of the panel, then the sequencer's. */
static const uint8_t settingFields[] = {
	offsetof(struct pocket_panel, kit), offsetof(struct pocket_panel, kit_generated), offsetof(struct pocket_panel, kit_edited),
	offsetof(struct pocket_panel, kit_wild), offsetof(struct pocket_panel, kit_seed), offsetof(struct pocket_panel, kit_seed) + 1,
	offsetof(struct pocket_panel, density), offsetof(struct pocket_panel, complexity), offsetof(struct pocket_panel, style),
	offsetof(struct pocket_panel, mutate_strength), offsetof(struct pocket_panel, locks), offsetof(struct pocket_panel, seed),
	offsetof(struct pocket_panel, seed) + 1
};
enum { SETTING_TEMPO_LOW = sizeof settingFields, SETTING_TEMPO_HIGH, SETTING_SWING, SETTING_LAST_PAGE, SETTINGS };
static uint8_t settingsBefore[SETTINGS];
/* While the history replays: where it goes, published together when it has finished. */
static struct pocket_pattern* replaying;
static uint8_t replayedTempo[2];

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

static void announce(struct pocket_panel* panel, const char* what)
{
	say(panel, what, "");
}

/* ------------------------------------------------------------------ the one history */
static unsigned setting_read(const struct pocket_panel* panel, unsigned i)
{
	if (i < SETTING_TEMPO_LOW) return ((const uint8_t*)panel)[settingFields[i]];
	if (i == SETTING_TEMPO_LOW) return (unsigned)pocket_seq_tempo() & 255u;
	if (i == SETTING_TEMPO_HIGH) return (unsigned)pocket_seq_tempo() >> 8;
	return i == SETTING_SWING ? (unsigned)pocket_seq_swing() : pocket_seq_pattern()->last_page;
}

static void setting_write(struct pocket_panel* panel, unsigned i, unsigned value)
{
	if (i < SETTING_TEMPO_LOW) ((uint8_t*)panel)[settingFields[i]] = (uint8_t)value;
	else if (i <= SETTING_TEMPO_HIGH) replayedTempo[i - SETTING_TEMPO_LOW] = (uint8_t)value;
	else if (i == SETTING_SWING) pocket_seq_set_swing((int)value);
	else replaying->last_page = (uint8_t)value;
}

/* An action: what it did to the settings is found by comparing them when it has finished. */
static void settings_keep(const struct pocket_panel* panel)
{
	for (unsigned i = 0; i < SETTINGS; i++) settingsBefore[i] = (uint8_t)setting_read(panel, i);
}

static void action_begin(const struct pocket_panel* panel, unsigned merge)
{
	pocket_history_begin(merge);
	settings_keep(panel);
}

static void action_end(const struct pocket_panel* panel)
{
	for (unsigned i = 0; i < SETTINGS; i++)
	{
		const unsigned now = setting_read(panel, i);
		if (now != settingsBefore[i]) pocket_history_value(POCKET_HISTORY_SETTING, i, settingsBefore[i], now);
	}
	pocket_history_end();
}

/* A whole-pattern change is recorded by what differs: the cost of an action is what it changed. */
static void pattern_changed(struct pocket_panel* panel)
{
	const struct pocket_pattern* was = pocket_seq_replaced();
	const struct pocket_pattern* now = pocket_seq_pattern();
	for (unsigned t = 0; t < POCKET_SEQ_TRACKS; t++)
		for (unsigned i = 0; i < POCKET_SEQ_MAX_STEPS; i++)
			if (was->cell[t][i] != now->cell[t][i]) pocket_history_cell(t << 6 | i, was->cell[t][i] ^ now->cell[t][i]);
	panel->dirty |= PATTERN_CHANGED;
}

/* One step, as the history will see it. */
static void set_cell(struct pocket_panel* panel, int track, int step, uint32_t cell)
{
	const uint32_t was = pocket_seq_pattern()->cell[track][step];
	if (was != cell) pocket_history_cell((unsigned)track << 6 | (unsigned)step, was ^ cell);
	pocket_seq_set_cell(track, step, cell);
	panel->dirty |= PATTERN_CHANGED | (1u << B_VOICE);
}

static void limit_step(struct pocket_panel* panel)
{
	const unsigned steps = POCKET_PATTERN_STEPS(pocket_seq_pattern());
	if (panel->step >= steps) panel->step = (uint8_t)(panel->step % steps);
}

/* How the kit stands: kitTarget while one is being applied, otherwise what the engine has. */
static void kit_sync(const struct pocket_panel* panel)
{
	if (panel->kit_cursor) return;
	for (int p = 1; p < POCKET_SOUND_PARAMS; p++) kitTarget[p] = (uint8_t)pocket_lxr_get_param(p);
}

static void replay_word(void* context, unsigned kind, unsigned index, uint32_t value)
{
	if (kind == POCKET_HISTORY_CELL) replaying->cell[index >> 6][index & 63u] ^= value;
	else if (kind == POCKET_HISTORY_PARAM) kitTarget[index] = (uint8_t)value;
	else setting_write(context, index, value);
}

/* SAVE+OCT- and SAVE+OCT+. Nothing runs again: what was there comes back, exactly. */
static void history_move(struct pocket_panel* panel, int forward)
{
	kit_sync(panel);
	replaying = pocket_seq_edit_begin();
	replayedTempo[0] = (uint8_t)(pocket_seq_tempo() & 255);
	replayedTempo[1] = (uint8_t)(pocket_seq_tempo() >> 8);
	if (!(forward ? pocket_history_redo : pocket_history_undo)(replay_word, panel))
	{
		announce(panel, forward ? "NOTHING TO REDO" : "NOTHING TO UNDO");
		return;
	}
	pocket_seq_edit_commit();
	pocket_seq_set_tempo(replayedTempo[0] | replayedTempo[1] << 8);
	pocket_lxr_set_bpm(pocket_seq_tempo());
	panel->kit_cursor = 1;
	limit_step(panel);
	panel->merge_ms = 0;
	panel->dirty = ALL_BANDS;
	announce(panel, forward ? "REDO" : "UNDO");
}

static struct pocket_gen_settings generator(const struct pocket_panel* panel)
{
	const struct pocket_gen_settings settings = { panel->seed, (uint8_t)panel->density, (uint8_t)panel->complexity, (uint8_t)panel->style, panel->locks, pocket_seq_pattern()->last_page };
	return settings;
}

static void generate(struct pocket_panel* panel)
{
	const struct pocket_gen_settings settings = generator(panel);
	pocket_gen_generate(pocket_seq_edit_begin(), &settings);
	pocket_seq_edit_commit();
	panel->mutations = 0;
	panel->settings_changed = 0;
	pattern_changed(panel);
}

void pocket_panel_start_sequencer(struct pocket_panel* panel)
{
	/* the LXR's velocity gain is a per-voice switch, off in the init kit: without it every step
	   sounds at full level. 127, the keys' velocity, is unity, so a key sounds as it did. */
	for (int v = 0; v < POCKET_LXR_VOICES; v++) pocket_lxr_set_param(POCKET_PAR_VOLUME_MOD_ON_OFF1 + v, 1);
	generate(panel);
	pocket_history_reset(); /* nothing to undo yet */
}

int pocket_panel_param(const struct pocket_panel* panel, int c)
{
	return controlParams[c][panel->trigger];
}

static void go(struct pocket_panel* panel, unsigned page);

/* ------------------------------------------------------------------ playing and recording */
/* While playing a hit overdubs: it is never a reason to take a note away. A hit that lands ahead
 * of the playhead was already heard from the key, so that step does not play it again. Stopped,
 * the selected step is the insertion point: the same tone again removes it, another tone on the
 * track replaces it, and no other track is touched. A step's condition outlives a new tone. */
static void record(struct pocket_panel* panel, int track, int tone, int velocity)
{
	if (tone == POCKET_SEQ_NOTE_PROGRAMMED) tone = 0;
	if (pocket_seq_playing())
	{
		int ahead;
		const int step = pocket_seq_quantise(panel->quantise == POCKET_QUANTISE_NEAREST, &ahead);
		pocket_history_begin(MERGE_TAKE);
		set_cell(panel, track, step, POCKET_CELL_WITH_CONDITION(POCKET_CELL(velocity, tone), POCKET_CELL_CONDITION(pocket_seq_pattern()->cell[track][step])));
		pocket_history_end();
		panel->merge_ms = POCKET_TAKE_MS;
		if (ahead) pocket_seq_skip(track, step);
	}
	else
	{
		const uint32_t was = pocket_seq_pattern()->cell[track][panel->step];
		const int same = was && POCKET_CELL_TONE(was) == (unsigned)tone;
		pocket_history_begin(MERGE_ENTRY);
		set_cell(panel, track, panel->step, same ? 0 : POCKET_CELL_WITH_CONDITION(POCKET_CELL(velocity, tone), POCKET_CELL_CONDITION(was)));
		pocket_history_end();
		panel->entered |= !same;
	}
}

/* A hit from a key or from MIDI. Its tone is as a step's: 0 the programmed pitch, a note, a variation. */
static int play(struct pocket_panel* panel, int trigger, int tone, int velocity)
{
	const int variation = tone >= 128 ? tone - 128 : 0;
	if (pocket_lxr_trigger_variation(trigger, velocity, tone && !variation ? tone : POCKET_SEQ_NOTE_PROGRAMMED, variation)) return -1;
	panel->flash_ms[trigger] = POCKET_FLASH_MS;
	panel->dirty |= 1u << B_ACTIVITY;
	if (panel->recording) record(panel, trigger, tone, velocity);
	return 0;
}

static void select_voice(struct pocket_panel* panel, int trigger)
{
	if (trigger == panel->trigger) return;
	panel->trigger = (uint8_t)trigger;
	panel->dirty |= (1u << B_VOICE) | BODY_BANDS;
}

/* The tone of a DRUM key: the bank chooses the variation, and the last two positions a voice's second sound. */
static int key_tone(int key)
{
	const int variation = key / POCKET_BANK_KEYS + (key % POCKET_BANK_KEYS >= BANK_ALT ? POCKET_VARIATION_ALT : 0);
	return variation ? (int)POCKET_TONE_VARIATION(variation) : 0;
}

static void key_hit(struct pocket_panel* panel, int key)
{
	const int drum = panel->key_mode == POCKET_KEYS_DRUM;
	const int trigger = drum ? bankTracks[key % POCKET_BANK_KEYS] : panel->trigger;
	if (play(panel, trigger, drum ? key_tone(key) : POCKET_KEY_LOWEST + key + 12 * panel->octave, panel->key_velocity)) return;
	if (drum)
	{
		panel->last_key = (uint8_t)(key + 1);
		panel->dirty |= 1u << B_VOICE;
		if (panel->follow) select_voice(panel, trigger);
	}
	panel->hits++;
	panel->dirty |= 1u << (B_BODY + L_MAIN_HITS + 1);
}

int pocket_panel_midi(struct pocket_panel* panel, unsigned channel, unsigned note, unsigned velocity)
{
	if (panel->midi_channel && channel + 1u != (unsigned)panel->midi_channel) return 0;
	const int pitch = panel->key_mode == POCKET_KEYS_PITCH, key = wrapped((int)note - POCKET_KEY_LOWEST, POCKET_KEYS);
	const int trigger = pitch ? panel->trigger : panel->midi_map ? bankTracks[key % POCKET_BANK_KEYS] : pocket_midi_drum_trigger(note);
	if (trigger < 0 || play(panel, trigger, pitch ? (int)note : panel->midi_map ? key_tone(key) : 0, (int)velocity)) return 0;
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
static void sound_text(char* d, unsigned kit, unsigned generated, unsigned seed, unsigned edited)
{
	d = append(d, kit && kit < POCKET_KITS ? pocket_kit_name(kit - 1u) : "INIT");
	if (generated) d = append_number(append(d, " "), seed);
	if (edited) append(d, "+");
}

static void kit_text(const struct pocket_panel* panel, char* d)
{
	sound_text(d, panel->kit, panel->kit_generated, panel->kit_seed, panel->kit_edited);
}

static void say_kit(struct pocket_panel* panel)
{
	char name[20];
	kit_text(panel, name);
	say(panel, "KIT ", name);
	panel->dirty |= 1u << (B_BODY + L_MAIN_KIT);
}

/* kitNext is the new kit: what differs goes to the history, and it is applied from here. */
static void kit_commit(struct pocket_panel* panel)
{
	for (int p = 1; p < POCKET_SOUND_PARAMS; p++)
		if (kitNext[p] != kitTarget[p])
		{
			pocket_history_value(POCKET_HISTORY_PARAM, (unsigned)p, kitTarget[p], kitNext[p]);
			kitTarget[p] = kitNext[p];
		}
	panel->kit_cursor = 1;
}

static void kit_design(struct pocket_panel* panel, unsigned wild)
{
	for (int p = 1; p < POCKET_SOUND_PARAMS; p++) kitNext[p] = (uint8_t)pocket_lxr_init_kit_value(p);
	for (int v = 0; v < POCKET_LXR_VOICES; v++) kitNext[POCKET_PAR_VOLUME_MOD_ON_OFF1 + v] = 1;
	if (panel->kit) pocket_kit_generate(kitNext, panel->kit - 1u, panel->kit_seed, wild);
	panel->kit_takes = 0;
	kit_commit(panel);
}

/* PRESETS. A turn through the kits is one change, whose undo is the kit it started from, so each
 * detent takes back the one before instead of filling the history with kits only passed through. */
static void kit_browse(struct pocket_panel* panel, int by)
{
	const unsigned kit = (unsigned)wrapped(panel->kit + by, POCKET_KITS);
	kit_sync(panel);
	if (pocket_history_begin(MERGE_BROWSE))
	{
		pocket_history_undo(replay_word, panel);
		panel->kit_cursor = 1;
		pocket_history_begin(MERGE_BROWSE);
	}
	settings_keep(panel);
	panel->kit = (uint8_t)kit;
	panel->kit_seed = (uint16_t)(panel->kit * 37u);
	panel->kit_generated = panel->kit_edited = panel->kit_settings_changed = 0;
	kit_design(panel, POCKET_KIT_WILD_DEFAULT);
	action_end(panel);
	panel->merge_ms = POCKET_MERGE_MS;
	say_kit(panel);
}

/* ENV: if KIT SEED or WILD moved, from those; otherwise from the next seed. In the character
 * PRESETS has chosen; from the init kit, CLASSIC. */
static void kit_generate(struct pocket_panel* panel)
{
	kit_sync(panel);
	action_begin(panel, MERGE_NONE);
	if (!panel->kit) panel->kit = 1;
	if (!panel->kit_settings_changed) panel->kit_seed = (uint16_t)wrapped(panel->kit_seed + 1, 10000);
	panel->kit_generated = 1;
	panel->kit_edited = panel->kit_settings_changed = 0;
	kit_design(panel, (unsigned)panel->kit_wild);
	action_end(panel);
	announce(panel, "GENERATING KIT");
	panel->notice_ms = POCKET_GENERATING_MS;
	panel->kit_announce = 1;
}

/* SAVE+ENV: the selected voice only. */
static void kit_voice(struct pocket_panel* panel)
{
	kit_sync(panel);
	action_begin(panel, MERGE_NONE);
	for (int p = 1; p < POCKET_SOUND_PARAMS; p++) kitNext[p] = kitTarget[p];
	if (!panel->kit) panel->kit = 1;
	pocket_kit_voice(kitNext, panel->kit - 1u, panel->kit_seed, (unsigned)panel->kit_wild,
	                 panel->trigger > POCKET_TRIG_CLOSED_HAT ? POCKET_TRIG_CLOSED_HAT : panel->trigger, ++panel->kit_takes);
	panel->kit_edited = 1;
	kit_commit(panel);
	action_end(panel);
	say(panel, "NEW SOUND ", triggerNames[panel->trigger]);
	panel->dirty |= 1u << (B_BODY + L_MAIN_KIT);
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

/* ------------------------------------------------------------------ saved projects */
/* A project is everything that makes the music, on the record's own terms. A sound and a rhythm
 * are as they were in the banks of kits and patterns before projects, which are still read. */
struct saved_sound_head {
	uint8_t used, kit, generated, edited;
	uint16_t seed;
};
struct saved_sound {
	struct saved_sound_head head;
	uint8_t wild, reserved;
	uint8_t params[POCKET_SOUND_PARAMS];
};
struct saved_rhythm {
	uint8_t used, swing, density, complexity, style, mutate, locks, last_page;
	uint16_t seed, tempo;
};
struct saved_head { /* as much of a project as its row shows */
	struct saved_rhythm rhythm;
	struct saved_sound_head sound;
};
struct saved_pattern {
	struct saved_rhythm rhythm;
	uint16_t cells[POCKET_SEQ_TRACKS][POCKET_SEQ_STEPS];
};
/* Every step of every page, 19 bits a step: velocity 7, tone 8, condition 4. */
#define CELL_BITS 19u
#define PROJECT_STEPS (POCKET_SEQ_TRACKS * POCKET_SEQ_MAX_STEPS)
struct project {
	struct saved_rhythm rhythm;
	struct saved_sound sound;
	uint8_t cells[(PROJECT_STEPS * CELL_BITS + 7u) / 8u];
};
_Static_assert(sizeof(struct saved_sound) <= POCKET_STORE_OLD_SLOT_BYTES && sizeof(struct saved_pattern) <= POCKET_STORE_OLD_SLOT_BYTES, "as the old slots");
_Static_assert(sizeof(struct project) <= POCKET_STORE_PROJECT_BYTES, "a project");

/* The rows of the page: projects, then, if there are any, what is left from before them. */
enum { SLOT_OLD_KITS = POCKET_STORE_PROJECTS, SLOT_OLD_PATTERNS = SLOT_OLD_KITS + POCKET_STORE_OLD_SLOTS,
	SLOT_FORGET = SLOT_OLD_PATTERNS + POCKET_STORE_OLD_SLOTS };

static int store_rows(void)
{
	return pocket_store_old(POCKET_STORE_OLD_KITS) || pocket_store_old(POCKET_STORE_OLD_PATTERNS) ? SLOT_FORGET + 1 : POCKET_STORE_PROJECTS;
}

static void store_open(struct pocket_panel* panel)
{
	panel->store_armed = 0;
	panel->store_state = (int8_t)pocket_store_open();
	if (panel->store_slot >= store_rows()) panel->store_slot = 0;
	go(panel, POCKET_PAGE_STORE);
	panel->dirty |= BODY_BANDS;
	if (panel->store_state == POCKET_STORE_DAMAGED) announce(panel, "SAVED DATA DAMAGED");
}

static void slot_say(struct pocket_panel* panel, const char* action)
{
	char b[8];
	number(b, panel->store_slot + 1u, 0);
	say(panel, action, b);
	panel->dirty |= BODY_BANDS;
}

static void cell_write(uint8_t* bytes, unsigned at, uint32_t cell)
{
	const uint32_t v = POCKET_CELL_VELOCITY(cell) | POCKET_CELL_TONE(cell) << 7 | POCKET_CELL_CONDITION(cell) << 15;
	for (unsigned b = 0; b < CELL_BITS; b++, at++)
		if (v >> b & 1u) bytes[at >> 3] |= (uint8_t)(1u << (at & 7u));
}

/* Values loaded are range-checked: a good CRC says what was written, not that it is sensible. */
static uint32_t cell_read(const uint8_t* bytes, unsigned at, unsigned track)
{
	uint32_t v = 0;
	for (unsigned b = 0; b < CELL_BITS; b++, at++) v |= (uint32_t)(bytes[at >> 3] >> (at & 7u) & 1u) << b;
	const unsigned velocity = v & 127u, tone = v >> 7 & 255u, condition = v >> 15;
	if (!velocity) return 0;
	return POCKET_CELL_WITH_CONDITION(POCKET_CELL(velocity, tone <= 127u || (tone > 128u && tone - 128u <= POCKET_VARIATION_LAST(track)) ? tone : 0u), condition < POCKET_CONDITIONS ? condition : 0u);
}

/* SAVE, on this page. Erasing silences audio, so not while a pattern plays, and a slot that holds
 * something needs a second press while the question is still on the screen. */
static int store_confirmed(struct pocket_panel* panel, const char* question)
{
	if (panel->store_armed == panel->store_slot + 1u && panel->notice_ms) return 1;
	panel->store_armed = panel->store_slot + 1u;
	announce(panel, question);
	return 0;
}

static void store_failed(struct pocket_panel* panel)
{
	panel->store_state = (int8_t)pocket_store_open();
	announce(panel, "SAVE FAILED");
	panel->dirty |= BODY_BANDS;
}

static void store_forget(struct pocket_panel* panel)
{
	if (!store_confirmed(panel, "ERASE OLD: SAVE AGAIN")) return;
	panel->store_armed = 0;
	if (pocket_store_forget_old() != POCKET_STORE_OK)
	{
		store_failed(panel);
		return;
	}
	panel->store_slot = 0;
	announce(panel, "OLD SAVES ERASED");
	panel->dirty |= BODY_BANDS;
}

static void store_save(struct pocket_panel* panel)
{
	const unsigned slot = panel->store_slot;
	if (panel->store_state < 0) return;
	if (pocket_seq_playing())
		announce(panel, "STOP TO SAVE");
	else if (slot == SLOT_FORGET)
		store_forget(panel);
	else if (slot >= pocket_store_room())
		say(panel, slot >= SLOT_OLD_KITS ? "OLD SAVES ONLY LOAD" : "HELD BY OLD SAVES", "");
	else
	{
		struct project* p = (struct project*)pocket_store_project(slot);
		if (!p)
		{
			store_failed(panel);
			return;
		}
		if (p->rhythm.used && !store_confirmed(panel, "OVERWRITE: SAVE AGAIN")) return;
		panel->store_armed = 0;
		for (uint8_t* b = (uint8_t*)p; b < (uint8_t*)(p + 1); b++) *b = 0;
		const struct pocket_pattern* pattern = pocket_seq_pattern();
		const struct saved_rhythm rhythm = { 1, (uint8_t)pocket_seq_swing(), (uint8_t)panel->density, (uint8_t)panel->complexity, (uint8_t)panel->style,
			(uint8_t)panel->mutate_strength, panel->locks, pattern->last_page, panel->seed, (uint16_t)pocket_seq_tempo() };
		p->rhythm = rhythm;
		p->sound.head.used = 1;
		p->sound.head.kit = panel->kit;
		p->sound.head.generated = panel->kit_generated;
		p->sound.head.edited = panel->kit_edited;
		p->sound.head.seed = panel->kit_seed;
		p->sound.wild = (uint8_t)panel->kit_wild;
		kit_sync(panel);
		for (int k = 1; k < POCKET_SOUND_PARAMS; k++) p->sound.params[k] = kitTarget[k];
		for (unsigned t = 0; t < POCKET_SEQ_TRACKS; t++)
			for (unsigned i = 0; i < POCKET_SEQ_MAX_STEPS; i++) cell_write(p->cells, (t * POCKET_SEQ_MAX_STEPS + i) * CELL_BITS, pattern->cell[t][i]);
		if (pocket_store_commit() == POCKET_STORE_OK) slot_say(panel, "SAVED ");
		else store_failed(panel);
	}
}

static void load_sound(struct pocket_panel* panel, const struct saved_sound* s)
{
	for (int p = 1; p < POCKET_SOUND_PARAMS; p++) kitNext[p] = s->params[p];
	for (int v = 0; v < POCKET_LXR_VOICES; v++) kitNext[POCKET_PAR_VOLUME_MOD_ON_OFF1 + v] = 1;
	panel->kit = s->head.kit < POCKET_KITS ? s->head.kit : 0;
	panel->kit_generated = s->head.generated != 0;
	panel->kit_edited = s->head.edited != 0;
	panel->kit_seed = s->head.seed;
	panel->kit_wild = (int8_t)limited(s->wild, 0, 100);
	panel->kit_settings_changed = 0;
	panel->kit_takes = 0;
	kit_commit(panel);
	panel->dirty |= 1u << (B_BODY + L_MAIN_KIT);
}

static void load_rhythm(struct pocket_panel* panel, const struct saved_rhythm* s)
{
	pocket_seq_edit_commit();
	pattern_changed(panel);
	pocket_seq_set_tempo(s->tempo);
	pocket_lxr_set_bpm(pocket_seq_tempo());
	pocket_seq_set_swing(s->swing);
	panel->density = (int8_t)limited(s->density, 0, 100);
	panel->complexity = (int8_t)limited(s->complexity, 0, 100);
	panel->style = (int8_t)(s->style < POCKET_GEN_STYLES ? s->style : 0);
	panel->mutate_strength = (int8_t)(s->mutate < POCKET_MUTATE_STRENGTHS ? s->mutate : POCKET_MUTATE_MEDIUM);
	panel->locks = (uint8_t)(s->locks & POCKET_GEN_ALL_TRACKS);
	panel->seed = (uint16_t)(s->seed < 10000u ? s->seed : 1u);
	panel->settings_changed = 0;
	panel->mutations = 0;
	limit_step(panel);
}

/* OCT-, on this page, also while playing. Its undo is everything it replaced. */
static void store_load(struct pocket_panel* panel)
{
	const unsigned slot = panel->store_slot;
	if (panel->store_state < 0 || slot == SLOT_FORGET) return;
	uint8_t* record = pocket_store_project(slot < SLOT_OLD_KITS ? slot : 0);
	if (!record) return;
	if (slot >= SLOT_OLD_KITS)
		pocket_store_old_peek(slot < SLOT_OLD_PATTERNS ? POCKET_STORE_OLD_KITS : POCKET_STORE_OLD_PATTERNS, (slot - SLOT_OLD_KITS) % POCKET_STORE_OLD_SLOTS, record, POCKET_STORE_OLD_SLOT_BYTES);
	if (!record[0]) return;
	kit_sync(panel);
	action_begin(panel, MERGE_NONE);
	if (slot < SLOT_OLD_KITS)
	{
		const struct project* p = (const struct project*)record;
		struct pocket_pattern* next = pocket_seq_edit_begin();
		for (unsigned t = 0; t < POCKET_SEQ_TRACKS; t++)
			for (unsigned i = 0; i < POCKET_SEQ_MAX_STEPS; i++) next->cell[t][i] = cell_read(p->cells, (t * POCKET_SEQ_MAX_STEPS + i) * CELL_BITS, t);
		next->last_page = p->rhythm.last_page & 3u;
		load_rhythm(panel, &p->rhythm);
		load_sound(panel, &p->sound);
	}
	else if (slot < SLOT_OLD_PATTERNS)
		load_sound(panel, (const struct saved_sound*)record);
	else
	{
		const struct saved_pattern* s = (const struct saved_pattern*)record;
		struct pocket_pattern* next = pocket_seq_edit_begin();
		for (int t = 0; t < POCKET_SEQ_TRACKS; t++)
			for (int i = 0; i < POCKET_SEQ_MAX_STEPS; i++)
			{
				const unsigned c = i < POCKET_SEQ_STEPS ? s->cells[t][i] & 0x7f7fu : 0;
				next->cell[t][i] = POCKET_CELL_VELOCITY(c) ? c : 0;
			}
		next->last_page = 0;
		load_rhythm(panel, &s->rhythm);
	}
	action_end(panel);
	slot_say(panel, "LOADED ");
}

/* ------------------------------------------------------------------ time */
static uint16_t less(uint16_t left, uint32_t elapsed)
{
	return left > elapsed ? (uint16_t)(left - elapsed) : 0;
}

/* The page of steps on screen: the selected step's, or while playing where a hit would land if
 * one is being recorded, and on MAIN where the music is. */
static unsigned shown_page(const struct pocket_panel* panel)
{
	const int position = pocket_seq_position();
	if (position != POCKET_SEQ_NOT_PLAYING && (panel->recording || panel->page == POCKET_PAGE_MAIN)) return (unsigned)position / POCKET_SEQ_STEPS;
	return panel->step / POCKET_SEQ_STEPS;
}

/* What the audio context played since the last look. The screen follows it; it cannot lag it. */
static void follow_sequencer(struct pocket_panel* panel)
{
	const int position = pocket_seq_position();
	if (position != panel->shown_position)
	{
		const int newPage = position / POCKET_SEQ_STEPS != panel->shown_position / POCKET_SEQ_STEPS;
		panel->shown_position = (int16_t)position;
		panel->dirty |= panel->page == POCKET_PAGE_PATTERN ? 1u << (B_BODY + L_RULER) : 1u << (B_BODY + L_MAIN_RULER);
		if (newPage) panel->dirty |= 1u << (B_BODY + L_TRANSPORT);
		if (newPage && panel->recording) panel->dirty |= PATTERN_CHANGED;
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
	if (panel->merge_ms)
	{
		panel->merge_ms = less(panel->merge_ms, elapsed_ms);
		if (!panel->merge_ms) pocket_history_close();
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

static void navigate(struct pocket_panel* panel, uint32_t pressed, int save)
{
	if (pressed & BTN(HOME)) go(panel, POCKET_PAGE_MAIN);
	if (pressed & BTN(SEQ)) go(panel, POCKET_PAGE_PATTERN);
	if (pressed & BTN(EDIT)) go(panel, POCKET_PAGE_SOUND);
	if ((pressed & BTN(FX)) && !save) go(panel, POCKET_PAGE_MIXER);
	if (pressed & BTN(GLO)) go(panel, panel->page == POCKET_PAGE_GLOBAL ? POCKET_PAGE_SYSTEM : POCKET_PAGE_GLOBAL);
}

static void edit(struct pocket_panel* panel, int control, int band, int delta)
{
	const int p = pocket_panel_param(panel, control);
	kit_sync(panel);
	const int value = limited(kitTarget[p] + delta, 0, 127);
	if (value == kitTarget[p]) return;
	action_begin(panel, MERGE_PARAM + (unsigned)p);
	pocket_history_value(POCKET_HISTORY_PARAM, (unsigned)p, kitTarget[p], (unsigned)value);
	kitTarget[p] = (uint8_t)value;
	if (pocket_lxr_set_param(p, value)) panel->kit_cursor = 1;
	panel->kit_edited = 1;
	action_end(panel);
	panel->merge_ms = POCKET_MERGE_MS;
	panel->dirty |= (3u << band) | (1u << (B_BODY + L_MAIN_KIT));
}

/* How long the pattern is, in pages. The steps of pages left out stay, and come back with them. */
static void set_length(struct pocket_panel* panel, int last_page)
{
	last_page = limited(last_page, 0, POCKET_SEQ_PAGES - 1);
	if (last_page == pocket_seq_pattern()->last_page) return;
	action_begin(panel, MERGE_LENGTH);
	pocket_seq_set_last_page((unsigned)last_page);
	action_end(panel);
	panel->merge_ms = POCKET_MERGE_MS;
	limit_step(panel);
	panel->dirty |= PATTERN_CHANGED | (1u << B_VOICE) | (1u << (B_BODY + L_TRANSPORT));
}

/* GLOBAL. A row is one of the panel's int8 settings, or a number the sequencer keeps. */
enum { ROW_KEYS, ROW_OCTAVE, ROW_KEY_VEL, ROW_FOLLOW, ROW_QUANTISE, ROW_AUTO_STEP, ROW_TEMPO, ROW_SWING, ROW_LENGTH, ROW_STYLE, ROW_DENSITY,
	ROW_COMPLEXITY, ROW_SEED, ROW_MUTATE, ROW_KIT_SEED, ROW_WILD, ROW_MIDI_CHANNEL, ROW_MIDI_MAP, ROWS };
#define NO_FIELD 0xff
static const char offOn[2][4] = { "OFF", "ON" };
static const char keyModes[2][6] = { "DRUM", "PITCH" };
static const char quantises[2][8] = { "BEHIND", "NEAREST" };
static const char midiMaps[2][5] = { "GM", "KEYS" };
static const struct row {
	const char* name;
	uint8_t field;     /* offset in the panel, or NO_FIELD */
	int8_t lo, hi;
	const char* words;   /* the values' names, every `size` bytes */
	uint8_t size;
} rows[ROWS] = {
	[ROW_KEYS] = { "KEYS", offsetof(struct pocket_panel, key_mode), 0, 1, *keyModes, 6 },
	[ROW_OCTAVE] = { "OCTAVE", offsetof(struct pocket_panel, octave), POCKET_OCTAVE_MIN, POCKET_OCTAVE_MAX, 0 },
	[ROW_KEY_VEL] = { "KEY VEL", offsetof(struct pocket_panel, key_velocity), 1, 127, 0 },
	[ROW_FOLLOW] = { "FOLLOW", offsetof(struct pocket_panel, follow), 0, 1, *offOn, 4 },
	[ROW_QUANTISE] = { "QUANTISE", offsetof(struct pocket_panel, quantise), 0, 1, *quantises, 8 },
	[ROW_AUTO_STEP] = { "AUTO STEP", offsetof(struct pocket_panel, auto_step), 0, 4, 0 },
	[ROW_TEMPO] = { "TEMPO", NO_FIELD, 0, 0, 0 },
	[ROW_SWING] = { "SWING", NO_FIELD, 0, 0, 0 },
	[ROW_LENGTH] = { "LENGTH", NO_FIELD, 0, 0, 0 },
	[ROW_STYLE] = { "STYLE", offsetof(struct pocket_panel, style), 0, POCKET_GEN_STYLES - 1, 0 },
	[ROW_DENSITY] = { "DENSITY", offsetof(struct pocket_panel, density), 0, 100, 0 },
	[ROW_COMPLEXITY] = { "COMPLEX", offsetof(struct pocket_panel, complexity), 0, 100, 0 },
	[ROW_SEED] = { "SEED", NO_FIELD, 0, 0, 0 },
	[ROW_MUTATE] = { "MUTATE", offsetof(struct pocket_panel, mutate_strength), 0, POCKET_MUTATE_STRENGTHS - 1, *mutateNames, 4 },
	[ROW_KIT_SEED] = { "KIT SEED", NO_FIELD, 0, 0, 0 },
	[ROW_WILD] = { "WILD", offsetof(struct pocket_panel, kit_wild), 0, 100, 0 },
	[ROW_MIDI_CHANNEL] = { "MIDI CH", offsetof(struct pocket_panel, midi_channel), 0, 16, 0 },
	[ROW_MIDI_MAP] = { "MIDI MAP", offsetof(struct pocket_panel, midi_map), 0, 1, *midiMaps, 5 },
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
	else if (row == ROW_LENGTH) set_length(panel, pocket_seq_pattern()->last_page + by);
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
		: row == ROW_LENGTH ? (int)POCKET_PATTERN_STEPS(pocket_seq_pattern()) : row == ROW_SEED ? panel->seed : panel->kit_seed;
	if (row == ROW_STYLE) append(b, pocket_gen_style_name((unsigned)v));
	else if (r->words) append(b, r->words + v * r->size);
	else if (row == ROW_MIDI_CHANNEL && !v) append(b, "ALL");
	else if (v < 0) append_number(append(b, "-"), (uint32_t)-v);
	else append_number(b, (uint32_t)v);
}

static void move_step(struct pocket_panel* panel, int by)
{
	panel->step = (uint8_t)wrapped(panel->step + by, (int)POCKET_PATTERN_STEPS(pocket_seq_pattern()));
	panel->dirty |= (1u << B_VOICE) | PATTERN_CHANGED | (1u << (B_BODY + L_TRANSPORT));
}

/* One step changed by a knob: turning on is one undo. */
static void step_edit(struct pocket_panel* panel, uint32_t cell)
{
	pocket_history_begin(MERGE_CELL + ((unsigned)panel->trigger << 6 | panel->step));
	set_cell(panel, panel->trigger, panel->step, cell);
	pocket_history_end();
	panel->merge_ms = POCKET_MERGE_MS;
}

/* The knobs of PATTERN: the selected step's velocity, the tempo and swing, the selected step.
 * With SAVE held: its condition, the pattern's length, a page at a time. */
static void pattern_knobs(struct pocket_panel* panel, const int16_t* knob, int save)
{
	if (!(knob[0] | knob[1] | knob[2] | knob[3])) return;
	const uint32_t c = pocket_seq_pattern()->cell[panel->trigger][panel->step];
	if (save)
	{
		if (knob[0] && c) step_edit(panel, POCKET_CELL_WITH_CONDITION(c, limited((int)POCKET_CELL_CONDITION(c) + knob[0], 0, POCKET_CONDITIONS - 1)));
		if (knob[2]) set_length(panel, pocket_seq_pattern()->last_page + knob[2]);
		if (knob[3]) move_step(panel, POCKET_SEQ_STEPS * knob[3]);
		return;
	}
	if (knob[0] && c) step_edit(panel, (c & ~0xffu) | (uint32_t)limited((int)POCKET_CELL_VELOCITY(c) + 4 * knob[0], 1, 127));
	if (knob[1]) row_move(panel, ROW_TEMPO, knob[1]);
	if (knob[2]) row_move(panel, ROW_SWING, knob[2]);
	if (knob[3]) move_step(panel, knob[3]);
	panel->dirty |= (1u << (B_BODY + L_TRANSPORT)) | (1u << (B_BODY + L_LEGEND));
}

static void move_slot(struct pocket_panel* panel, int by)
{
	panel->store_slot = (uint8_t)limited(panel->store_slot + by, 0, store_rows() - 1);
	panel->store_armed = 0;
	panel->dirty |= BODY_BANDS;
}

static void page_knobs(struct pocket_panel* panel, const int16_t* knob, int save)
{
	switch (panel->page)
	{
	case POCKET_PAGE_PATTERN:
		pattern_knobs(panel, knob, save);
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
	case POCKET_PAGE_STORE:
		if (knob[0]) move_slot(panel, knob[0]);
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
		action_begin(panel, MERGE_NONE);
		pocket_gen_track(pocket_seq_edit_begin(), &settings, panel->trigger, panel->track_takes++);
		pocket_seq_edit_commit();
		panel->mutations = 0;
		pattern_changed(panel);
		action_end(panel);
		say(panel, "NEW PATTERN ", triggerNames[panel->trigger]);
		return;
	}
	action_begin(panel, MERGE_NONE);
	/* a setting moved: that seed with that setting. Otherwise the next seed. */
	if (!panel->settings_changed) panel->seed = (uint16_t)wrapped(panel->seed + 1, 10000);
	generate(panel);
	action_end(panel);
	say(panel, "NEW PATTERN ", pocket_gen_style_name((unsigned)panel->style));
}

static void mutate_button(struct pocket_panel* panel, int one_track)
{
	if (one_track && !track_free(panel)) return;
	struct pocket_pattern* next = pocket_seq_edit_begin();
	action_begin(panel, MERGE_NONE);
	pocket_gen_mutate(next, ((uint32_t)panel->seed << 16) | panel->mutations++, (unsigned)panel->mutate_strength,
	                  one_track ? POCKET_GEN_ALL_TRACKS & ~(1u << panel->trigger) : panel->locks);
	pocket_seq_edit_commit();
	pattern_changed(panel);
	action_end(panel);
	say(panel, "MUTATED ", one_track ? triggerNames[panel->trigger] : mutateNames[panel->mutate_strength]);
}

/* OCT-/OCT+. In PITCH, the octave; in DRUM, how hard the keys hit. Under SAVE, undo and redo. */
static void octave_button(struct pocket_panel* panel, int by, int held)
{
	if (held)
	{
		history_move(panel, by > 0);
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

static void buttons(struct pocket_panel* panel, uint32_t pressed, uint32_t released, uint32_t held)
{
	const int save = (held & BTN(SAVE)) != 0;
	if (pressed & BTN(SAVE))
	{
		panel->save_down = 1;
		panel->save_chorded = 0;
		panel->dirty |= 1u << B_FOOTER;
	}
	/* SAVE is a tap only if nothing else happened while it was down */
	if (pressed & ~BTN(SAVE)) panel->save_chorded |= (uint8_t)save;
	if ((released & BTN(SAVE)) && panel->save_down)
	{
		panel->save_down = 0;
		panel->dirty |= 1u << B_FOOTER;
		if (!panel->save_chorded && panel->page == POCKET_PAGE_STORE) store_save(panel);
		else if (!panel->save_chorded) store_open(panel);
	}
	if ((pressed & BTN(FX)) && save)
	{
		panel->locks ^= (uint8_t)(1u << panel->trigger);
		panel->dirty |= (1u << B_VOICE) | GRID_BANDS;
		say(panel, panel->locks >> panel->trigger & 1u ? "LOCKED " : "UNLOCKED ", triggerNames[panel->trigger]);
	}
	if (pressed & BTN(PLAY))
	{
		pocket_seq_play(!pocket_seq_playing());
		pocket_history_close(); /* a take ends with the transport */
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
		pocket_history_close();
		say(panel, "REC ", offOn[panel->recording]);
	}
	if ((pressed & BTN(OCT_DOWN)) && !save && panel->page == POCKET_PAGE_STORE) store_load(panel);
	else if (pressed & BTN(OCT_DOWN)) octave_button(panel, -1, save);
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

	const int save = (in->buttons & BTN(SAVE)) != 0;
	unsigned turned = 0;
	for (int e = 0; e < POCKET_ENCODERS; e++) turned |= (uint16_t)in->encoders[e];
	navigate(panel, pressedButtons, save);
	buttons(panel, pressedButtons, releasedButtons, in->buttons);
	if (turned || pressedKeys) panel->save_chorded |= (uint8_t)save;
	if (in->encoders[POCKET_ENC_SELECT])
	{
		if (panel->page == POCKET_PAGE_STORE) move_slot(panel, in->encoders[POCKET_ENC_SELECT]);
		else select_voice(panel, wrapped(panel->trigger + in->encoders[POCKET_ENC_SELECT], POCKET_LXR_TRIGGERS));
	}
	if (in->encoders[POCKET_ENC_ALGORITHM])
	{
		panel->style = (int8_t)wrapped(panel->style + in->encoders[POCKET_ENC_ALGORITHM], POCKET_GEN_STYLES);
		panel->settings_changed = 1;
		say(panel, "STYLE ", pocket_gen_style_name((unsigned)panel->style));
		panel->dirty |= BODY_BANDS;
	}
	if (in->encoders[POCKET_ENC_PRESETS]) kit_browse(panel, in->encoders[POCKET_ENC_PRESETS]);
	page_knobs(panel, &in->encoders[POCKET_ENC_KNOB1], save);
	for (int k = 0; k < POCKET_KEYS; k++)
		if (pressedKeys & (1u << k)) key_hit(panel, k);
	if (pressedKeys && !pocket_seq_playing()) pocket_history_close(); /* a chord is one entry */
	if (panel->entered)
	{
		panel->entered = 0;
		if (panel->auto_step) move_step(panel, panel->auto_step);
	}
	return 0;
}

/* ------------------------------------------------------------------ drawing */
#define PAPER 0x0000u
#define INK 0xffffu
#define ACCENT 0xfbe0u  /* orange: what is selected, sounding or recording, and nothing else */
#define DIM 0x8c51u     /* what explains the white: labels and the footer */
#define FAINT 0x2965u   /* what the eye need not read: rules, rests, the ends of bars */
#define ADVANCE 8
#define TEXT_Y ((POCKET_BAND_ROWS - 10) / 2)
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

/* A band's one line, from the left margin. */
static int line_text(uint16_t* px, const char* s, uint16_t color)
{
	return text(px, MARGIN, TEXT_Y, s, color);
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
	line_text(px, label, DIM);
	text(px, 96, TEXT_Y, value, INK);
}

static void label_number(uint16_t* px, const char* label, uint32_t value)
{
	char a[12];
	label_value(px, label, number(a, value, 0));
}

/* Two bands of one control: name and value, then a bar. */
static void control(uint16_t* px, int control, int value, const char* knob, unsigned second)
{
	if (!second)
	{
		char a[12];
		line_text(px, knob, DIM);
		text(px, MARGIN + 7 * ADVANCE, TEXT_Y, controlNames[control], INK);
		text(px, POCKET_SCREEN_W - MARGIN - 3 * ADVANCE, TEXT_Y, number(a, (uint32_t)value, 3), ACCENT);
		return;
	}
	const int w = POCKET_SCREEN_W - 2 * MARGIN;
	fill(px, MARGIN, 5, w, POCKET_BAND_ROWS - 10, FAINT);
	if (control == POCKET_CTL_PAN)
	{
		const int mid = MARGIN + w / 2, at = MARGIN + 2 + (w - 5) * value / 127;
		fill(px, mid, 3, 1, POCKET_BAND_ROWS - 6, DIM);
		fill(px, at - 1, 3, 3, POCKET_BAND_ROWS - 6, ACCENT);
	}
	else
		fill(px, MARGIN, 5, w * value / 127, POCKET_BAND_ROWS - 10, INK);
}

/* The step grid: a page of 16 steps fits the 240 pixels. Beats are set apart. */
#define GRID_X 30
#define CELL_W 12
#define ROW_H 11
#define GRID_ROWS_TOP 1  /* in the first grid band */

static int step_x(int step)
{
	return GRID_X + step * CELL_W + (step / 4) * 2;
}

/* A step: a bar as tall as its velocity, orange if it plays a note of its own, hollow or striped
 * if it is a variation, with an orange corner if a condition decides when; a dot when off. */
static void cell(uint16_t* px, int x, int y, uint32_t step, int selected)
{
	const int velocity = (int)POCKET_CELL_VELOCITY(step), variation = (int)POCKET_CELL_VARIATION(step);
	if (selected)
	{
		fill(px, x, y, CELL_W - 1, ROW_H, ACCENT);
		fill(px, x + 1, y + 1, CELL_W - 3, ROW_H - 2, PAPER);
	}
	if (velocity)
	{
		const int h = 1 + velocity * 7 / 127, top = y + ROW_H - 2 - h;
		fill(px, x + 2, top, CELL_W - 5, h, POCKET_CELL_NOTE(step) ? ACCENT : INK);
		if (variation & 1) fill(px, x + 3, top + 1, CELL_W - 7, h - 2, PAPER);
		else if (variation)
			for (int r = 1; r < h; r += 2) fill(px, x + 2, top + r - 1, CELL_W - 5, 1, PAPER);
		if (POCKET_CELL_CONDITION(step)) fill(px, x + 7, y + 1, 2, 2, ACCENT);
	}
	else
		fill(px, x + 4, y + 4, 2, 2, FAINT);
}

static void draw_grid(const struct pocket_panel* panel, unsigned line, uint16_t* px)
{
	const struct pocket_pattern* pattern = pocket_seq_pattern();
	const int top = GRID_ROWS_TOP - (int)line * POCKET_BAND_ROWS, first = (int)shown_page(panel) * POCKET_SEQ_STEPS;
	for (int t = 0; t < POCKET_SEQ_TRACKS; t++)
	{
		const int y = top + t * ROW_H;
		if (y + ROW_H <= 0 || y >= POCKET_BAND_ROWS) continue;
		const int selected = t == panel->trigger;
		text_scaled(px, MARGIN, y, cellNames[t], selected ? ACCENT : DIM, 2);
		if (panel->locks & (1u << t)) fill(px, GRID_X - 5, y + 1, 3, ROW_H - 3, ACCENT);
		for (int s = 0; s < POCKET_SEQ_STEPS; s++)
			cell(px, step_x(s), y, pattern->cell[t][first + s], selected && first + s == panel->step);
	}
}

/* A row of the pattern's activity: how many tracks play each step, the playhead over it. */
static void draw_ruler(const struct pocket_panel* panel, int cursor, uint16_t* px)
{
	const struct pocket_pattern* pattern = pocket_seq_pattern();
	const int position = pocket_seq_position(), first = (int)shown_page(panel) * POCKET_SEQ_STEPS;
	for (int s = 0; s < POCKET_SEQ_STEPS; s++)
	{
		const int x = step_x(s), here = first + s == position;
		int tracks = 0;
		for (int t = 0; t < POCKET_SEQ_TRACKS; t++) tracks += pattern->cell[t][first + s] != 0;
		if (here) fill(px, x, 1, CELL_W - 1, 13, ACCENT);
		if (tracks) fill(px, x + 2, 13 - 2 * tracks, CELL_W - 5, 2 * tracks, here ? PAPER : DIM);
		if (!(s & 3)) fill(px, x, 13, CELL_W - 1, 1, DIM);
		if (cursor && first + s == panel->step) fill(px, x, 14, CELL_W - 1, 2, ACCENT);
	}
}

/* PLAY or STOP. Returns the x after it. */
static int transport(uint16_t* px, int y)
{
	const int playing = pocket_seq_playing();
	return text(px, MARGIN, y, playing ? "PLAY" : "STOP", playing ? ACCENT : INK);
}

/* PAGE 2/4, and a block a page: orange where it is playing, white the one on screen. */
static void draw_transport(const struct pocket_panel* panel, uint16_t* px)
{
	const int y = (POCKET_BAND_ROWS - 10) / 2, pages = pocket_seq_pattern()->last_page + 1, position = pocket_seq_position();
	const int shown = (int)shown_page(panel);
	char b[24];
	transport(px, y);
	append(append_number(b, (uint32_t)pocket_seq_tempo()), " BPM");
	text(px, 48, y, b, INK);
	append_number(append(append_number(append(b, "PAGE "), (uint32_t)shown + 1u), "/"), (uint32_t)pages);
	text(px, 128, y, b, INK);
	for (int p = 0; p < POCKET_SEQ_PAGES; p++)
		fill(px, 204 + p * 7, 5, 5, 6, position != POCKET_SEQ_NOT_PLAYING && position / POCKET_SEQ_STEPS == p ? ACCENT : p == shown ? INK : p < pages ? DIM : FAINT);
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
	line_text(px, b, DIM);
}

static const char knobNames[4][7] = { "KNOB 1", "KNOB 2", "KNOB 3", "KNOB 4" };
static const char conditionNames[POCKET_CONDITIONS][4] = { "", "1:2", "2:2", "1:3", "2:3", "3:3", "1:4", "2:4", "3:4", "4:4", "25%", "50%", "75%" };
static const char variationNames[POCKET_VARIATIONS][3] = { "", "A", "B", "X", "XA", "XB" };

/* What a DRUM key plays, by name: there are nine, and it is not the voice's. */
static void bank_text(char* d, int key)
{
	static const char* const banks[3] = { "", " A", " B" };
	append(append(d, bankNames[key % POCKET_BANK_KEYS]), banks[key / POCKET_BANK_KEYS]);
}

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
	if (line == L_TRANSPORT) draw_transport(panel, px);
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
		fill(px, x, top + height - h, 20, h, v == selected ? ACCENT : INK);
		text(px, x + 2, top + height + 2, names[v], v == selected ? ACCENT : DIM);
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

/* A row that can be selected: orange, with a mark in the margin. */
static uint16_t row_ink(uint16_t* px, int selected)
{
	if (selected) fill(px, 0, 2, 4, POCKET_BAND_ROWS - 4, ACCENT);
	return selected ? ACCENT : INK;
}

/* Eight rows of the saved projects, scrolled to keep the selected one. */
static void draw_store(const struct pocket_panel* panel, unsigned line, uint16_t* px)
{
	const int top = panel->store_slot < 8 ? 0 : panel->store_slot - 7, slot = top + (int)line;
	char b[34], *d = b;
	if (panel->store_state < 0)
	{
		if (line) return;
		line_text(px, panel->store_state == POCKET_STORE_UNAVAILABLE ? "NO USER FLASH" : panel->store_state == POCKET_STORE_OCCUPIED ? "FLASH IN USE: SAVING OFF" : "FLASH ERROR", INK);
		return;
	}
	if (slot >= store_rows()) return;
	const uint16_t ink = row_ink(px, slot == panel->store_slot);
	if (slot == SLOT_FORGET)
		append(d, "ERASE OLD SAVES");
	else if (slot < SLOT_OLD_KITS)
	{
		struct saved_head head;
		d = number(d, (uint32_t)slot + 1u, 2) + 2;
		d = append(d, " ");
		pocket_store_peek((unsigned)slot, &head, sizeof head);
		if ((unsigned)slot >= pocket_store_room()) append(d, "HELD BY OLD SAVES");
		else if (!head.rhythm.used) append(d, "--");
		else
		{
			sound_text(d, head.sound.kit, head.sound.generated, head.sound.seed, head.sound.edited);
			while (*d) d++;
			d = append_number(append(d, " "), head.rhythm.tempo);
			if (head.rhythm.last_page) append_number(append(d, " "), ((head.rhythm.last_page & 3u) + 1u) * POCKET_SEQ_STEPS);
		}
	}
	else
	{
		const unsigned kind = slot < SLOT_OLD_PATTERNS ? POCKET_STORE_OLD_KITS : POCKET_STORE_OLD_PATTERNS, old = (unsigned)(slot - SLOT_OLD_KITS) % POCKET_STORE_OLD_SLOTS;
		union { struct saved_rhythm rhythm; struct saved_sound_head sound; } head;
		d = append_number(append(d, kind == POCKET_STORE_OLD_KITS ? "OLD KIT " : "OLD PAT "), old + 1u);
		d = append(d, " ");
		pocket_store_old_peek(kind, old, &head, sizeof head.rhythm);
		if (!head.sound.used) append(d, "--");
		else if (kind == POCKET_STORE_OLD_KITS) sound_text(d, head.sound.kit, head.sound.generated, head.sound.seed, head.sound.edited);
		else append_number(append(append(d, pocket_gen_style_name(head.rhythm.style)), " "), head.rhythm.tempo);
	}
	line_text(px, b, ink);
}

/* Eight rows of GLOBAL, scrolled to keep the selected one. */
static void draw_global(const struct pocket_panel* panel, unsigned line, uint16_t* px)
{
	const int top = panel->global_row < 8 ? 0 : panel->global_row - 7, row = top + (int)line;
	const uint16_t ink = row_ink(px, row == panel->global_row);
	char b[16];
	line_text(px, rows[row].name, ink == ACCENT ? ACCENT : DIM);
	row_text(panel, row, b);
	text(px, 112, TEXT_Y, b, ink);
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
	fill(px, 0, 0, POCKET_SCREEN_W, POCKET_BAND_ROWS, PAPER);
	text_scaled(px, 36, top + 56, "CLATTER POCKET", INK, 3);
	text(px, 56, top + 84, "LXR DRUM MACHINE", ACCENT);
	text(px, MARGIN, top + 136, states[status->state < 4 ? status->state : 3], INK);
	text(px, MARGIN, top + 152, panel->footer, INK);
	text(px, MARGIN, top + 200, POCKET_FW_VERSION, DIM);
	text(px, MARGIN, top + 216, status->build, DIM);
}

void pocket_panel_draw_band(const struct pocket_panel* panel, const struct pocket_panel_status* status,
                            unsigned band, uint16_t* px)
{
	static const struct pocket_panel_status none = { 0 };
	char a[24], b[48];
	if (!status) status = &none;
	if (status->state != POCKET_STATE_READY)
	{
		draw_identity(panel, status, band, px);
		return;
	}
	fill(px, 0, 0, POCKET_SCREEN_W, POCKET_BAND_ROWS, PAPER);
	if (band == B_TITLE)
	{
		const char* name = pageNames[panel->page];
		int n = 0;
		while (name[n]) n++;
		line_text(px, "CLATTER POCKET", INK);
		text(px, POCKET_SCREEN_W - MARGIN - n * ADVANCE, TEXT_Y, name, ACCENT);
		fill(px, 0, POCKET_BAND_ROWS - 1, POCKET_SCREEN_W, 1, FAINT);
		return;
	}
	if (band == B_FOOTER)
	{
		const char* footer = pageFooters[panel->page];
		if (panel->save_down && panel->page != POCKET_PAGE_STORE)
			footer = panel->page == POCKET_PAGE_PATTERN ? "K1 COND K3 LEN K4 PAGE" : "OCT- UNDO OCT+ REDO FX LOCK";
		fill(px, 0, 0, POCKET_SCREEN_W, 1, FAINT);
		line_text(px, footer, panel->save_down ? ACCENT : DIM);
		return;
	}
	if (band == B_STATE)
	{
		if (panel->alert_ms)
		{
			fill(px, 0, 0, POCKET_SCREEN_W, POCKET_BAND_ROWS, ACCENT);
			line_text(px, concat(b, "AUDIO OVERRUN ", number(a, panel->alert_overruns, 0)), PAPER);
		}
		else if (panel->notice_ms)
			line_text(px, panel->notice, ACCENT);
		else
		{
			char* d = append(append(b, "KEYS: "), keyModes[panel->key_mode]);
			if (panel->key_mode == POCKET_KEYS_PITCH)
			{
				d = append(d, panel->octave < 0 ? " OCT -" : " OCT ");
				append_number(d, (uint32_t)(panel->octave < 0 ? -panel->octave : panel->octave));
			}
			else
				append_number(append(d, " VEL "), (uint32_t)panel->key_velocity);
			const int x = line_text(px, b, INK);
			if (panel->recording) text(px, x + 2 * ADVANCE, TEXT_Y, "REC ON", ACCENT);
		}
		return;
	}
	if (band == B_VOICE)
	{
		const int x = line_text(px, "VOICE ", DIM);
		text(px, x, TEXT_Y, triggerNames[panel->trigger], ACCENT);
		if (panel->page == POCKET_PAGE_PATTERN)
		{
			const uint32_t c = pocket_seq_pattern()->cell[panel->trigger][panel->step];
			const unsigned note = POCKET_CELL_NOTE(c), variation = POCKET_CELL_VARIATION(c), condition = POCKET_CELL_CONDITION(c);
			char* d = append_number(append(b, note || variation || condition ? "S" : "STEP "), panel->step + 1u);
			if (note) d = append_number(append(d, " N"), note);
			if (variation) d = append(append(d, " "), variationNames[variation]);
			if (condition) append(append(d, " "), conditionNames[condition]);
			text(px, 128, TEXT_Y, b, INK);
			if (panel->locks & (1u << panel->trigger)) fill(px, 122, 2, 2, POCKET_BAND_ROWS - 4, ACCENT);
		}
		else if (panel->last_key)
		{
			const int key = panel->last_key - 1;
			bank_text(b, key);
			text(px, 128, TEXT_Y, b, INK);
		}
		return;
	}
	if (band == B_MIDI)
	{
		if (panel->midi_trigger < POCKET_LXR_TRIGGERS)
		{
			const uint16_t ink = panel->midi_flash_ms ? ACCENT : DIM;
			const int x = line_text(px, concat(b, "MIDI NOTE ", number(a, panel->midi_note, 0)), ink);
			text(px, x + ADVANCE, TEXT_Y, triggerNames[panel->midi_trigger], ink);
		}
		else if (status->midi_messages)
			line_text(px, concat(b, "MIDI MESSAGES ", number(a, status->midi_messages, 0)), DIM);
		else
			line_text(px, "MIDI WAITING", DIM);
		return;
	}
	if (band == B_ACTIVITY)
	{
		for (int t = 0; t < POCKET_LXR_TRIGGERS; t++)
		{
			const int x = 4 + t * 33;
			if (panel->flash_ms[t]) fill(px, x + 1, 1, 32, POCKET_BAND_ROWS - 3, ACCENT);
			text(px, x + 9, TEXT_Y, cellNames[t], panel->flash_ms[t] ? PAPER : DIM);
		}
		return;
	}
	if (band == B_LEVEL)
	{
		const int x = line_text(px, "LEVEL", DIM) + ADVANCE;
		const int w = POCKET_SCREEN_W - MARGIN - x, level = status->level > 100 ? 100 : status->level;
		fill(px, x, 5, w, POCKET_BAND_ROWS - 10, FAINT);
		fill(px, x, 5, w * level / 100, POCKET_BAND_ROWS - 10, level >= 90 ? ACCENT : INK);
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
		case POCKET_PAGE_STORE: draw_store(panel, line, px); break;
		default: draw_system(status, line, px); break;
		}
	}
}
