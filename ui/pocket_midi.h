/*
 * pocket_midi.h - MIDI input for CLATTER Pocket: complete short messages in, LXR triggers out.
 * Platform-independent (host/tests/test_midi.c); the FM-1 adapter calls it from the SDK's
 * MIDI routing task and queues the result for the control task.
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef POCKET_MIDI_H
#define POCKET_MIDI_H
#include <stddef.h>
#include <stdint.h>

/* The LXR trigger (POCKET_TRIG_*, 0..6) for a General MIDI drum note, or -1 if unmapped:
 * 35/36 DRUM 1; 41/43/45 DRUM 2; 47/48/50 DRUM 3; 37/38/39/40 SNARE;
 * 49/51/52/55/57/59 CYMBAL; 42/44 CLOSED HAT; 46 OPEN HAT. */
int pocket_midi_drum_trigger(unsigned note);

/* Parses one complete message. Returns 1 and sets *trigger and *velocity (1..127) for a
 * note-on (any channel, velocity > 0) of a mapped drum note; 0 for anything else, including
 * note-offs, note-on with velocity 0, other messages, SysEx, short or malformed input (a
 * status byte without bit 7, a data byte with bit 7). Never reads beyond `bytes`. */
int pocket_midi_parse(const uint8_t* message, size_t bytes, int* trigger, int* velocity);

#endif
