// The FM-1's controls as CLATTER Pocket uses them, for the control guide.
//
// Every action is from docs/POCKET-CONTROLS.md and docs/POCKET-STORAGE.md. The panel geometry is
// fm1-emu's panel map (crates/fm1-live/src/panel.rs, 1280 x 820), which that project drew after
// Felucca's controls diagram. tools/test-site.mjs compares the labels with the documents.
//
// Copyright (C) 2026 The CLATTER contributors. SPDX-License-Identifier: GPL-3.0-only

export const CONTROLS = [
  { id: 'keys', label: 'KEYS', kind: '27 note keys, F3 to G5, no velocity', group: 'Play', key: 'A W S E D F T G Y H U J K O L P ; \'',
    does: 'Play. In DRUM each key is an instrument, the same at every octave. In PITCH every key plays the selected voice chromatically.' },
  { id: 'scl', label: 'SCL', kind: 'Button', group: 'Play', key: '2', light: 'lit in PITCH',
    does: 'Switch the keyboard: KEYS: DRUM or KEYS: PITCH.' },
  { id: 'octdown', label: 'OCT−', kind: 'Button', group: 'Play', key: 'Z',
    does: 'PITCH: an octave down, to −3. DRUM: the keys hit softer: 40, 70, 100 or 127.',
    save: 'Undo the last change to the pattern. Again, redo.', chord: 'SAVE + OCT−', savePage: 'On a saved-kits or saved-patterns page: load the selected slot.' },
  { id: 'octup', label: 'OCT+', kind: 'Button', group: 'Play', key: 'X',
    does: 'PITCH: an octave up, to +3. DRUM: the keys hit harder.',
    save: 'Undo the last change of kit. Again, redo.', chord: 'SAVE + OCT+' },
  { id: 'play', label: 'PLAY', kind: 'Button', group: 'Play', key: 'Space', light: 'lit while playing',
    does: 'Start or stop the sequencer.' },
  { id: 'rec', label: 'REC', kind: 'Button', group: 'Play', key: 'R', light: 'lit while recording',
    does: 'Recording on or off. Playing, a hit lands on the nearest step. Stopped, it lands on the selected step, and hitting it again removes it.' },

  { id: 'env', label: 'ENV', kind: 'Button', group: 'Generate', key: '3',
    does: 'A new kit, in the character PRESETS chose.',
    save: 'A new sound for the selected voice. The rest of the kit stays.', chord: 'SAVE + ENV' },
  { id: 'arp', label: 'ARP', kind: 'Button', group: 'Generate', key: '9',
    does: 'A new pattern, in the style ALGORITHM chose.',
    save: 'A new pattern for the selected track. The other six stay.', chord: 'SAVE + ARP' },
  { id: 'lfo', label: 'LFO', kind: 'Button', group: 'Generate', key: '4',
    does: 'Mutate the pattern, at the strength set by MUTATE on GLOBAL.',
    save: 'Mutate the selected track only.', chord: 'SAVE + LFO' },
  { id: 'save', label: 'SAVE', kind: 'Button, and the modifier', group: 'Generate', key: '8', light: 'lit on a locked track',
    does: 'Hold it to reach the second action of ENV, ARP, LFO, OCT−, OCT+, EDIT and SEQ. A tap locks or unlocks the selected track, so ARP and LFO leave it alone.',
    savePage: 'On a saved-kits or saved-patterns page, a tap stores to the selected slot. Only when stopped.' },

  { id: 'presets', label: 'PRESETS', kind: 'Encoder', group: 'Choose', key: '↑ ↓',
    does: 'The kit: INIT, then 16 characters. Nothing is stored, so browsing never overwrites anything.' },
  { id: 'algorithm', label: 'ALGORITHM', kind: 'Encoder', group: 'Choose', key: ', .',
    does: 'The pattern style, of 11: MACHINE, ELECTRO, BREAKS, JUNGLE, HALFTIME, BROKEN, GARAGE, INDUSTRIAL, GLITCH, POLYMETER, DUB.' },
  { id: 'select', label: 'SELECT', kind: 'Encoder with no push switch: it cannot be held', group: 'Choose', key: '← →',
    does: 'The selected voice: DRUM 1 to 3, SNARE, CYMBAL, CL HAT, OP HAT. SAVE is the modifier, not SELECT.' },

  { id: 'home', label: 'HOME', kind: 'Button', group: 'Pages', key: '7',
    does: 'MAIN: transport, tempo, swing, style, hits, a row of the pattern, the kit.',
    boot: 'Held 1.5 s after power-on: SAFE MODE. The engine stays off and the updater keeps running.' },
  { id: 'seq', label: 'SEQ', kind: 'Button', group: 'Pages', key: '0',
    does: 'PATTERN: the step grid, velocity, tempo, swing.',
    save: 'Saved patterns: 12 slots.', chord: 'SAVE + SEQ' },
  { id: 'edit', label: 'EDIT', kind: 'Button', group: 'Pages', key: '5',
    does: 'SOUND: pitch, decay, filter and drive of the selected voice.',
    save: 'Saved kits: 12 slots.', chord: 'SAVE + EDIT' },
  { id: 'fx', label: 'FX', kind: 'Button', group: 'Pages', key: '1',
    does: 'MIXER: level and pan of the selected voice.' },
  { id: 'glo', label: 'GLO', kind: 'Button', group: 'Pages', key: '6',
    does: 'GLOBAL settings. Press again for SYSTEM: version, build, CPU, overruns, clipping, heap.' },

  { id: 'knob1', label: 'KNOB 1', kind: 'Encoder', group: 'Knobs', key: 'mouse',
    does: 'PATTERN: the selected step\'s velocity. SOUND: pitch. MIXER: level. GLOBAL: row.',
    savePage: 'On a saved-kits or saved-patterns page: choose a slot.' },
  { id: 'knob2', label: 'KNOB 2', kind: 'Encoder', group: 'Knobs', key: 'mouse',
    does: 'PATTERN: tempo. SOUND: decay. MIXER: pan. GLOBAL: value.' },
  { id: 'knob3', label: 'KNOB 3', kind: 'Encoder', group: 'Knobs', key: 'mouse',
    does: 'PATTERN: swing. SOUND: filter.' },
  { id: 'knob4', label: 'KNOB 4', kind: 'Encoder', group: 'Knobs', key: 'mouse',
    does: 'PATTERN: the selected step. SOUND: drive.' },
  { id: 'master', label: 'MASTER', kind: 'Potentiometer', group: 'Knobs', key: 'mouse',
    does: 'Output level.' },
];

