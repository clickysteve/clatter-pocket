/*
 * clatter_pocket_fm1.h - CLATTER Pocket inside the FM-1 Open firmware (fm1_open).
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Two hooks, added to fm1_open by firmware/fm1/fm1_open.patch:
 *   app/board/audio.c  fm1_audio_irq   -> clatter_pocket_fm1_render (after the stock engine)
 *   app/screen.c       groove_screen_control -> clatter_pocket_fm1_control (after reading
 *                                             the panel; Pocket mode takes the panel over)
 * The stock instrument keeps running underneath; the LXR voices are added to its output
 * before master gain, USB and Bluetooth mixing (fm1_output_mix).
 */
#ifndef CLATTER_POCKET_FM1_H
#define CLATTER_POCKET_FM1_H

#include "fm1.h"

/* Audio ISR, once per FM1_BLOCK_FRAMES block: add the LXR voices to the Q23 block. */
FM1_AUDIO_CODE void clatter_pocket_fm1_render(int32_t* output);

/* Control task, every poll, with the controls just read and the buttons of the previous
 * poll. Returns 0: stock UI continues; 1: Pocket mode owns the panel this poll (the stock
 * UI returns); 2: Pocket mode just ended (the stock UI repaints its page and returns). */
int clatter_pocket_fm1_control(const struct fm1_controls* controls, uint32_t previous_buttons);

#endif
