/*
 * pocket_lxr.c - CLATTER Pocket's real-time host for the LXR-02 synthesis engine.
 *
 * Copyright (C) 2026 The CLATTER contributors
 * Adapted from CLATTER's lxr/bridge/clatter_lxr.c, which adapts portions of the LXR firmware
 * (Core/Src/Soundengine/DSPAudio/mixer.c, Core/Src/Soundengine/Soundengine.c,
 * Core/Src/Soundengine/MIDI/MidiVoiceControl.c, Core/Src/Preset/presetManager.c),
 * Copyright (C) 2012-2026 Erica Synths.
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * The synthesis is the firmware's, unchanged (lxr/upstream/): voices, envelopes, filters,
 * modulation, decimation, and the firmware's own parameter handler (midiParser_ccHandler).
 * This file replaces only what surrounds it on the LXR-02 hardware:
 *
 *   - the codec DMA interrupt that called mixer_calcNextSampleBlock becomes
 *     pocket_lxr_render_q23, which runs the same per-block sequence and mixes the six voices
 *     to signed Q23 stereo (the FM-1 SDK's sample format) with the firmware's trigger-time
 *     pan law;
 *   - the front panel and MIDI parser become an event ring (see pocket_lxr.h), drained by the
 *     audio context, so only the audio context touches synthesis state;
 *   - the hardware RNG becomes a seeded xorshift32 (deterministic renders), as in CLATTER.
 *
 * The FX bus (FxBus.c, 64 KB of half-float delay memory) is not used here; see
 * ARCHITECTURE.md for the memory budget that decides when it can come in.
 */
#include "pocket_lxr.h"

#include "Soundengine/config.h"
#include "Soundengine/DSPAudio/DrumVoice.h"
#include "Soundengine/DSPAudio/Snare.h"
#include "Soundengine/DSPAudio/HiHat.h"
#include "Soundengine/DSPAudio/CymbalVoice.h"
#include "Soundengine/DSPAudio/mixer.h"
#include "Soundengine/DSPAudio/modulationNode.h"
#include "Soundengine/DSPAudio/automationNode.h"
#include "Soundengine/DSPAudio/lfo.h"
#include "Soundengine/MIDI/ParameterArray.h"
#include "Soundengine/MIDI/MidiParser.h"
#include "Soundengine/MIDI/MidiMessages.h"
#include "Soundengine/FxBus.h"
#include "Preset/InitKit.h"
#include "menu/menu.h"
#include "main.h"

/* defined in mixer.c, not declared in mixer.h */
void mixer_decimateBlock(const uint8_t voiceNr, int16_t* buffer);

#include "pocket_params.h"
#include "pocket_variations.h"
#ifdef POCKET_PACKED_TABLES
#include "lxr_tables_packed.h"
#endif

_Static_assert(POCKET_LXR_BLOCK == OUTPUT_DMA_SIZE, "engine block must match the firmware");
_Static_assert(POCKET_SOUND_PARAMS == END_OF_SOUND_PARAMETERS, "regenerate engine/pocket_params.h");
_Static_assert(POCKET_PAR_COARSE1 == PAR_COARSE1 && POCKET_PAR_VELOD1 == PAR_VELOD1 &&
               POCKET_PAR_VOL1 == PAR_VOL1 && POCKET_PAR_FX_TYPE == PAR_FX_TYPE &&
               POCKET_PAR_TRANS1_WAVE == PAR_TRANS1_WAVE,
               "regenerate engine/pocket_params.h");
_Static_assert((POCKET_LXR_QUEUE & (POCKET_LXR_QUEUE - 1)) == 0, "queue size must be a power of two");

/* Ordering between the control context's writes to an event and its publication. On a
 * single core with interrupts a compiler barrier suffices; the default is a full fence. */
#ifndef POCKET_BARRIER
#define POCKET_BARRIER() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#endif

/* Everything the audio context runs is marked IN_ITCM_RAM, the firmware's own marker for its
 * per-sample code (config.h), so the FM-1 linker script places it in internal SRAM with the
 * firmware's kernels (firmware/fm1/: .itcm_text). */

