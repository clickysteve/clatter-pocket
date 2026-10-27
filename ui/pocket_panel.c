/*
 * pocket_panel.c - CLATTER Pocket's first panel. See pocket_panel.h.
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Input mapping (documented in README.md):
 *   note keys      trigger the selected voice, chromatic: the 13th key (F4) plays note 60,
 *                  the voice's programmed pitch
 *   PLAY           trigger the selected voice at its programmed pitch
 *   encoder 1/2/3  pitch (COARSE), amp decay, level (VOLUME) of the selected voice
 *   encoder 7      select the voice: DRUM 1-3, SNARE, CYMBAL, CL HAT, OP HAT
 *   HOME + ARP     leave CLATTER Pocket mode
 * Drawing: the Slop Factory 1-bit look (black on white, inverted title and footer), a 3x5
 * pixel font drawn at twice its size for the 240x240 panel.
 */
#include "pocket_panel.h"
#include "pocket_lxr.h"
#include "pocket_params.h"

static const char* const triggerNames[POCKET_LXR_TRIGGERS] = {
	"DRUM 1", "DRUM 2", "DRUM 3", "SNARE", "CYMBAL", "CL HAT", "OP HAT"
};
static const uint8_t pitchParam[POCKET_LXR_TRIGGERS] = {
	POCKET_PAR_COARSE1, POCKET_PAR_COARSE2, POCKET_PAR_COARSE3, POCKET_PAR_COARSE4,
	POCKET_PAR_COARSE5, POCKET_PAR_COARSE6, POCKET_PAR_COARSE6
};
static const uint8_t decayParam[POCKET_LXR_TRIGGERS] = {
	POCKET_PAR_VELOD1, POCKET_PAR_VELOD2, POCKET_PAR_VELOD3, POCKET_PAR_VELOD4,
	POCKET_PAR_VELOD5, POCKET_PAR_VELOD6_CLOSED, POCKET_PAR_VELOD6_OPEN
};
static const uint8_t levelParam[POCKET_LXR_TRIGGERS] = {
	POCKET_PAR_VOL1, POCKET_PAR_VOL2, POCKET_PAR_VOL3, POCKET_PAR_VOL4,
	POCKET_PAR_VOL5, POCKET_PAR_VOL6, POCKET_PAR_VOL6
};
static const char* const controlNames[POCKET_CTLS] = { "PITCH", "DECAY", "LEVEL" };

/* bands of the layout */
enum { B_TITLE = 0, B_VOICE = 2, B_CTL0 = 4, B_HITS = 11, B_CPU = 12, B_ERRORS = 13, B_FOOTER = 14 };
#define ALL_BANDS ((uint16_t)((1u << POCKET_BANDS) - 1))

void pocket_panel_init(struct pocket_panel* panel)
{
	panel->trigger = POCKET_TRIG_DRUM1;
	panel->previous_keys = 0;
	panel->previous_buttons = ~0u; /* buttons held on entry do not act */
	panel->dirty = ALL_BANDS;
	panel->hits = 0;
}

int pocket_panel_param(const struct pocket_panel* panel, int c)
{
	const uint8_t* table = c == POCKET_CTL_PITCH ? pitchParam : c == POCKET_CTL_DECAY ? decayParam : levelParam;
	return table[panel->trigger];
}

static void hit(struct pocket_panel* panel, int note)
{
	if (pocket_lxr_trigger(panel->trigger, 127, note) == 0) panel->hits++;
	panel->dirty |= 1u << B_HITS;
}

int pocket_panel_apply(struct pocket_panel* panel, const struct pocket_panel_input* in)
{
	const uint32_t pressedButtons = in->buttons & ~panel->previous_buttons;
	const uint32_t pressedKeys = in->keys & ~panel->previous_keys;
	const int exit = POCKET_PANEL_TOGGLE(panel->previous_buttons, in->buttons);
	panel->previous_buttons = in->buttons;
	panel->previous_keys = in->keys;
	if (exit) return 1;

	if (in->encoders[6])
	{
		int t = (panel->trigger + in->encoders[6]) % POCKET_LXR_TRIGGERS;
		if (t < 0) t += POCKET_LXR_TRIGGERS;
		panel->trigger = (uint8_t)t;
		panel->dirty = ALL_BANDS;
	}
	for (int c = 0; c < POCKET_CTLS; c++)
	{
		if (!in->encoders[c]) continue;
		const int p = pocket_panel_param(panel, c);
		int v = pocket_lxr_get_param(p) + in->encoders[c];
		v = v < 0 ? 0 : v > 127 ? 127 : v;
		pocket_lxr_set_param(p, v);
		panel->dirty |= 3u << (B_CTL0 + 2 * c);
	}
	if (pressedButtons & (1u << POCKET_BTN_PLAY)) hit(panel, 60);
	for (int k = 0; k < POCKET_KEYS; k++)
		if (pressedKeys & (1u << k)) hit(panel, 48 + k);
	return 0;
}