// x, y, r for round controls; x, y, w, h for rectangles. fm1-emu's panel map, unchanged.
export const GEOMETRY = {
  master: { c: [110, 135, 44] },
  select: { c: [250, 135, 44] },
  presets: { c: [110, 295, 44] },
  algorithm: { c: [250, 295, 44] },
  knob1: { c: [935, 120, 36] },
  knob2: { c: [1027, 120, 36] },
  knob3: { c: [1119, 120, 36] },
  knob4: { c: [1211, 120, 36] },
  octdown: { r: [62, 398, 112, 44] },
  octup: { r: [186, 398, 112, 44] },
};
[['fx', 'scl', 'env', 'lfo', 'edit', 'glo'], ['home', 'save', 'arp', 'seq', 'play', 'rec']].forEach((row, y) =>
  row.forEach((id, x) => { GEOMETRY[id] = { r: [906 + x * 58, 232 + y * 76, 50, 64] }; }));

const KW = 62, GAP = 11, X0 = 61;
const WHITES = 16;
const BLACKS = [0, 1, 2, 4, 5, 7, 8, 9, 11, 12, 14];  // the white key each sits after
export const LCD = { x: 370, y: 42, size: 480 };

const esc = s => s.replace(/&/g, '&amp;').replace(/</g, '&lt;');