/* ------------------------------------------------------------------ hardware replacements */
/* As CLATTER (lxr/bridge/clatter_lxr.c): the DSP's few hardware dependencies. */
RNG_HandleTypeDef hrng = { 0x9e3779b9u };
GPIO_TypeDef clatter_jacks = { 0xffffffffu };
uint8_t activeParameters[NUM_PARAMS];
static uint16_t engineBpm = 120;

uint32_t HAL_RNG_GetRandomNumber(RNG_HandleTypeDef* h)
{
	/* xorshift32: the hardware RNG made reproducible */
	uint32_t x = h->state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	h->state = x ? x : 0x9e3779b9u;
	return h->state;
}

float adc_getFaderVolume(uint8_t voice)
{
	(void)voice;
	return 1.f;
}

uint16_t seq_getBpm(void)
{
	return engineBpm;
}

/* ------------------------------------------------------------------ event ring */
enum { EV_PARAM, EV_TRIGGER, EV_MASTER, EV_BPM, EV_DC_BLOCK };

struct event {
	uint8_t type, a, b, c;  /* PARAM: a/b = p lo/hi, c = value. TRIGGER: trig, vel, note */
	uint8_t p1lo, p1hi, v1, p2lo, p2hi, v2, variation, pad;
	int32_t value;          /* MASTER: gain. BPM: bpm */
};

static struct event ring[POCKET_LXR_QUEUE];
static volatile uint32_t ringHead; /* written by control */
static volatile uint32_t ringTail; /* written by audio */

static uint8_t paramShadow[END_OF_SOUND_PARAMETERS]; /* control's view */
static AutomationNode lockNodes[POCKET_LXR_VOICES][VARIATION_LOCKS]; /* a voice's: the hats share them */
static int32_t masterGain = POCKET_GAIN_UNITY;       /* audio's copy */
static uint8_t dcBlock;                              /* audio's copy */
static pocket_lxr_hook blockHook;                    /* set while the audio context is held off */
/* DC blocker: y = x - x1 + R y1, R = 1 - 2 pi 5 Hz / 44.1 kHz, Q30 */
#define DC_R_Q30 1072976913
static int64_t dcX1[2], dcY1[2];
static struct pocket_lxr_stats stats;                /* written by audio (and drops by control) */

static int push(const struct event* e)
{
	const uint32_t head = ringHead;
	if (head - ringTail >= POCKET_LXR_QUEUE)
	{
		stats.events_dropped++;
		return -1;
	}
	ring[head & (POCKET_LXR_QUEUE - 1)] = *e;
	POCKET_BARRIER();
	ringHead = head + 1;
	return 0;
}

static uint32_t room(void)
{
	return POCKET_LXR_QUEUE - (ringHead - ringTail);
}

/* ------------------------------------------------------------------ firmware dispatch */
/*
 * As the firmware's front panel parser and kit loader (and CLATTER) do: parameters below 128
 * arrive as MIDI_CC p+1 (the firmware's offset), the rest as CC2 p-128.
 *
 * Except PAR_BANK_CHANGE (127): p+1 wraps to CC 0, which the handler treats as a no-op bank
 * select, but only after storing originalCcValues[(uint16_t)(0 - 1) + 1], 64 KB past the end
 * of a 255-byte array. CLATTER dispatches it (lxr/bridge/clatter_lxr.c dispatchParam, from
 * clatter_load_init_kit); WebAssembly's flat memory hides the stray write there, native code
 * faults. The value is kept in the kit but never sent.
 */
IN_ITCM_RAM static void dispatchParam(uint16_t p)
{
	if (p == PAR_BANK_CHANGE) return;
	MidiMsg msg;
	msg.data2 = activeParameters[p];
	if (p < 128)
	{
		msg.status = MIDI_CC;
		msg.data1 = (uint8_t)((p + 1) & 0x7f);
	}
	else
	{
		msg.status = MIDI_CC2;
		msg.data1 = (uint8_t)((p - 128) & 0x7f);
	}
	midiParser_ccHandler(msg, 1, 0, 0.f);
}

