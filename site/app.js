// The home page: the control guide and the hero's screen.
// Copyright (C) 2026 The CLATTER contributors. SPDX-License-Identifier: GPL-3.0-only

import { CONTROLS } from './assets/controls.mjs';

const still = matchMedia('(prefers-reduced-motion: reduce)');

// ---------------------------------------------------------------- the hero's screen
// Real screens read from the emulated LCD, changing as they would while someone plays.
const SCREENS = ['seq-2-playing', 'page-sound', 'perf-1-kit', 'page-mixer', 'page-saved-kits'];
const heroScreen = document.querySelector('#hero-fm1 image');
if (heroScreen) {
  for (const name of SCREENS) new Image().src = `assets/screens/${name}.png`;
  let at = 0, timer = 0;
  const run = () => {
    clearInterval(timer);
    if (still.matches) return;
    timer = setInterval(() => {
      if (document.hidden) return;
      at = (at + 1) % SCREENS.length;
      heroScreen.setAttribute('href', `assets/screens/${SCREENS[at]}.png`);
    }, 3200);
  };
  still.addEventListener('change', run);
  run();
}

// ---------------------------------------------------------------- the control guide
// The list is the way in for keyboards, touch and screen readers: real buttons, big enough to
// tap at any width. The diagram follows it, and answers to a mouse or a finger as well.
const list = document.getElementById('guide-list');
const readout = document.getElementById('readout');
const diagram = document.getElementById('guide-fm1');

if (list && readout) {
  const CHORDS = CONTROLS.filter(c => c.save);
  let selected = 'env', held = false;

  const el = (tag, className, text) => {
    const e = document.createElement(tag);
    if (className) e.className = className;
    if (text !== undefined) e.textContent = text;
    return e;
  };
  const chip = (control, chord) => {
    const b = el('button', 'chip', chord ? control.chord : control.label);
    b.type = 'button';
    b.dataset.ctl = control.id;
    if (chord) b.dataset.chord = '1';
    b.addEventListener('click', () => { held ||= !!chord; select(control.id); });
    return b;
  };

  const hold = el('button', 'chip hold', 'HOLD SAVE');
  hold.type = 'button';
  hold.setAttribute('aria-pressed', 'false');
  hold.addEventListener('click', () => { held = !held; show(); });
  const holdRow = el('div', 'chips');
  holdRow.append(hold);
  list.append(holdRow);

  for (const group of [...new Set(CONTROLS.map(c => c.group))]) {
    const box = el('div', 'chip-group');
    box.append(el('h3', '', group));
    const chips = el('div', 'chips');
    for (const c of CONTROLS) if (c.group === group) chips.append(chip(c));
    box.append(chips);
    list.append(box);
  }
  const chords = el('div', 'chip-group');
  chords.append(el('h3', '', 'With SAVE held'));
  const chordChips = el('div', 'chips');
  for (const c of CHORDS) chordChips.append(chip(c, true));
  chords.append(chordChips);
  list.append(chords);

  const row = (head, text) => {
    const p = el('p');
    p.append(el('b', '', head), ` ${text}`);
    return p;
  };

  function show() {
    const c = CONTROLS.find(x => x.id === selected);
    const chord = held && c.save;
    readout.replaceChildren();
    readout.append(el('p', 'readout-name', chord ? c.chord : c.label), el('p', 'readout-kind', c.kind));
    readout.append(el('p', 'readout-does', chord ? c.save : c.does));
    if (held && !c.save && c.id !== 'save') readout.append(row('SAVE:', 'no second action.'));
    if (chord) readout.append(row('Alone:', c.does));
    else if (c.save) readout.append(row(`${c.chord}:`, c.save));
    if (c.savePage) readout.append(row('Save pages:', c.savePage.replace(/^On a saved-kits or saved-patterns page[:,]? ?/, '')));
    if (c.boot) readout.append(row('At start-up:', c.boot));
    if (c.light) readout.append(row('Light:', c.light));
    readout.append(row('In the demo:', chord ? `8 + ${c.key}` : c.key));

    hold.setAttribute('aria-pressed', String(held));
    hold.textContent = held ? 'SAVE HELD' : 'HOLD SAVE';
    for (const b of list.querySelectorAll('.chip[data-ctl]')) {
      const on = b.dataset.ctl === selected && !!b.dataset.chord === !!chord;
      b.classList.toggle('on', on);
      b.setAttribute('aria-pressed', String(on));
    }
    for (const g of diagram.querySelectorAll('.ctl')) {
      const id = g.dataset.ctl, has = CONTROLS.find(x => x.id === id);
      g.classList.toggle('on', id === selected);
      g.classList.toggle('chord', held && !!has.save);
      g.classList.toggle('modifier', held && id === 'save');
    }
  }
  const select = id => { selected = id; show(); };

  diagram.addEventListener('pointerover', e => {
    const g = e.target.closest('.ctl');
    if (g && e.pointerType === 'mouse') select(g.dataset.ctl);
  });
  diagram.addEventListener('click', e => {
    const g = e.target.closest('.ctl');
    if (g) select(g.dataset.ctl);
  });

  list.hidden = false;
  readout.hidden = false;
  diagram.classList.add('live');
  show();
}
