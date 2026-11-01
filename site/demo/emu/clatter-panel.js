// CLATTER Pocket's change to fm1-emu: the FM-1 drawn as the site draws it.
//
// The emulator's canvas stays above this drawing, masked to its LCD (clatter.css), so it still
// shows the screen and takes every press and drag. The drawing shows the rest: which keys and
// buttons are held or lit, and where the knobs point. The panel map is the emulator's, so
// panel_hit names each control from where it is drawn.
//
// Copyright (C) 2026 The CLATTER contributors. SPDX-License-Identifier: GPL-3.0-only

import { panelSvg } from '../../assets/controls.mjs';

const DEGREES_PER_CLICK = 15;
const POT_SWEEP = 270, POT_MAX = 1023;
const LIT = 0.02;                            // a lit LED's duty reads about 0.09, an unlit one 0

/** Draw the panel in `host`. Returns the function that shows a frame's state on it. */
export function clatterPanel(ui, host) {
  host.insertAdjacentHTML('afterbegin', panelSvg({ interactive: true, title: 'FM-1 front panel' }));
  const svg = host.querySelector('svg');
  svg.setAttribute('aria-hidden', 'true');   // the canvas above it is the control
  const number = (el, name) => Number(el.getAttribute(name));

  const pressable = new Map();               // matrix id -> its shape; ids are also LED numbers
  for (const el of svg.querySelectorAll('.btn, .key')) {
    const hit = ui.panel_hit(number(el, 'x') + number(el, 'width') / 2, number(el, 'y') + number(el, 'height') / 2);
    pressable.set(hit - 100, { el, lit: false, held: false });
  }
  const knobs = [];                          // by the emulator's number: 0..6 encoders, 7 MASTER
  for (const group of svg.querySelectorAll('.ctl')) {
    const knob = group.querySelector('.knob');
    if (!knob) continue;
    const x = number(knob, 'cx'), y = number(knob, 'cy');
    knobs[ui.panel_hit(x, y)] = { group, x, y, pointer: group.querySelector('.ptr'), angle: 0, turning: false };
  }

  const point = (knob, angle) => {
    if (angle === knob.angle) return;
    knob.angle = angle;
    knob.pointer.setAttribute('transform', `rotate(${angle} ${knob.x} ${knob.y})`);
  };
  return (leds, down, clicks, pot, turning) => {
    for (const [id, control] of pressable) {
      const lit = (leds?.[id] ?? 0) > LIT, held = down[id] !== 0;
      if (lit !== control.lit) control.el.classList.toggle('lit', control.lit = lit);
      if (held !== control.held) control.el.classList.toggle('held', control.held = held);
    }
    knobs.forEach((knob, i) => {
      point(knob, i === 7 ? pot / POT_MAX * POT_SWEEP : clicks[i] * DEGREES_PER_CLICK);
      const moving = turning[i] !== 0;
      if (moving !== knob.turning) knob.group.classList.toggle('turning', knob.turning = moving);
    });
  };
}