/* firmware automation numbering: parameters below 128 carry the +1 offset of the CC handler */
IN_ITCM_RAM static uint16_t autoDest(int p)
{
	if (p <= 0 || p >= END_OF_SOUND_PARAMETERS) return NO_AUTOMATION;
	return (uint16_t)(p < 127 ? p + 1 : p);
}

/* MidiVoiceControl.c voiceControl_noteOn's mapping of triggers to voices. */
IN_ITCM_RAM static void triggerVoice(int trig, uint8_t vel, uint8_t note)
{
	if (trig < 3)
		Drum_trigger((uint8_t)trig, vel, note);
	else if (trig < 4)
		Snare_trigger(vel, note);
	else if (trig < 5)
		Cymbal_trigger(vel, note);
	else
		HiHat_trigger(vel, (uint8_t)(trig - 5), note);
}

/* A hit's locks: what the variation moves, from the kit's own values, for this hit. Whatever the
 * last hit on the voice locked goes back first, as a sequencer step without automation does
 * (CLATTER's clatter_trigger), so a variation cannot outlive the next hit. */
IN_ITCM_RAM static AutomationNode* lockVoice(int trig, int variation)
{
	const struct variation_lock* row = variation >= POCKET_VARIATION_A && variation <= POCKET_VARIATION_LAST(trig) ? variations[variationRow[trig] + variation - 1] : 0;
	AutomationNode* nodes = lockNodes[trig > POCKET_TRIG_CLOSED_HAT ? POCKET_TRIG_CLOSED_HAT : trig];
	for (int i = 0; i < VARIATION_LOCKS; i++)
	{
		const int p = row ? row[i].param : 0;
		autoNode_setDestination(&nodes[i], autoDest(p));
		if (!p) continue;
		const int v = activeParameters[p] + row[i].by, top = p >= POCKET_PAR_VELOA1 && p <= POCKET_PAR_VELOD6_OPEN ? VARIATION_LONGEST : 127;
		autoNode_updateValue(&nodes[i], (uint8_t)(v < 0 ? 0 : v > top ? (activeParameters[p] > top ? activeParameters[p] : top) : v));
	}
	return nodes;
}

IN_ITCM_RAM static void apply(const struct event* e)
{
	switch (e->type)
	{
	case EV_PARAM:
	{
		const uint16_t p = (uint16_t)(e->a | (e->b << 8));
		activeParameters[p] = e->c;
		dispatchParam(p);
		break;
	}
	case EV_TRIGGER:
	{
		AutomationNode* nodes = lockVoice(e->a, e->variation);
#ifndef POCKET_FM1 /* the comparison with CLATTER's engine: the firmware plays variations only */
		const int p1 = e->p1lo | (e->p1hi << 8), p2 = e->p2lo | (e->p2hi << 8);
		if (p1)
		{
			autoNode_setDestination(&nodes[0], autoDest(p1));
			autoNode_updateValue(&nodes[0], e->v1);
		}
		if (p2)
		{
			autoNode_setDestination(&nodes[1], autoDest(p2));
			autoNode_updateValue(&nodes[1], e->v2);
		}
#else
		(void)nodes;
#endif
		triggerVoice(e->a, e->b, e->c);
		break;
	}
	case EV_MASTER:
		masterGain = e->value;
		break;
	case EV_DC_BLOCK:
		dcBlock = (uint8_t)e->value;
		break;
	case EV_BPM:
		if (engineBpm != (uint16_t)e->value)
		{
			engineBpm = (uint16_t)e->value;
			lfo_recalcSync();
		}
		break;
	}
}

IN_ITCM_RAM void pocket_lxr_trigger_now(int trig, int velocity, int note, int variation)
{
	lockVoice(trig, variation);
	triggerVoice(trig, (uint8_t)velocity, (uint8_t)note);
}