/* ------------------------------------------------------------------ drawing */
#define INK 0x0000u
#define PAPER 0xffffu
#define SCALE 2
#define ADVANCE (4 * SCALE)

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
	default: return 0;
	}
}

static void fill(uint16_t* px, int x, int y, int w, int h, uint16_t color)
{
	for (int r = y < 0 ? 0 : y; r < y + h && r < POCKET_BAND_ROWS; r++)
		for (int c = x < 0 ? 0 : x; c < x + w && c < POCKET_SCREEN_W; c++) px[r * POCKET_SCREEN_W + c] = color;
}

static int text(uint16_t* px, int x, int y, const char* s, uint16_t color)
{
	for (; *s; s++, x += ADVANCE)
	{
		const uint16_t g = glyph(*s);
		for (int r = 0; r < 5; r++)
			for (int c = 0; c < 3; c++)
				if (g & (1u << (14 - 3 * r - c))) fill(px, x + c * SCALE, y + r * SCALE, SCALE, SCALE, color);
	}
	return x;
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

void pocket_panel_draw_band(const struct pocket_panel* panel, const struct pocket_panel_status* status,
                            unsigned band, uint16_t* px)
{
	char a[24], b[48];
	const int y = (POCKET_BAND_ROWS - 5 * SCALE) / 2;
	fill(px, 0, 0, POCKET_SCREEN_W, POCKET_BAND_ROWS, PAPER);
	if (band == B_TITLE || band == B_FOOTER)
	{
		fill(px, 0, 0, POCKET_SCREEN_W, POCKET_BAND_ROWS, INK);
		if (band == B_TITLE)
		{
			text(px, 8, y, "CLATTER POCKET", PAPER);
			text(px, POCKET_SCREEN_W - 8 - 3 * ADVANCE, y, "LXR", PAPER);
		}
		else
			text(px, 8, y, "KEYS/PLAY HIT  HOME+ARP EXIT", PAPER);
		return;
	}
	if (band == B_VOICE)
	{
		const int x = text(px, 8, y, "VOICE ", INK);
		fill(px, x - 2, 0, (int)(6 * ADVANCE) + 4, POCKET_BAND_ROWS, INK);
		text(px, x, y, triggerNames[panel->trigger], PAPER);
		text(px, POCKET_SCREEN_W - 8 - 5 * ADVANCE, y, "ENC 7", INK);
		return;
	}
	if (band >= B_CTL0 && band < B_CTL0 + 2 * POCKET_CTLS)
	{
		const int c = (int)(band - B_CTL0) / 2;
		const int value = pocket_lxr_get_param(pocket_panel_param(panel, c));
		if (((band - B_CTL0) & 1) == 0)
		{
			text(px, 8, y, concat(b, "ENC ", number(a, (uint32_t)c + 1, 0)), INK);
			text(px, 8 + 7 * ADVANCE, y, controlNames[c], INK);
			text(px, POCKET_SCREEN_W - 8 - 3 * ADVANCE, y, number(a, (uint32_t)value, 3), INK);
		}
		else
		{
			const int w = POCKET_SCREEN_W - 16;
			fill(px, 8, 2, w, POCKET_BAND_ROWS - 4, INK);
			fill(px, 9, 3, w - 2, POCKET_BAND_ROWS - 6, PAPER);
			fill(px, 10, 4, (w - 4) * value / 127, POCKET_BAND_ROWS - 8, INK);
		}
		return;
	}
	if (band == B_HITS)
	{
		text(px, 8, y, concat(b, "HITS ", number(a, panel->hits, 0)), INK);
		return;
	}
	if (band == B_CPU && status)
	{
		char c[12], d[12];
		concat(b, "CPU ", percent(c, status->cpu_permille));
		concat(a, "  PEAK ", percent(d, status->cpu_peak_permille));
		char line[48];
		text(px, 8, y, concat(line, b, a), INK);
		return;
	}
	if (band == B_ERRORS && status)
	{
		char n1[12], n2[12], n3[12], l1[24], l2[24], line[48];
		concat(l1, "XRUN ", number(n1, status->overruns, 0));
		concat(l2, " CLIP ", number(n2, status->clipped, 0));
		concat(b, l1, l2);
		concat(l1, " DROP ", number(n3, status->dropped, 0));
		text(px, 8, y, concat(line, b, l1), INK);
		return;
	}
}
