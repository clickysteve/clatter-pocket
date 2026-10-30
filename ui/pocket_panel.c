/*
 * pocket_panel.c - CLATTER Pocket's hardware interface. See pocket_panel.h; the control map
 * and the screens are in docs/POCKET-UI.md.
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Only what the firmware can do is on screen: every knob moves an engine parameter that the
 * host tests hear change, or the sequencer. Nothing here touches Flash; the one thing a control
 * can lose is a pattern, and ENV undoes that.
 *
 * Drawing: the Slop Factory 1-bit look (black on white, inverted title and footer), a 3x5
 * pixel font at twice its size, a band at a time so a page costs one 240x16 buffer.
 */
#include "pocket_panel.h"
#include "pocket_params.h"
#include "pocket_gen.h"

static const char* const triggerNames[POCKET_LXR_TRIGGERS] = {
	"DRUM 1", "DRUM 2", "DRUM 3", "SNARE", "CYMBAL", "CL HAT", "OP HAT"
};
static const char* const cellNames[POCKET_LXR_TRIGGERS] = { "D1", "D2", "D3", "SN", "CY", "CH", "OH" };
static const char* const pageNames[POCKET_PAGES] = { "MAIN", "PATTERN", "SOUND", "MIXER", "SYSTEM" };
static const char* const pageFooters[POCKET_PAGES] = {
	"PLAY START/STOP  KEYS HIT",
	"REC STEP ARP GEN LFO MUTATE",
	"SEL VOICE  KNOBS EDIT",
	"SEL VOICE  KNOBS EDIT",
	"PLAY START/STOP  KEYS HIT"
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

/* The frame of every page; the page's own bands are B_BODY..B_ACTIVITY-1. */
enum { B_TITLE = 0, B_STATE = 1, B_VOICE = 2, B_MIDI = 3, B_BODY = 4, B_ACTIVITY = 12, B_LEVEL = 13, B_FOOTER = 14 };
#define ALL_BANDS ((uint16_t)((1u << POCKET_BANDS) - 1))
#define BODY_BANDS ((uint16_t)(((1u << B_ACTIVITY) - 1) & ~((1u << B_BODY) - 1)))
/* PATTERN's bands within the body, and MAIN's line that follows the pattern. */
enum { L_TRANSPORT = 0, L_RULER = 1, L_GRID = 2, L_GRID_LAST = 6, L_LEGEND = 7, L_MAIN_HITS = 4, L_MAIN_RULER = 6 };
#define GRID_BANDS ((uint16_t)(((2u << (B_BODY + L_GRID_LAST)) - 1) & ~((1u << (B_BODY + L_GRID)) - 1)))
#define PATTERN_CHANGED ((uint16_t)(GRID_BANDS | (1u << (B_BODY + L_RULER)) | (1u << (B_BODY + L_MAIN_RULER))))

void pocket_panel_init(struct pocket_panel* panel)
{
	*panel = (struct pocket_panel){ 0 };
	panel->trigger = POCKET_TRIG_DRUM1;
	panel->midi_trigger = 0xff;
	panel->previous_buttons = ~0u; /* buttons held on entry do not act */
	panel->dirty = ALL_BANDS;
	panel->footer = "";
	panel->density = 50;
	panel->complexity = 30;
	panel->seed = 1;
	panel->mutate_strength = POCKET_MUTATE_MEDIUM;
	panel->shown_position = POCKET_SEQ_NOT_PLAYING;
}

static void generate(struct pocket_panel* panel)
{
	const struct pocket_gen_settings settings = { panel->seed, panel->density, panel->complexity, panel->style, panel->locks };
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

static void hit(struct pocket_panel* panel, int note)
{
	if (pocket_lxr_trigger(panel->trigger, 127, note) == 0)
	{
		panel->hits++;
		panel->flash_ms[panel->trigger] = POCKET_FLASH_MS;
		panel->dirty |= (1u << B_ACTIVITY) | (1u << (B_BODY + 5));
	}
}

void pocket_panel_midi_hit(struct pocket_panel* panel, int trigger, unsigned note)
{
	if (trigger < 0 || trigger >= POCKET_LXR_TRIGGERS) return;
	panel->midi_trigger = (uint8_t)trigger;
	panel->midi_note = (uint8_t)note;
	panel->midi_flash_ms = POCKET_MIDI_FLASH_MS;
	panel->flash_ms[trigger] = POCKET_FLASH_MS;
	panel->dirty |= (1u << B_MIDI) | (1u << B_ACTIVITY);
}

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

static void go(struct pocket_panel* panel, unsigned page)
{
	if (page == panel->page) return;
	panel->page = (uint8_t)page;
	panel->dirty = ALL_BANDS;
}

static void navigate(struct pocket_panel* panel, uint32_t pressed)
{
	if (pressed & (1u << POCKET_BTN_OCT_UP)) go(panel, (panel->page + 1u) % POCKET_PAGES);
	if (pressed & (1u << POCKET_BTN_OCT_DOWN)) go(panel, (panel->page + POCKET_PAGES - 1u) % POCKET_PAGES);
	if (pressed & (1u << POCKET_BTN_HOME)) go(panel, POCKET_PAGE_MAIN);
	if (pressed & (1u << POCKET_BTN_SEQ)) go(panel, POCKET_PAGE_PATTERN);
	if (pressed & (1u << POCKET_BTN_EDIT)) go(panel, POCKET_PAGE_SOUND);
	if (pressed & (1u << POCKET_BTN_FX)) go(panel, POCKET_PAGE_MIXER);
	if (pressed & (1u << POCKET_BTN_GLO)) go(panel, POCKET_PAGE_SYSTEM);
}

static void edit(struct pocket_panel* panel, int control, int band, int delta)
{
	const int p = pocket_panel_param(panel, control);
	int v = pocket_lxr_get_param(p) + delta;
	v = v < 0 ? 0 : v > 127 ? 127 : v;
	pocket_lxr_set_param(p, v);
	panel->dirty |= 3u << band;
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

/* The knobs of PATTERN: on the step (and the tempo and swing), or on the generator. */
static void pattern_knobs(struct pocket_panel* panel, const int16_t* knob)
{
	if (!(knob[0] | knob[1] | knob[2] | knob[3])) return;
	if (panel->generator_knobs)
	{
		panel->density = (uint8_t)limited(panel->density + 5 * knob[0], 0, 100);
		panel->complexity = (uint8_t)limited(panel->complexity + 5 * knob[1], 0, 100);
		panel->seed = (uint16_t)wrapped(panel->seed + knob[2], 10000);
		panel->style = (uint8_t)wrapped(panel->style + knob[3], POCKET_GEN_STYLES);
		panel->settings_changed = 1;
		panel->dirty |= (1u << (B_BODY + L_TRANSPORT)) | (1u << (B_BODY + L_LEGEND));
		return;
	}
	const int v = pocket_seq_pattern()->vel[panel->trigger][panel->step];
	if (knob[0] && v)
	{
		pocket_seq_set_step(panel->trigger, panel->step, limited(v + 4 * knob[0], 1, 127));
		panel->dirty |= GRID_BANDS;
	}
	if (knob[1])
	{
		pocket_seq_set_tempo(pocket_seq_tempo() + knob[1]);
		pocket_lxr_set_bpm(pocket_seq_tempo()); /* the engine's tempo-synced LFOs */
	}
	if (knob[2]) pocket_seq_set_swing(pocket_seq_swing() + knob[2]);
	if (knob[3]) panel->mutate_strength = (uint8_t)limited(panel->mutate_strength + knob[3], 0, POCKET_MUTATE_STRENGTHS - 1);
	panel->dirty |= (1u << (B_BODY + L_TRANSPORT)) | (1u << (B_BODY + L_LEGEND));
}

static void pattern_controls(struct pocket_panel* panel, const struct pocket_panel_input* in, uint32_t pressed)
{
	const int step = in->encoders[POCKET_ENC_ALGORITHM];
	if (step)
	{
		panel->step = (uint8_t)wrapped(panel->step + step, POCKET_SEQ_STEPS);
		panel->dirty |= (1u << B_VOICE) | GRID_BANDS | (1u << (B_BODY + L_RULER)) | (1u << (B_BODY + L_LEGEND));
	}
	pattern_knobs(panel, &in->encoders[POCKET_ENC_KNOB1]);
	if (pressed & (1u << POCKET_BTN_REC))
	{
		const int v = pocket_seq_pattern()->vel[panel->trigger][panel->step];
		pocket_seq_set_step(panel->trigger, panel->step, v ? 0 : POCKET_SEQ_VEL_DEFAULT);
		panel->dirty |= PATTERN_CHANGED | (1u << (B_BODY + L_LEGEND));
	}
	if (pressed & (1u << POCKET_BTN_SCL))
	{
		panel->generator_knobs = !panel->generator_knobs;
		panel->dirty |= (1u << (B_BODY + L_TRANSPORT)) | (1u << (B_BODY + L_LEGEND));
	}
	if (pressed & (1u << POCKET_BTN_SAVE))
	{
		panel->locks ^= (uint8_t)(1u << panel->trigger);
		panel->dirty |= (1u << B_VOICE) | GRID_BANDS;
	}
	if (pressed & (1u << POCKET_BTN_ARP))
	{
		/* a setting moved: that seed with that setting. Otherwise the next seed. */
		if (!panel->settings_changed) panel->seed = (uint16_t)wrapped(panel->seed + 1, 10000);
		generate(panel);
		panel->dirty |= 1u << (B_BODY + L_LEGEND);
	}
	if (pressed & (1u << POCKET_BTN_LFO))
	{
		struct pocket_pattern* next = pocket_seq_edit_begin();
		pocket_gen_mutate(next, ((uint32_t)panel->seed << 16) | panel->mutations++, panel->mutate_strength, panel->locks);
		pocket_seq_edit_commit();
		panel->dirty |= PATTERN_CHANGED;
	}
	if (pressed & (1u << POCKET_BTN_ENV))
	{
		pocket_seq_undo();
		panel->dirty |= PATTERN_CHANGED | (1u << (B_BODY + L_LEGEND));
	}
}

int pocket_panel_apply(struct pocket_panel* panel, const struct pocket_panel_input* in)
{
	const uint32_t pressedButtons = in->buttons & ~panel->previous_buttons;
	const uint32_t pressedKeys = in->keys & ~panel->previous_keys;
	const int exit = POCKET_PANEL_TOGGLE(panel->previous_buttons, in->buttons);
	panel->previous_buttons = in->buttons;
	panel->previous_keys = in->keys;
	if (exit) return 1;

	navigate(panel, pressedButtons);
	if (in->encoders[POCKET_ENC_SELECT])
	{
		int t = (panel->trigger + in->encoders[POCKET_ENC_SELECT]) % POCKET_LXR_TRIGGERS;
		if (t < 0) t += POCKET_LXR_TRIGGERS;
		panel->trigger = (uint8_t)t;
		panel->dirty |= (1u << B_VOICE) | BODY_BANDS;
	}
	if (pressedButtons & (1u << POCKET_BTN_PLAY))
	{
		pocket_seq_play(!pocket_seq_playing());
		panel->dirty |= (1u << B_BODY); /* MAIN's and PATTERN's first line */
	}
	if (panel->page == POCKET_PAGE_PATTERN)
		pattern_controls(panel, in, pressedButtons);
	else if (panel->page == POCKET_PAGE_SOUND)
	{
		for (int c = 0; c < 4; c++)
			if (in->encoders[POCKET_ENC_KNOB1 + c]) edit(panel, POCKET_CTL_PITCH + c, B_BODY + 2 * c, in->encoders[POCKET_ENC_KNOB1 + c]);
	}
	else if (panel->page == POCKET_PAGE_MIXER)
	{
		for (int c = 0; c < 2; c++)
			if (in->encoders[POCKET_ENC_KNOB1 + c]) edit(panel, POCKET_CTL_LEVEL + c, B_BODY + 2 * c, in->encoders[POCKET_ENC_KNOB1 + c]);
		if (in->encoders[POCKET_ENC_KNOB1]) panel->dirty |= BODY_BANDS; /* the channel bars */
	}
	for (int k = 0; k < POCKET_KEYS; k++)
		if (pressedKeys & (1u << k)) hit(panel, 48 + k);
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
	char* d = dst;
	while (*a) *d++ = *a++;
	while (*b) *d++ = *b++;
	*d = 0;
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

/* A step: a bar as tall as its velocity, a dot when off. */
static void cell(uint16_t* px, int x, int y, int velocity, int selected)
{
	if (selected)
	{
		fill(px, x, y, CELL_W - 1, ROW_H, INK);
		fill(px, x + 1, y + 1, CELL_W - 3, ROW_H - 2, PAPER);
	}
	if (velocity)
	{
		const int h = 1 + velocity * 7 / 127;
		fill(px, x + 2, y + ROW_H - 2 - h, CELL_W - 5, h, INK);
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
			cell(px, step_x(s), y, pattern->vel[t][s], selected && s == panel->step);
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
		for (int t = 0; t < POCKET_SEQ_TRACKS; t++) tracks += pattern->vel[t][s] != 0;
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

static char* append(char* d, const char* s)
{
	while (*s) *d++ = *s++;
	*d = 0;
	return d;
}

static char* append_number(char* d, uint32_t v)
{
	char a[12];
	return append(d, number(a, v, 0));
}

static void draw_transport(const struct pocket_panel* panel, uint16_t* px)
{
	const int y = (POCKET_BAND_ROWS - 10) / 2;
	char b[24], *d = b;
	transport(px, y);
	d = append_number(d, (uint32_t)pocket_seq_tempo());
	append(d, " BPM");
	text(px, 48, y, b, INK);
	if (panel->generator_knobs)
		text(px, 112, y, pocket_gen_style_name(panel->style), INK);
	else
	{
		d = append(b, "SWING ");
		append_number(d, (uint32_t)pocket_seq_swing());
		text(px, 128, y, b, INK);
	}
}

/* The four knobs' fields, in knob order. */
static void draw_legend(const struct pocket_panel* panel, uint16_t* px)
{
	char b[40], *d = b;
	if (panel->generator_knobs)
	{
		d = append_number(append(d, "DEN"), panel->density);
		d = append_number(append(d, " CPX"), panel->complexity);
		d = append_number(append(d, " SEED"), panel->seed);
		append(d, " STYLE");
	}
	else
	{
		const int v = pocket_seq_pattern()->vel[panel->trigger][panel->step];
		d = append(d, "VEL");
		d = v ? append_number(d, (uint32_t)v) : append(d, "--");
		d = append_number(append(d, " BPM"), (uint32_t)pocket_seq_tempo());
		d = append_number(append(d, " SWG"), (uint32_t)pocket_seq_swing());
		d = append(append(d, " MUT"), mutateNames[panel->mutate_strength]);
	}
	text(px, MARGIN, (POCKET_BAND_ROWS - 10) / 2, b, INK);
}

static const char* const knobNames[4] = { "KNOB 1", "KNOB 2", "KNOB 3", "KNOB 4" };

static void draw_main(const struct pocket_panel* panel, unsigned line, uint16_t* px)
{
	char a[12], b[16];
	if (line == 0) transport(px, (POCKET_BAND_ROWS - 10) / 2);
	if (line == 1) label_value(px, "TEMPO", concat(b, number(a, (uint32_t)pocket_seq_tempo(), 0), " BPM"));
	if (line == 2) label_number(px, "SWING", (uint32_t)pocket_seq_swing());
	if (line == 3) label_value(px, "STYLE", pocket_gen_style_name(panel->style));
	if (line == L_MAIN_HITS) label_number(px, "SEQ HITS", pocket_seq_triggers());
	if (line == 5) label_number(px, "HITS", panel->hits);
	if (line == L_MAIN_RULER) draw_ruler(panel, 0, px);
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
			text(px, MARGIN, y, pageFooters[panel->page], PAPER);
		return;
	}
	if (band == B_STATE)
	{
		if (panel->alert_ms)
		{
			fill(px, 0, 0, POCKET_SCREEN_W, POCKET_BAND_ROWS, INK);
			text(px, MARGIN, y, concat(b, "AUDIO OVERRUN ", number(a, panel->alert_overruns, 0)), PAPER);
		}
		else
			text(px, MARGIN, y, "LXR ENGINE READY", INK);
		return;
	}
	if (band == B_VOICE)
	{
		const int x = text(px, MARGIN, y, "VOICE ", INK);
		fill(px, x - 2, 0, 6 * ADVANCE + 4, POCKET_BAND_ROWS, INK);
		text(px, x, y, triggerNames[panel->trigger], PAPER);
		if (panel->page == POCKET_PAGE_PATTERN)
		{
			text(px, 128, y, concat(b, "STEP ", number(a, panel->step + 1u, 0)), INK);
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
		default: draw_system(status, line, px); break;
		}
	}
}