IN_ITCM_RAM static void drain(void)
{
	const uint32_t head = ringHead;
	POCKET_BARRIER();
	uint32_t tail = ringTail;
	const uint32_t waiting = head - tail;
	if (waiting > stats.backlog_max) stats.backlog_max = waiting;
	unsigned n = 0;
	while (tail != head && n < POCKET_LXR_DRAIN_MAX)
	{
		apply(&ring[tail & (POCKET_LXR_QUEUE - 1)]);
		tail++;
		n++;
	}
	POCKET_BARRIER();
	ringTail = tail;
	stats.events_applied += n;
}

/* ------------------------------------------------------------------ control context */
static int clamp(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

POCKET_CONTROL_CODE int pocket_lxr_set_param(int p, int value)
{
	if (p <= 0 || p >= END_OF_SOUND_PARAMETERS) return -1;
	value = clamp(value, 0, 127);
	struct event e = { 0 };
	e.type = EV_PARAM;
	e.a = (uint8_t)(p & 0xff);
	e.b = (uint8_t)(p >> 8);
	e.c = (uint8_t)value;
	if (push(&e)) return -1;
	paramShadow[p] = (uint8_t)value;
	return 0;
}

POCKET_CONTROL_CODE int pocket_lxr_get_param(int p)
{
	if (p < 0 || p >= END_OF_SOUND_PARAMETERS) return 0;
	return paramShadow[p];
}

POCKET_CONTROL_CODE int pocket_lxr_trigger_locked(int trig, int velocity, int note, int p1, int v1, int p2, int v2)
{
	if (trig < 0 || trig >= POCKET_LXR_TRIGGERS) return -1;
	if (p1 <= 0 || p1 >= END_OF_SOUND_PARAMETERS) p1 = 0;
	if (p2 <= 0 || p2 >= END_OF_SOUND_PARAMETERS) p2 = 0;
	struct event e = { 0 };
	e.type = EV_TRIGGER;
	e.a = (uint8_t)trig;
	e.b = (uint8_t)clamp(velocity, 1, 127);
	e.c = (uint8_t)clamp(note, 0, 127);
	e.p1lo = (uint8_t)(p1 & 0xff);
	e.p1hi = (uint8_t)(p1 >> 8);
	e.v1 = (uint8_t)clamp(v1, 0, 127);
	e.p2lo = (uint8_t)(p2 & 0xff);
	e.p2hi = (uint8_t)(p2 >> 8);
	e.v2 = (uint8_t)clamp(v2, 0, 127);
	return push(&e);
}

POCKET_CONTROL_CODE int pocket_lxr_trigger(int trig, int velocity, int note)
{
	return pocket_lxr_trigger_locked(trig, velocity, note, 0, 0, 0, 0);
}

POCKET_CONTROL_CODE int pocket_lxr_trigger_variation(int trig, int velocity, int note, int variation)
{
	if (trig < 0 || trig >= POCKET_LXR_TRIGGERS) return -1;
	struct event e = { 0 };
	e.type = EV_TRIGGER;
	e.a = (uint8_t)trig;
	e.b = (uint8_t)clamp(velocity, 1, 127);
	e.c = (uint8_t)clamp(note, 0, 127);
	e.variation = (uint8_t)variation;
	return push(&e);
}

POCKET_CONTROL_CODE void pocket_lxr_set_block_hook(pocket_lxr_hook hook)
{
	blockHook = hook;
}

POCKET_CONTROL_CODE int pocket_lxr_set_master(int gain_q12)
{
	struct event e = { 0 };
	e.type = EV_MASTER;
	e.value = clamp(gain_q12, 0, 4 * POCKET_GAIN_UNITY);
	return push(&e);
}

POCKET_CONTROL_CODE int pocket_lxr_set_bpm(int bpm)
{
	struct event e = { 0 };
	e.type = EV_BPM;
	e.value = clamp(bpm, 1, 999);
	return push(&e);
}

POCKET_CONTROL_CODE int pocket_lxr_set_dc_block(int on)
{
	struct event e = { 0 };
	e.type = EV_DC_BLOCK;
	e.value = on ? 1 : 0;
	return push(&e);
}

POCKET_CONTROL_CODE int pocket_lxr_load_init_kit(void)
{
	/* initKit holds the kit header (8 bytes) followed by every sound parameter, as
	   preset_init() loads it */
	if (room() < END_OF_SOUND_PARAMETERS) return -1;
	for (int p = 1; p < END_OF_SOUND_PARAMETERS && p + 8 < INIT_KIT_SIZE; p++)
		pocket_lxr_set_param(p, initKit[p + 8]);
	return 0;
}

POCKET_CONTROL_CODE uint32_t pocket_crc32(uint32_t crc, const void* data, unsigned bytes)
{
	const uint8_t* p = data;
	crc = ~crc;
	while (bytes--)
	{
		crc ^= *p++;
		for (unsigned i = 0; i < 8; i++) crc = crc >> 1 ^ (0xedb88320u & -(crc & 1u));
	}
	return ~crc;
}

POCKET_CONTROL_CODE int pocket_lxr_init_kit_value(int p)
{
	return p >= 0 && p + 8 < INIT_KIT_SIZE ? initKit[p + 8] : 0;
}

POCKET_CONTROL_CODE void pocket_lxr_stats_read(struct pocket_lxr_stats* out)
{
	*out = stats;
	stats.peak_q23 = 0;
	stats.backlog_max = 0;
}

/* ------------------------------------------------------------------ lifetime */
/* The engine state the firmware keeps in zero-initialised DTCM (config.h INDTCMZ). */
extern float mixer_decimation_rate[7];
extern float mixer_decimation_cnt[6];
extern int16_t mixer_voice_samples[6];
void pocket_fx_reset(void); /* fx_on.c or fx_off.c */

static void zero(void* p, unsigned n)
{
	volatile uint8_t* b = (volatile uint8_t*)p;
	while (n--) *b++ = 0;
}

/*
 * The STM32 startup code zeroes .dtcmz before soundengine_init runs, and soundengine_init
 * relies on it (oscillator phases, filter and envelope state, decimator counters). Do the
 * same, so a re-initialised engine renders exactly as a freshly booted one.
 */
static void zeroEngineState(void)
{
	zero(voiceArray, sizeof voiceArray);
	zero(&snareVoice, sizeof snareVoice);
	zero(&cymbalVoice, sizeof cymbalVoice);
	zero(&hatVoice, sizeof hatVoice);
	zero(velocityModulators, sizeof velocityModulators);
	zero(mixer_audioRouting, sizeof(uint8_t) * 6);
	zero(mixer_decimation_rate, sizeof(float) * 7);
	zero(mixer_decimation_cnt, sizeof(float) * 6);
	zero(mixer_voice_samples, sizeof(int16_t) * 6);
	zero(&fxBus, sizeof fxBus);
	pocket_fx_reset();
}

/*
 * soundengine_init() (Soundengine.c) without the codec, the sequencer and the Euclidean
 * generator, as CLATTER's clatter_init. The init kit is applied directly here (the audio
 * context is not running yet), every parameter through the firmware's handler.
 */
POCKET_CONTROL_CODE int pocket_lxr_init(uint32_t seed)
{
#ifdef POCKET_PACKED_TABLES
	/* the firmware's constant tables, unpacked into RAM before anything reads them */
	static uint8_t tablesReady, tablesBad;
	if (!tablesReady)
	{
		tablesBad = pocket_lxr_tables_unpack() != 0;
		tablesReady = 1;
	}
#endif
	zeroEngineState();
	hrng.state = seed ? seed : 0x9e3779b9u;
	engineBpm = 120;
	ringHead = ringTail = 0;
	masterGain = POCKET_GAIN_UNITY;
	dcBlock = 0;
	blockHook = 0;
	dcX1[0] = dcX1[1] = dcY1[0] = dcY1[1] = 0;
	for (unsigned i = 0; i < sizeof stats; i++) ((uint8_t*)&stats)[i] = 0;
#ifdef POCKET_PACKED_TABLES
	if (tablesBad) stats.events_dropped = ~0u; /* corrupt tables: shown as DROP on the panel */
#endif
	for (int i = 0; i < NUM_PARAMS; i++) activeParameters[i] = 0;
	for (int i = 0; i < END_OF_SOUND_PARAMETERS; i++) paramShadow[i] = 0;

	mixer_init();
	parameterArray_init();
	for (int i = 0; i < POCKET_LXR_VOICES; i++) modNode_init(&velocityModulators[i]);
	for (int i = 0; i < 8; i++) midi_MidiChannels[i] = 0;
	initDrumVoice();
	Snare_init();
	HiHat_init();
	Cymbal_init();
	for (int v = 0; v < POCKET_LXR_VOICES; v++)
		for (int i = 0; i < VARIATION_LOCKS; i++) autoNode_init(&lockNodes[v][i]);

	for (int p = 0; p < END_OF_SOUND_PARAMETERS && p + 8 < INIT_KIT_SIZE; p++)
	{
		activeParameters[p] = initKit[p + 8];
		paramShadow[p] = initKit[p + 8];
	}
	for (int p = 1; p < END_OF_SOUND_PARAMETERS; p++) dispatchParam((uint16_t)p);
	/* no LFO or velocity modulation targets until a kit sets them (CLATTER's default) */
	modNode_setDestination(&voiceArray[0].lfo.modTarget, 0);
	modNode_setDestination(&voiceArray[1].lfo.modTarget, 0);
	modNode_setDestination(&voiceArray[2].lfo.modTarget, 0);
	modNode_setDestination(&snareVoice.lfo.modTarget, 0);
	modNode_setDestination(&cymbalVoice.lfo.modTarget, 0);
	modNode_setDestination(&hatVoice.lfo.modTarget, 0);
	for (int v = 0; v < POCKET_LXR_VOICES; v++) modNode_setDestination(&velocityModulators[v], 0);
#ifdef POCKET_PACKED_TABLES
	return tablesBad ? -1 : 0;
#else
	return 0;
#endif
}

/* ------------------------------------------------------------------ audio context */
static int16_t voiceData[POCKET_LXR_VOICES][POCKET_LXR_BLOCK];
static float voicePanL[POCKET_LXR_VOICES], voicePanR[POCKET_LXR_VOICES];
static int32_t mixL[POCKET_LXR_BLOCK], mixR[POCKET_LXR_BLOCK];

const int16_t* pocket_lxr_voice_block(int v, float* pan_l, float* pan_r)
{
	if (v < 0 || v >= POCKET_LXR_VOICES) return 0;
	if (pan_l) *pan_l = voicePanL[v];
	if (pan_r) *pan_r = voicePanR[v];
	return voiceData[v];
}

/* mixer_addDataToOutput's pan law and int16 conversion, summed in 32 bits */
IN_ITCM_RAM static void addVoice(int v, float panL, float panR)
{
	const int16_t* data = voiceData[v];
	voicePanL[v] = panL;
	voicePanR[v] = panR;
	for (int i = 0; i < POCKET_LXR_BLOCK; i++)
	{
		mixL[i] += (int16_t)(data[i] * panL);
		mixR[i] += (int16_t)(data[i] * panR);
	}
}

/*
 * One engine block: mixer.c's mixer_calcNextSampleBlock order of operations (modulation
 * reset, LFOs, filter coefficients, the voices' asynchronous parts, then each voice's samples
 * and its decimator), with each voice kept for inspection and mixed with its triggered pan.
 */
IN_ITCM_RAM static void renderBlock(void)
{
	modNode_resetTargets();
	modNode_reassignVeloMod();

	lfo_dispatchNextValue(&voiceArray[0].lfo);
	lfo_dispatchNextValue(&voiceArray[1].lfo);
	lfo_dispatchNextValue(&voiceArray[2].lfo);
	lfo_dispatchNextValue(&snareVoice.lfo);
	lfo_dispatchNextValue(&cymbalVoice.lfo);
	lfo_dispatchNextValue(&hatVoice.lfo);

	SVF_recalcFreq(&voiceArray[0].filter);
	SVF_recalcFreq(&voiceArray[1].filter);
	SVF_recalcFreq(&voiceArray[2].filter);
	SVF_recalcFreq(&snareVoice.filter);
	SVF_recalcFreq(&cymbalVoice.filter);
	SVF_recalcFreq(&hatVoice.filter);

	calcDrumVoiceAsync(0);
	calcDrumVoiceAsync(1);
	calcDrumVoiceAsync(2);
	Snare_calcAsync();
	Cymbal_calcAsync();
	HiHat_calcAsync();

	for (int i = 0; i < POCKET_LXR_BLOCK; i++) mixL[i] = mixR[i] = 0;
	for (int v = 0; v < 3; v++)
	{
		calcDrumVoiceSyncBlock((uint8_t)v, voiceData[v], POCKET_LXR_BLOCK);
		mixer_decimateBlock((uint8_t)v, voiceData[v]);
		addVoice(v, voiceArray[v].triggeredPanL, voiceArray[v].triggeredPanR);
	}
	Snare_calcSyncBlock(voiceData[3], POCKET_LXR_BLOCK);
	mixer_decimateBlock(3, voiceData[3]);
	addVoice(3, snareVoice.triggeredPanL, snareVoice.triggeredPanR);

	Cymbal_calcSyncBlock(voiceData[4], POCKET_LXR_BLOCK);
	mixer_decimateBlock(4, voiceData[4]);
	addVoice(4, cymbalVoice.triggeredPanL, cymbalVoice.triggeredPanR);

	HiHat_calcSyncBlock(voiceData[5], POCKET_LXR_BLOCK);
	mixer_decimateBlock(5, voiceData[5]);
	addVoice(5, hatVoice.triggeredPanL, hatVoice.triggeredPanR);
}

/* One-pole DC blocker on channel c (Q23 in, Q23 out, rounded). */
IN_ITCM_RAM static int64_t blockDc(int c, int64_t x)
{
	const int64_t y = x - dcX1[c] + ((dcY1[c] * DC_R_Q30 + (1 << 29)) >> 30);
	dcX1[c] = x;
	dcY1[c] = y;
	return y;
}

IN_ITCM_RAM static int32_t saturateQ23(int64_t v)
{
	if (v > POCKET_Q23_MAX)
	{
		stats.clipped_samples++;
		return POCKET_Q23_MAX;
	}
	if (v < POCKET_Q23_MIN)
	{
		stats.clipped_samples++;
		return POCKET_Q23_MIN;
	}
	return (int32_t)v;
}

IN_ITCM_RAM void pocket_lxr_render_q23(int32_t* out, unsigned frames, int accumulate)
{
	for (unsigned f = 0; f + POCKET_LXR_BLOCK <= frames; f += POCKET_LXR_BLOCK)
	{
		drain();
		if (blockHook) blockHook();
		renderBlock();
		/* int16 full scale is Q23 full scale shifted by 8; master gain is Q12 */
		const int64_t gain = masterGain;
		uint32_t peak = stats.peak_q23;
		int32_t* o = out + 2 * f;
		for (int i = 0; i < POCKET_LXR_BLOCK; i++)
		{
			int64_t l = (((int64_t)mixL[i] << 8) * gain) >> 12;
			int64_t r = (((int64_t)mixR[i] << 8) * gain) >> 12;
			if (dcBlock)
			{
				l = blockDc(0, l);
				r = blockDc(1, r);
			}
			if (accumulate)
			{
				l += o[2 * i];
				r += o[2 * i + 1];
			}
			const int32_t sl = saturateQ23(l), sr = saturateQ23(r);
			o[2 * i] = sl;
			o[2 * i + 1] = sr;
			const uint32_t al = (uint32_t)(sl < 0 ? -(int64_t)sl : sl);
			const uint32_t ar = (uint32_t)(sr < 0 ? -(int64_t)sr : sr);
			if (al > peak) peak = al;
			if (ar > peak) peak = ar;
		}
		stats.peak_q23 = peak;
		stats.blocks++;
	}
}