/** The panel as line art. `lcd` is the image on its screen, `interactive` marks the controls. */
export function panelSvg({ lcd, interactive = false, title }) {
  const parts = [];
  const group = (id, body) => parts.push(interactive
    ? `<g class="ctl" data-ctl="${id}">${body}</g>` : body);
  const label = (x, y, text, anchor = 'middle') =>
    `<text x="${x}" y="${y}" text-anchor="${anchor}">${esc(text)}</text>`;

  parts.push(`<rect class="body" x="8" y="8" width="1264" height="804" rx="30"/>`);
  parts.push(`<rect class="tray" x="44" y="556" width="1192" height="244" rx="14"/>`);

  for (const c of CONTROLS) {
    const g = GEOMETRY[c.id];
    if (!g) continue;
    if (g.c) {
      const [x, y, r] = g.c;
      const pointer = c.id === 'master' ? [x - r * .55, y + r * .55] : [x, y - r * .7];
      group(c.id, `<circle class="hit" cx="${x}" cy="${y}" r="${r + 10}"/><circle class="knob" cx="${x}" cy="${y}" r="${r}"/>` +
        `<circle class="cap" cx="${x}" cy="${y}" r="${r - 12}"/><line class="ptr" x1="${x}" y1="${y}" x2="${pointer[0]}" y2="${pointer[1]}"/>` +
        label(x, y + r + 26, c.id === 'algorithm' ? 'ALGO' : c.label));
    } else {
      const [x, y, w, h] = g.r;
      group(c.id, `<rect class="btn" x="${x}" y="${y}" width="${w}" height="${h}" rx="6"/>` +
        label(x + w / 2, y + h / 2 + 5, c.label));
    }
  }

  let keys = '';
  for (let i = 0; i < WHITES; i++) keys += `<rect class="key" x="${X0 + i * (KW + GAP)}" y="680" width="${KW}" height="106" rx="26"/>`;
  for (const after of BLACKS) {
    const cx = X0 + (after + 1) * (KW + GAP) - GAP / 2;
    keys += `<rect class="key blk" x="${cx - KW / 2}" y="574" width="${KW}" height="94" rx="26"/>`;
  }
  group('keys', keys);

  parts.push(`<rect class="lcd" x="${LCD.x - 8}" y="${LCD.y - 8}" width="${LCD.size + 16}" height="${LCD.size + 16}" rx="6"/>`);
  if (lcd) parts.push(`<image class="screen" href="${lcd}" x="${LCD.x}" y="${LCD.y}" width="${LCD.size}" height="${LCD.size}"/>`);

  const name = title ?? 'Line drawing of the M-VAVE FM-1 front panel';
  return `<svg class="fm1" viewBox="0 0 1280 820" role="img" aria-label="${esc(name)}">${parts.join('')}</svg>`;
}

/** The control map as a table, for reading without the diagram. */
export function controlTable() {
  const rows = CONTROLS.map(c => `<tr><th scope="row">${esc(c.label)}</th><td>${esc(c.does)}</td>` +
    `<td>${esc([c.save, c.savePage].filter(Boolean).join(' '))}</td><td><kbd>${esc(c.key)}</kbd></td></tr>`).join('\n');
  return `<table><thead><tr><th scope="col">Control</th><th scope="col">Alone</th>` +
    `<th scope="col">With SAVE</th><th scope="col">Demo key</th></tr></thead><tbody>\n${rows}\n</tbody></table>`;
}

// The firmware's 3 x 5 font (tools/gen-font.py), for the title.
const GLYPHS = {
  A: '010 101 111 101 101', C: '011 100 100 100 011', E: '111 100 110 100 111', K: '101 101 110 101 101',
  L: '100 100 100 100 111', O: '010 101 101 101 010', P: '110 101 110 100 100', R: '110 101 110 101 101',
  T: '111 010 010 010 010',
};

/** Words in the firmware's font, one per line, as one path. */
export function pixelText(lines) {
  let d = '', width = 0;
  lines.forEach((line, row) => {
    width = Math.max(width, line.length * 4 - 1);
    [...line].forEach((ch, col) => {
      if (ch === ' ') return;
      GLYPHS[ch].replace(/ /g, '').split('').forEach((bit, i) => {
        if (bit === '1') d += `M${col * 4 + i % 3} ${row * 7 + Math.floor(i / 3)}h1v1h-1z`;
      });
    });
  });
  const height = lines.length * 7 - 2;
  return `<svg class="pixel" viewBox="0 0 ${width} ${height}" aria-hidden="true" shape-rendering="crispEdges"><path d="${d}"/></svg>`;
}
