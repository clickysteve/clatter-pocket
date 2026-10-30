/*
 * pocket_lxr.h - CLATTER Pocket's real-time host for the LXR-02 synthesis engine.
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Platform-independent: the same code runs in the host harness (host/) and in the FM-1
 * firmware (firmware/fm1/). Nothing here knows about the FM-1, WebAssembly or an OS.
 *
 * Threading model
 * ---------------
 * Two contexts, one producer and one consumer:
 *
 *   control context  (FM-1 control task, host main thread)
 *       pocket_lxr_set_param, pocket_lxr_trigger, pocket_lxr_set_master, ...
 *       These never touch synthesis state. They push a fixed-size event onto a
 *       single-producer/single-consumer ring and return immediately.
 *
 *   audio context    (FM-1 I2S/DMA interrupt, host render loop)
 *       pocket_lxr_render_q23
 *       Drains a bounded number of events at each 64-sample engine block, then renders.
 *       Only the audio context ever reads or writes LXR state, so parameter changes cannot
 *       tear a voice mid-block. No allocation, no locks, no I/O.
 *
 * pocket_lxr_init must run before the audio context starts (or while it is excluded).
 */
#ifndef POCKET_LXR_H
#define POCKET_LXR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Firmware geometry (Soundengine/config.h, Parameters.h). Checked at compile time in
 * pocket_lxr.c. */
#define POCKET_LXR_BLOCK 64      /* OUTPUT_DMA_SIZE: samples per engine block */
#define POCKET_LXR_VOICES 6      /* drum 1-3, snare, cymbal, hihat */
#define POCKET_LXR_TRIGGERS 7    /* drum 1-3, snare, cymbal, closed hat, open hat */
#define POCKET_LXR_QUEUE 256     /* events; power of two */
#define POCKET_LXR_DRAIN_MAX 32  /* events applied per engine block (bounds ISR time) */

/* Triggers (CLATTER's numbering, src/sound/lxrParams.ts TRIGGERS). */
enum {
	POCKET_TRIG_DRUM1 = 0,
	POCKET_TRIG_DRUM2,
	POCKET_TRIG_DRUM3,
	POCKET_TRIG_SNARE,
	POCKET_TRIG_CYMBAL,
	POCKET_TRIG_CLOSED_HAT,
	POCKET_TRIG_OPEN_HAT
};

/* Q23 full scale (signed 24-bit PCM in an int32, as the FM-1 SDK's audio blocks). */
#define POCKET_Q23_MAX 8388607
#define POCKET_Q23_MIN (-8388608)

/* Master gain is Q12: 4096 = unity. */
#define POCKET_GAIN_UNITY 4096

struct pocket_lxr_stats {
	uint32_t blocks;          /* engine blocks rendered (64 samples each) */
	uint32_t events_applied;  /* events drained by the audio context */
	uint32_t events_dropped;  /* events refused because the ring was full */
	uint32_t clipped_samples; /* output samples saturated to Q23 */
	uint32_t peak_q23;        /* largest |sample| since the last pocket_lxr_stats_read */
	uint32_t backlog_max;     /* most events waiting at a block start */
};

/* ---------------------------------------------------------------- lifetime (control) */
/* Initialise the engine exactly as soundengine_init() does (without codec or sequencer),
 * reset the event ring and load the firmware's init kit. `seed` seeds the noise source.
 * Returns 0, or -1 if the packed tables (POCKET_PACKED_TABLES builds) failed to unpack: the
 * engine must then not be rendered. */
int pocket_lxr_init(uint32_t seed);

/* ---------------------------------------------------------------- control context */
/* Sound parameter `p` (a Parameters.h PAR_* number, 1..END_OF_SOUND_PARAMETERS-1) to `value`
 * (0-127, the front panel value). Returns 0, or -1 if the event ring is full. */
int pocket_lxr_set_param(int p, int value);
/* The last value queued for p (the control context's view of the kit). */
int pocket_lxr_get_param(int p);
/* Trigger (POCKET_TRIG_*), velocity 1-127, MIDI note (60 = the programmed pitch). */
int pocket_lxr_trigger(int trig, int velocity, int note);
/* Trigger with up to two one-hit parameter locks (p = PAR_* or 0, v = 0-127), applied with
 * the firmware's AutomationNode as the LXR sequencer applies its step automation. */
int pocket_lxr_trigger_locked(int trig, int velocity, int note, int p1, int v1, int p2, int v2);
/* Master gain, Q12 (4096 = unity, 0-16384). */
int pocket_lxr_set_master(int gain_q12);
/* Output DC blocker (one-pole high-pass at about 5 Hz) on or off; off after init.
 * The LXR's decimator is a sample-and-hold: at very low rates (VOICE_DECIMATION near 0) it
 * holds a voice's last sample for seconds or for ever, a DC level that the LXR-02's
 * AC-coupled outputs remove. The FM-1 build turns this on; comparisons with CLATTER leave
 * it off. */
int pocket_lxr_set_dc_block(int on);
/* Tempo for tempo-synced LFOs (1-999 BPM). */
int pocket_lxr_set_bpm(int bpm);
/* Reload the firmware's init kit. Queued as individual parameter events, so it is spread
 * over several blocks rather than stalling one. Returns -1 if the ring lacks room. */
int pocket_lxr_load_init_kit(void);
/* Parameter p of the firmware's init kit. */
int pocket_lxr_init_kit_value(int p);
/* Copy and reset the statistics (peak and backlog reset; counters keep counting). */
void pocket_lxr_stats_read(struct pocket_lxr_stats* out);

/* Called in the audio context at the start of every engine block, after queued events and
 * before the block renders: what it triggers sounds from that block. Set only before the audio
 * context starts (pocket_lxr_init clears it). */
typedef void (*pocket_lxr_hook)(void);
void pocket_lxr_set_block_hook(pocket_lxr_hook hook);

/* ---------------------------------------------------------------- audio context */
/* Trigger at once: for the block hook, which must not use the ring (the control context is its
 * only producer). The same path as pocket_lxr_trigger. `trig`, `velocity` (1-127) and `note`
 * (0-127, 60 = the programmed pitch) are not checked. */
void pocket_lxr_trigger_now(int trig, int velocity, int note);
/* Render `frames` stereo frames (a multiple of POCKET_LXR_BLOCK) of the six voices, each
 * panned as the firmware pans it at trigger time, as signed Q23 interleaved L/R. With
 * `accumulate`, adds to `out` (saturating) instead of overwriting it. */
void pocket_lxr_render_q23(int32_t* out, unsigned frames, int accumulate);

/* ---------------------------------------------------------------- inspection (tests) */
/* The raw int16 output of voice v (0-5) in the last rendered engine block, and the pan
 * gains it was mixed with. For host tests and comparisons with CLATTER only. */
const int16_t* pocket_lxr_voice_block(int v, float* pan_l, float* pan_r);

#ifdef __cplusplus
}
#endif

#endif
