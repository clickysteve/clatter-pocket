#!/usr/bin/env node
// Runs a CLATTER Pocket package in the FM-1 emulator (joakimeriksson/fm1-emu) and checks what
// the firmware itself puts on the LCD and the audio output. No mock-up: the emulated pi32v2
// executes the package's app.bin; this script only presses keys and reads the screen.
//
//   node tools/emu-test.mjs --wasm fm1_web.wasm [--fwsc PACKAGE] [--stock STOCK.fwsc] [--out DIR]
//
// The sequencer's checks listen: onsets are taken from the emulated audio output, the grid and
// the playhead from the LCD's pixels, and a generated pattern is compared with the host's.
// --wasm is the emulator's WebAssembly core (web/build.sh, or fm1_web.wasm from its site).
// --stock is a negative control: a stock package must fail the identity check.
// Writes a PNG of each screen to --out. Exit status 0 only if every check passed.
import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { deflateSync, crc32 } from "node:zlib";

const arg = (name, fallback) => {
  const i = process.argv.indexOf(name);
  return i > 0 ? process.argv[i + 1] : fallback;
};
const wasmPath = arg("--wasm");
const fwsc = arg("--fwsc", "build/fm1-replace/clatter-pocket-fm1-replace-UNVERIFIED.fwsc");
const stock = arg("--stock");
const out = arg("--out", "build/emu");
if (!wasmPath) {
  console.error("usage: emu-test.mjs --wasm fm1_web.wasm [--fwsc PACKAGE] [--stock STOCK.fwsc] [--out DIR]");
  process.exit(2);
}
mkdirSync(out, { recursive: true });

const BUTTON = { OCTDN: 0, OCTUP: 1, FX: 2, SCL: 3, ENV: 4, LFO: 5, EDIT: 6, GLO: 7, HOME: 8, SAVE: 9, ARP: 10, SEQ: 11, PLAY: 12, REC: 13 };
const KEY = (n) => 14 + n;
const ENC = { SELECT: 0, ALGORITHM: 1, KNOB1: 2, KNOB2: 3, KNOB3: 4, KNOB4: 5, PRESETS: 6 };
const MHZ = 240;
const RATE = 44118; // FM1_AUDIO_HARDWARE_RATE: the sequencer's clock
// ui/pocket_panel.c's grid
const stepX = (s) => 30 + 12 * s + 2 * (s >> 2);
const rowY = (t) => 97 + 11 * t;
const PAPER = 0x0000, ACCENT = 0xfbe0;
const TRACKS = 7, STEPS = 16;
// the DRUM keys: the first bank's note of each track, D1 D2 D3 SN CY CH OH; key 0 is F3 (53)
const DRUM_NOTE = [53, 57, 58, 54, 59, 55, 56];
// the nine positions of a bank, and what each key is called on screen
const BANK_TRACK = [0, 3, 5, 6, 1, 2, 4, 2, 3];
const BANK_NAME = ["KICK", "SNARE", "CL HAT", "OP HAT", "LOW TOM", "HI TOM", "CYMBAL", "PERC", "CLAP"];
const NOTE = (n) => KEY(n - 53);
const VOICE = ["DRUM 1", "DRUM 2", "DRUM 3", "SNARE", "CYMBAL", "CL HAT", "OP HAT"];
// GLOBAL's rows
const ROW = { KEYS: 0, OCTAVE: 1, KEY_VEL: 2, FOLLOW: 3, QUANTISE: 4, AUTO_STEP: 5, TEMPO: 6, SWING: 7, LENGTH: 8, STYLE: 9, DENSITY: 10, COMPLEX: 11, SEED: 12, MUTATE: 13, KIT_SEED: 14, WILD: 15, MIDI_CH: 16, MIDI_MAP: 17 };

// ui/pocket_panel.c's 3x5 font
const FONT = new Map();
const LETTERS = [0x2bed, 0x6bae, 0x3923, 0x6b6e, 0x79a7, 0x79a4, 0x396b, 0x5bed, 0x7497, 0x126a, 0x5bad, 0x4927, 0x5fed, 0x6b6d, 0x2b6a, 0x6ba4, 0x2b73, 0x6bad, 0x388e, 0x7492, 0x5b6f, 0x5b6a, 0x5bfd, 0x5aad, 0x5a92, 0x72a7];
const DIGITS = [0x7b6f, 0x2c97, 0x62a7, 0x628e, 0x5bc9, 0x798e, 0x39ef, 0x7292, 0x7bef, 0x7bce];
LETTERS.forEach((g, i) => FONT.set(g, String.fromCharCode(65 + i)));
DIGITS.forEach((g, i) => { if (!FONT.has(g)) FONT.set(g, String(i)); });
for (const [g, c] of [[0x52a5, "%"], [0x0002, "."], [0x0410, ":"], [0x01c0, "-"], [0x12a4, "/"], [0x05d0, "+"], [0x0014, ","], [0, " "]]) FONT.set(g, c);

class Machine {
  async boot(pkg, mhz = MHZ) {
    let x;
    const env = {
      fm1_jit_compile(ptr, len) {
        const bytes = new Uint8Array(x.memory.buffer, ptr, len).slice();
        const table = x.__indirect_function_table;
        const run = new WebAssembly.Instance(new WebAssembly.Module(bytes), { env: { memory: x.memory, table } }).exports.run;
        const slot = table.grow(1);
        table.set(slot, run);
        return slot;
      },
    };
    x = (await WebAssembly.instantiate(readFileSync(wasmPath), { env })).instance.exports;
    this.x = x;
    x.fm1_jit(1, 16);
    const p = x.fm1_alloc(pkg.length);
    new Uint8Array(x.memory.buffer, p, pkg.length).set(pkg);
    if (x.fm1_boot_fwsc(p, pkg.length, 0, 0, mhz) !== 0) throw new Error("the emulator refused the package");
    this.budget = mhz * 1e6 * 0.02;
    this.peak = 0;
    this.sample = 0;
    this.onsets = null;
    this.tape = null;
    return this;
  }
  // Emulated time passes in audio blocks; frames that are not audio yet still advance the machine.
  run(ms) {
    const x = this.x, until = x.fm1_ms() + ms;
    let guard = 0;
    while (x.fm1_ms() < until) {
      const have = x.fm1_render(128, this.budget);
      const l = new Float32Array(x.memory.buffer, x.fm1_out_l(), have), r = new Float32Array(x.memory.buffer, x.fm1_out_r(), have);
      for (let i = 0; i < have; i++) {
        const a = Math.max(Math.abs(l[i]), Math.abs(r[i]));
        this.peak = Math.max(this.peak, a);
        this.sample++;
        if (this.tape) this.tape.push(l[i]);
        // an onset: well above the decaying envelope of what was already sounding
        if (this.onsets) {
          if (a >= 0.02 && a > this.env * 2 && this.sample - this.last > 400) { this.onsets.push(this.sample); this.last = this.sample; }
          this.env = Math.max(a, this.env * 0.9985);
        }
      }
      if (x.fm1_halted()) throw new Error(`machine halted at ${x.fm1_ms()} ms`);
      if (++guard > 5e6) throw new Error("time does not advance");
    }
  }
  takePeak() { const p = this.peak; this.peak = 0; return p; }
  // What a key sounds at, in Hz: the median period between rising zero crossings after the attack.
  pitchOf(id, ms = 500) {
    this.run(1200); // what was sounding is gone
    this.tape = [];
    this.tap(id);
    const until = this.sample + (ms * RATE) / 1000;
    while (this.sample < until) this.run(5);
    const t = this.tape, rises = [];
    this.tape = null;
    const peak = t.reduce((a, v) => Math.max(a, Math.abs(v)), 0);
    const start = t.findIndex((v) => Math.abs(v) > peak / 4) + Math.floor(RATE / 25);
    for (let i = start; i < t.length && Math.abs(t[i]) + Math.abs(t[i - 1]) > peak / 500; i++) if (t[i - 1] < 0 && t[i] >= 0) rises.push(i);
    const periods = rises.slice(1).map((v, i) => v - rises[i]).sort((a, b) => a - b);
    return RATE / periods[periods.length >> 1];
  }
  // Onsets of `ms` of audio with `action` at the start of it, so a hit it causes is heard.
  listenTo(action, ms) {
    this.onsets = [];
    this.env = 0;
    this.last = 0;
    const from = this.sample;
    action();
    const until = from + (ms * RATE) / 1000;
    while (this.sample < until) this.run(5);
    const found = this.onsets;
    this.onsets = null;
    return found.map((v) => v - from);
  }
  // The whole chip, and sectors of it put back before anything has run (a restart keeps Flash).
  flash() { return new Uint8Array(this.x.memory.buffer, this.x.fm1_flash(), this.x.fm1_flash_len()).slice(); }
  restore(sectors) {
    const bytes = new Uint8Array(sectors.length * 4100);
    sectors.forEach(([at, data], i) => { new DataView(bytes.buffer).setUint32(i * 4100, at, true); bytes.set(data, i * 4100 + 4); });
    const p = this.x.fm1_alloc(bytes.length);
    new Uint8Array(this.x.memory.buffer, p, bytes.length).set(bytes);
    if (this.x.fm1_flash_restore(p, bytes.length) !== 0) throw new Error("the emulator refused the sectors");
  }
  // Which buttons are lit (the emulator's LED levels are since the last look).
  leds() { return new Float32Array(this.x.memory.buffer, this.x.fm1_leds(), 41).slice(); }
  // Onset sample numbers over `ms` of audio (fm1_ms is the emulated CPU's clock, not the output's).
  listen(ms) {
    this.onsets = [];
    this.env = 1; // whatever is already sounding is no onset
    this.last = 0;
    const until = this.sample + (ms * RATE) / 1000;
    while (this.sample < until) this.run(5);
    const found = this.onsets;
    this.onsets = null;
    return found;
  }
  // The pattern on screen, as "0" and "1" per step, a line per track.
  grid() {
    const fb = this.lcd();
    return Array.from({ length: TRACKS }, (_, t) => Array.from({ length: STEPS }, (_, s) => (fb[(rowY(t) + 8) * 240 + stepX(s) + 3] !== PAPER ? "1" : "0")).join(""));
  }
  gridHits() { return this.grid().join("").split("1").length - 1; }
  playhead(y = 81) { // MAIN's activity row is at 161
    const fb = this.lcd();
    for (let s = 0; s < STEPS; s++) if (fb[y * 240 + stepX(s) + 1] === ACCENT) return s;
    return -1;
  }
  tap(id, ms = 60) { this.x.fm1_key(id, 1); this.run(ms); this.x.fm1_key(id, 0); this.run(ms); }
  // `held` down, `id` tapped, `held` up.
  chord(held, id, ms = 60) { this.x.fm1_key(held, 1); this.run(ms); this.tap(id, ms); this.x.fm1_key(held, 0); this.run(ms); }
  // The emulator delivers detents over time: wait for them.
  turn(enc, clicks) { this.x.fm1_enc(enc, clicks); this.run(120 + 10 * Math.abs(clicks)); }
  // An encoder turned with SAVE held.
  chordTurn(enc, clicks) { this.x.fm1_key(BUTTON.SAVE, 1); this.run(60); this.turn(enc, clicks); this.x.fm1_key(BUTTON.SAVE, 0); this.run(60); }
  // Which page of steps is playing, from PATTERN's blocks, or -1.
  pagePlaying() { const fb = this.lcd(); for (let p = 0; p < 4; p++) if (fb[72 * 240 + 206 + 7 * p] === ACCENT) return p; return -1; }
  midi(...bytes) { for (const b of bytes) this.x.fm1_midi(b); }
  lcd() { return new Uint16Array(this.x.memory.buffer, this.x.fm1_lcd(), 240 * 240).slice(); }
  // The screen as text: at each font scale, the lines the panel's grid can hold.
  text() {
    const fb = this.lcd();
    const px = (x, y) => (x < 240 && y < 240 ? fb[y * 240 + x] : fb[0]);
    const read = (x0, y0, scale) => {
      let line = "";
      for (let x = x0; x + 3 * scale <= 240; x += 4 * scale) {
        let g = 0;
        const bg = px(x + 3 * scale, y0); // the gap beside the glyph: inverted text reads too
        for (let r = 0; r < 5; r++) for (let c = 0; c < 3; c++) {
          if (px(x + c * scale, y0 + r * scale) !== bg) g |= 1 << (14 - 3 * r - c);
        }
        line += FONT.get(g) ?? "?";
      }
      return line.replace(/\s+$/, "");
    };
    const lines = [];
    for (let band = 0; band < 15; band++) lines.push(read(8, band * 16 + 3, 2));
    // the start screen's own positions
    if (read(36, 56, 3).startsWith("CLATTER")) for (const [x, y, s] of [[36, 56, 3], [56, 84, 2], [8, 136, 2], [8, 152, 2], [8, 200, 2], [8, 216, 2]]) lines.push(read(x, y, s));
    return lines.map((l) => l.replace(/[?\s]+/g, " ").trim()).join("\n");
  }
  png(path) {
    const fb = this.lcd(), raw = Buffer.alloc(241 * 240 * 3 + 240);
    let o = 0;
    for (let y = 0; y < 240; y++) {
      raw[o++] = 0;
      for (let x = 0; x < 240; x++) {
        const c = fb[y * 240 + x];
        raw[o++] = (c >> 11) << 3; raw[o++] = ((c >> 5) & 63) << 2; raw[o++] = (c & 31) << 3;
      }
    }
    const chunk = (type, data) => {
      const t = Buffer.concat([Buffer.from(type), data]), len = Buffer.alloc(4), crc = Buffer.alloc(4);
      len.writeUInt32BE(data.length);
      crc.writeUInt32BE(crc32(t));
      return Buffer.concat([len, t, crc]);
    };
    const ihdr = Buffer.alloc(13);
    ihdr.writeUInt32BE(240, 0); ihdr.writeUInt32BE(240, 4); ihdr[8] = 8; ihdr[9] = 2;
    writeFileSync(path, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", ihdr),
      chunk("IDAT", deflateSync(raw)), chunk("IEND", Buffer.alloc(0))]));
  }
}

let checks = 0, failures = 0;
const check = (ok, what, detail = "") => {
  checks++;
  if (!ok) failures++;
  console.log(`${ok ? "ok  " : "FAIL"} ${what}${ok || !detail ? "" : `\n     ${String(detail).replace(/\n/g, "\n     ")}`}`);
};
const shows = (m, want, what) => { const t = m.text(); check(t.includes(want), what ?? `screen shows "${want}"`, t); };
const SILENT = 1e-3, LOUD = 0.05;

const near = (got, want, by) => Math.abs(got - want) <= by;
const gaps = (o) => o.slice(1).map((v, i) => v - o[i]);
const mean = (xs) => xs.reduce((a, b) => a + b, 0) / xs.length;
const hostPattern = (seed, density, complexity, style) =>
  execFileSync("build/host/test_seq", ["--print", seed, density, complexity, style].map(String)).toString().trim().split("\n");
// Encoder detents between two selections, by the real controls.
function moveTo(m, enc, from, to) {
  if (to !== from) m.turn(enc, to - from);
}

// The voice and the step the panel has selected, read from its screen, and moved by its controls.
const voiceOn = (m) => VOICE.findIndex((v) => m.text().includes(`VOICE ${v}`));
function selectVoice(m, t) {
  const from = voiceOn(m);
  if (from !== t) m.turn(ENC.SELECT, t - from);
  m.run(150);
}
function moveStep(m, step) { // on PATTERN
  const from = Number(/VEL\S* BPM\d+ SWG\d+ STEP(\d+)/.exec(m.text())?.[1]) - 1;
  if (from !== step) m.turn(ENC.KNOB4, step - from);
  m.run(150);
}
// A GLOBAL row moved by `by`, from any page, back on `home`.
function setting(m, row, by, home) {
  m.tap(BUTTON.HOME, 90);
  m.tap(BUTTON.GLO, 90);
  m.turn(ENC.KNOB1, -40);
  m.turn(ENC.KNOB1, row);
  m.turn(ENC.KNOB2, by);
  m.tap(home, 90);
  m.run(200);
}
function systemPage(m) {
  m.tap(BUTTON.HOME, 90);
  m.tap(BUTTON.GLO, 90);
  m.tap(BUTTON.GLO, 90);
}
const POCKET_KITS = 17; // INIT and the 16 characters
const hostKit = (character, seed, wild) =>
  execFileSync("build/host/test_kit", ["--print", character, seed, wild].map(String)).toString().trim().split("\n").map(Number);
const PAR = { COARSE1: 8, VELOD1: 50, FILTER1: 37, DRIVE1: 102 };
// SOUND's four values for the selected voice, as shown.
const soundValues = (m) => ["PITCH", "DECAY", "FILTER", "DRIVE"].map((n) => Number(new RegExp(`${n} +(\\d+)`).exec(m.text())?.[1]));

// Your twelve: a playable, generative drum machine, by its own controls.
async function performance(m) {
  // ---- 12. PRESETS browses kits, and a rhythm plays through all of it
  m.tap(BUTTON.SEQ, 90);
  m.run(300);
  const rhythm = m.grid().join();
  m.tap(BUTTON.PLAY);
  m.run(300);
  for (const [clicks, name] of [[1, "KIT CLASSIC"], [1, "KIT ANALOG"], [1, "KIT DIGITAL"], [1, "KIT INDUSTRIAL"]]) {
    m.turn(ENC.PRESETS, clicks);
    shows(m, name, `PRESETS: ${name}`);
  }
  m.turn(ENC.PRESETS, -4);
  shows(m, "KIT INIT", "PRESETS back to the init kit");
  m.turn(ENC.PRESETS, -1);
  shows(m, "KIT CHAOS", "PRESETS wraps");
  m.turn(ENC.PRESETS, 1);
  m.run(300);
  check(m.grid().join() === rhythm, "browsing kits leaves the rhythm");

  // ---- 1. a complete kit, and it is the host's
  m.turn(ENC.PRESETS, 4); // INDUSTRIAL, factory seed 148
  m.tap(BUTTON.ENV, 60);
  shows(m, "GENERATING KIT", "ENV shows GENERATING KIT");
  m.run(700);
  shows(m, "KIT INDUSTRIAL 149", "then the new kit's name and seed");
  m.png(`${out}/perf-1-kit.png`);
  check(m.grid().join() === rhythm, "a new kit leaves the rhythm");
  check(m.playhead() >= 0, "and the pattern plays on");
  m.tap(BUTTON.EDIT, 90);
  m.run(300);
  selectVoice(m, 0);
  const kit = hostKit(3, 149, 50);
  const shown = soundValues(m), want = [kit[PAR.COARSE1], kit[PAR.VELOD1], kit[PAR.FILTER1], kit[PAR.DRIVE1]];
  check(shown.join() === want.join(), `SOUND shows the host's kit for this seed (${shown})`, `${shown} host ${want}`);
  m.png(`${out}/perf-2-sound.png`);

  // ---- 9. one voice's sound, the others left alone
  selectVoice(m, 3);
  const snare = soundValues(m);
  selectVoice(m, 0);
  const kick = soundValues(m);
  selectVoice(m, 3);
  m.chord(BUTTON.SAVE, BUTTON.ENV);
  shows(m, "NEW SOUND SNARE", "SAVE+ENV: a new sound for the selected voice");
  m.run(300);
  check(soundValues(m).join() !== snare.join(), "the snare changed", soundValues(m));
  selectVoice(m, 0);
  check(soundValues(m).join() === kick.join(), "the kick did not", soundValues(m));
  m.tap(BUTTON.SEQ, 90);
  m.run(300);
  check(m.grid().join() === rhythm, "and neither did the rhythm");

  // ---- 2, 3. a rhythm, from a style, heard
  m.turn(ENC.ALGORITHM, 2);
  shows(m, "STYLE BREAKS", "ALGORITHM chooses the style");
  const before = m.grid();
  m.tap(BUTTON.ARP, 60);
  shows(m, "NEW PATTERN BREAKS", "ARP generates, and says so");
  m.run(300);
  const breaks = m.grid();
  check(breaks.join() !== before.join(), "a new rhythm");
  check(breaks.join() === hostPattern(1, 50, 30, 2).join(), "the same seed in the style, bit for bit, as the host", breaks.join("\n"));
  check(m.listen(2000).length > 0, "and it is heard");
  m.tap(BUTTON.ARP, 90);
  m.run(300);
  check(m.grid().join() === hostPattern(2, 50, 30, 2).join(), "ARP again: the next seed");

  // ---- 10. one track's rhythm, the others left alone
  const whole = m.grid();
  selectVoice(m, 3);
  m.chord(BUTTON.SAVE, BUTTON.ARP);
  shows(m, "NEW PATTERN", "SAVE+ARP: a new pattern for one track");
  m.run(300);
  const one = m.grid();
  const others = (a, b, t) => a.filter((_, i) => i !== t).join() === b.filter((_, i) => i !== t).join();
  check(one[3] !== whole[3], "that track changed", `${whole[3]}\n${one[3]}`);
  check(others(one, whole, 3), "the others did not", one.join("\n"));

  // ---- 11. mutation: recognisable, and one track
  const diffs = [];
  for (let i = 0; i < 5; i++) {
    const was = m.grid().join("");
    m.tap(BUTTON.LFO, 90);
    m.run(300);
    const now = m.grid().join("");
    diffs.push([...now].filter((c, k) => c !== was[k]).length);
  }
  check(diffs.some((d) => d > 0) && diffs.every((d) => d <= 30), `LFO mutates: ${diffs.join(" ")} steps changed`, diffs.join(" "));
  const held = m.grid();
  let moved = false;
  for (let i = 0; i < 6 && !moved; i++) {
    m.chord(BUTTON.SAVE, BUTTON.LFO);
    m.run(300);
    moved = m.grid()[3] !== held[3];
  }
  check(moved, "SAVE+LFO mutates the selected track");
  check(others(m.grid(), held, 3), "and only that track", m.grid().join("\n"));
  m.chord(BUTTON.SAVE, BUTTON.OCTDN);
  m.run(300);
  shows(m, "UNDO", "SAVE+OCT- undoes the last change");
  m.chord(BUTTON.SAVE, BUTTON.OCTUP);
  m.run(300);
  shows(m, "REDO", "SAVE+OCT+ brings it back");

  // ---- kits: the way back
  m.tap(BUTTON.ENV, 90);
  m.run(800);
  m.chord(BUTTON.SAVE, BUTTON.OCTDN);
  m.run(300);
  shows(m, "UNDO", "the same undo brings the last kit back");

  // ---- the machine under all of that: kits land on SYSTEM, whose figures cover the last 500 ms
  systemPage(m);
  shows(m, "BOOT+HOME SAFE", "SYSTEM says how to reach SAFE MODE");
  let worst = 0;
  for (let i = 0; i < 8; i++) {
    m.tap(BUTTON.ENV, 60);
    m.run(560);
    worst = Math.max(worst, Number(/CPU PEAK +([\d.]+)%/.exec(m.text())?.[1]));
  }
  check(worst > 0 && worst < 60, `a kit loading while the pattern plays: CPU peak ${worst} %`, m.text());
  let sys = m.text();
  check(/OVERRUNS +0\b/.test(sys) && /DROPPED +0\b/.test(sys), "no overruns or dropped events through kits and patterns while playing", sys);
  m.tap(BUTTON.SEQ, 90);
  m.tap(BUTTON.PLAY);
  m.run(1800);

  // ---- what follows is measured on the init kit: plain tones, the same on every run
  m.turn(ENC.PRESETS, POCKET_KITS - 4);
  m.run(500);
  shows(m, "CLATTER POCKET");
  // ---- 5. DRUM: three banks of nine; the selection follows
  m.tap(BUTTON.HOME, 90);
  for (let k = 0; k < 27; k++) {
    const position = k % 9, name = BANK_NAME[position] + ["", " A", " B"][(k / 9) | 0];
    m.takePeak();
    m.tap(KEY(k), 60);
    check(m.takePeak() > LOUD / 5, `key ${k + 1} sounds`);
    shows(m, `VOICE ${VOICE[BANK_TRACK[position]]}`, `and is on ${VOICE[BANK_TRACK[position]]}`);
    shows(m, name, `and is called ${name}`);
    m.run(500);
  }
  m.tap(BUTTON.SEQ, 90);
  m.run(1500);

  // ---- 4. SCL
  selectVoice(m, 1); // the low tom
  const pattern = m.grid().join();
  m.tap(BUTTON.SCL, 60);
  shows(m, "KEYS: PITCH", "SCL: KEYS: PITCH");
  check(m.leds()[BUTTON.SCL] >= 0, "SCL's light is read");
  m.run(1400);
  shows(m, "KEYS: PITCH OCT 0", "the status line keeps the mode and octave");
  check(m.grid().join() === pattern, "SCL leaves the pattern");
  m.png(`${out}/perf-3-pitch.png`);

  // ---- 6, 8. one instrument chromatically, across octaves, in the emulated audio
  const voice = /VOICE (DRUM \d|SNARE|CYMBAL|CL HAT|OP HAT)/.exec(m.text())?.[1];
  check(voice === "DRUM 2", `PITCH plays the selected voice (${voice})`, m.text());
  const c4 = m.pitchOf(NOTE(60)), c5 = m.pitchOf(NOTE(72)), g4 = m.pitchOf(NOTE(67));
  check(c5 / c4 > 1.8 && c5 / c4 < 2.2, `an octave up the keyboard: ${c4.toFixed(0)} to ${c5.toFixed(0)} Hz`, `${c4} ${c5}`);
  check(g4 / c4 > 1.35 && g4 / c4 < 1.65, `a fifth: ratio ${(g4 / c4).toFixed(2)}`, `${c4} ${g4}`);
  m.tap(BUTTON.OCTUP, 60);
  m.run(1400);
  shows(m, "OCT 1", "OCT+ raises the octave");
  const c4up = m.pitchOf(NOTE(60));
  check(Math.abs(c4up / c5 - 1) < 0.05, `OCT+ gives the key an octave up: ${c4up.toFixed(0)}, ${c5.toFixed(0)}`, `${c5} ${c4up}`);
  m.tap(BUTTON.OCTDN, 60);
  for (let i = 0; i < 6; i++) m.tap(BUTTON.OCTUP, 40);
  m.run(1400);
  shows(m, "OCT 3", "the octave stops at +3");
  for (let i = 0; i < 3; i++) m.tap(BUTTON.OCTDN, 40);
  m.run(300);
  const c4again = m.pitchOf(NOTE(60));
  check(Math.abs(c4again / c4 - 1) < 0.02, `back at OCT 0 the pitch is the same: ${c4.toFixed(0)}, ${c4again.toFixed(0)}`, `${c4} ${c4again}`);

  // ---- 7. REC, stopped: the insertion point
  m.tap(BUTTON.SCL, 90); // DRUM
  m.run(1400);
  m.tap(BUTTON.REC, 60);
  shows(m, "REC ON", "REC ON");
  check(m.leds()[BUTTON.REC] > 0, "REC lights");
  m.run(1400);
  shows(m, "REC ON", "and stays on the status line");
  const entered = m.grid();
  { // the row empty, step by step, by the keys
    const row = entered[5];
    for (let s = 0; s < STEPS; s++) if (row[s] === "1") { moveStep(m, s); m.tap(NOTE(DRUM_NOTE[5]), 30); }
    m.run(200);
  }
  check(m.grid()[5] === "0".repeat(STEPS), "the same key removes each step", m.grid()[5]);
  check(others(m.grid(), entered, 5), "other tracks stay", m.grid().join("\n"));
  moveStep(m, 4);
  m.tap(NOTE(DRUM_NOTE[5]), 60);
  m.run(200);
  check(m.grid()[5][4] === "1", "a key writes the selected step");
  m.png(`${out}/perf-4-entry.png`);
  // pitch: a different note replaces, the same note removes (the selection followed the hat)
  m.tap(BUTTON.SCL, 90);
  m.tap(NOTE(67), 60);
  m.run(200);
  shows(m, "S5 N67", "PITCH writes the note");
  m.tap(NOTE(70), 60);
  m.run(200);
  shows(m, "S5 N70", "a different pitch replaces it");
  m.tap(NOTE(70), 60);
  m.run(200);
  shows(m, "STEP 5", "the same pitch removes it");
  m.tap(BUTTON.SCL, 90);

  // ---- 7. REC, playing: the hits land on the grid without stopping anything
  m.tap(BUTTON.ARP, 90);
  m.run(300);
  const open = m.grid();
  const silentStep = [...open[5]].findIndex((c) => c === "0");
  m.tap(BUTTON.PLAY);
  m.run(200);
  for (let i = 0; i < 400 && m.playhead() !== ((silentStep + STEPS - 1) % STEPS); i++) m.run(5);
  // a sixteenth at 120 BPM is 125 ms: press as the step before it plays out
  m.run(95);
  m.tap(NOTE(DRUM_NOTE[5]), 20);
  m.run(300);
  const live = m.grid();
  check(live[5][silentStep] === "1" || live[5][(silentStep + 1) % STEPS] === "1", "a live hit is quantised onto the track", `${open[5]}\n${live[5]}`);
  check(others(live, open, 5), "other tracks are untouched", live.join("\n"));
  shows(m, "PLAY", "recording does not stop the pattern");
  m.tap(BUTTON.REC, 90);
  m.run(1400);
  check(!m.text().includes("REC ON"), "REC off", m.text());
  systemPage(m);
  m.run(800);
  sys = m.text();
  check(/OVERRUNS +0\b/.test(sys) && /DROPPED +0\b/.test(sys), "no overruns or dropped events through recording", sys);
  m.tap(BUTTON.SEQ, 90);
  m.tap(BUTTON.PLAY);
  m.run(1800);

  // ---- MIDI follows the keyboard: DRUM's GM map, PITCH's notes, and it records
  m.tap(BUTTON.SCL, 90);
  m.tap(BUTTON.REC, 90);
  m.midi(0x90, 72, 70);
  m.run(200);
  shows(m, "MIDI NOTE 72", "MIDI plays the selected voice in PITCH");
  m.tap(BUTTON.REC, 90);
  m.tap(BUTTON.SCL, 90);
  m.run(1400);

  // ---- GLOBAL: settings, then the diagnostics
  m.tap(BUTTON.HOME, 90);
  m.tap(BUTTON.GLO, 90);
  m.turn(ENC.KNOB1, -40);
  m.run(300);
  shows(m, "GLOBAL", "GLO opens GLOBAL");
  shows(m, "KEYS DRUM", "GLOBAL shows the keyboard mode");
  shows(m, "QUANTISE NEAREST", "and the recording quantisation");
  m.png(`${out}/page-global.png`);
  m.turn(ENC.KNOB1, 16);
  m.run(200);
  shows(m, "MIDI CH ALL", "the rows scroll");
  m.png(`${out}/page-global-2.png`);
  m.turn(ENC.KNOB1, -16);
  m.tap(BUTTON.GLO, 90);
  m.run(300);
  shows(m, "SYSTEM", "GLO again opens SYSTEM");
  shows(m, "CPU MEAN", "the diagnostics are still there");
  m.tap(BUTTON.HOME, 90);
  m.turn(ENC.PRESETS, 4);
  m.run(1600);
  shows(m, "KIT INDUSTRIAL", "MAIN names the kit");
  m.png(`${out}/page-main-performance.png`);
}

// Saving: what the firmware really wrote to the emulated chip, a restart, and what it refuses.
const STORE = 0xe5000, STORE_END = 0xe9000, SECTOR = 4096, USER = 507904, USR = 0xea000, USR_END = 0xfc000;
const changed = (a, b) => {
  const out = [];
  for (let at = 0; at < a.length; at += SECTOR) {
    for (let i = at; i < at + SECTOR; i++) if (a[i] !== b[i]) { out.push(at); break; }
  }
  return out;
};
// A bank of kits or patterns as firmware 0.5.0 wrote it.
function oldBank(kind, generation, fill) {
  const d = new Uint8Array(SECTOR).fill(255), v = new DataView(d.buffer);
  d.fill(0, 20, 3104);
  v.setUint32(0, 0x31535043, true);
  v.setUint32(4, 0, true);
  v.setUint32(12, 1 << 8 | kind, true);
  v.setUint32(16, generation, true);
  fill(d, 32);
  v.setUint32(8, crc32(d.subarray(12, 3104)), true);
  return d;
}

async function storage(pkg) {
  const fresh = async (sectors = []) => {
    const n = await new Machine().boot(pkg);
    if (sectors.length) n.restore(sectors);
    n.x.fm1_pot(1023);
    n.run(2500);
    return n;
  };
  const keep = (from) => { // the sectors a restart would find, as they are on the chip
    const f = from.flash(), out = [];
    for (let at = 0; at < f.length; at += SECTOR) if (at >= USER && at < 0xff000) out.push([at, f.slice(at, at + SECTOR)]);
    return out.filter(([, d]) => d.some((b) => b !== 255));
  };
  const sound = (m) => { m.tap(BUTTON.EDIT, 90); m.run(300); selectVoice(m, 0); return soundValues(m); };
  // what a blank device's user partition must look like afterwards, and factory data to protect
  const factory = new Uint8Array(SECTOR).fill(0x63), library = new Uint8Array(SECTOR).fill(0x00);
  let m = await fresh([[0xee000, factory], [0xfa000, library]]);
  const start = m.flash();
  check(changed(start, start).length === 0 && start.subarray(STORE, STORE_END).every((b) => b === 255), "our sectors start blank");

  // ---- using it writes nothing: only a tap of SAVE on its page reaches Flash
  m.tap(BUTTON.ARP, 90); m.tap(BUTTON.ENV, 90); m.run(800);
  m.tap(BUTTON.EDIT, 90); m.tap(BUTTON.SEQ, 90); m.tap(BUTTON.GLO, 90); m.tap(BUTTON.HOME, 90);
  m.chord(BUTTON.SAVE, BUTTON.OCTDN); m.chord(BUTTON.SAVE, BUTTON.OCTUP);
  m.run(300);
  const idle = changed(start, m.flash()).filter((a) => a >= USER);
  check(idle.length === 0, "nothing touches the user partition until asked, undo included", idle.join());

  // ---- a project: a long pattern with a note and a condition, tempo, swing, kit
  const kit = sound(m);
  m.tap(BUTTON.SEQ, 90); m.run(200);
  m.chordTurn(ENC.KNOB3, 1);
  shows(m, "PAGE 1/2", "SAVE+KNOB3 lengthens the pattern");
  m.tap(BUTTON.ARP, 90); m.run(200);
  m.turn(ENC.KNOB4, 18);
  shows(m, "PAGE 2/2", "KNOB4 reaches the second page");
  m.tap(BUTTON.SCL, 90); m.tap(BUTTON.REC, 90); m.tap(NOTE(72), 60); m.tap(BUTTON.REC, 90); m.tap(BUTTON.SCL, 90);
  m.chordTurn(ENC.KNOB1, 3);
  shows(m, "S19 N72 1:3", "SAVE+KNOB1 gives the step a condition, shown");
  m.turn(ENC.KNOB2, 13); m.turn(ENC.KNOB3, 7);
  m.run(300);
  const page2 = m.grid().join();
  m.turn(ENC.KNOB4, -18); m.run(200);
  const page1 = m.grid().join();
  m.png(`${out}/page-pattern-condition.png`);
  m.tap(BUTTON.PLAY);
  m.tap(BUTTON.SAVE, 60);
  m.run(200);
  shows(m, "PROJECTS", "a tap of SAVE opens PROJECTS");
  shows(m, "1 --", "an empty slot");
  m.png(`${out}/page-projects-empty.png`);
  m.tap(BUTTON.SAVE, 60);
  shows(m, "STOP TO SAVE", "not while it plays");
  m.tap(BUTTON.PLAY);
  m.run(300);
  m.tap(BUTTON.SAVE, 60);
  shows(m, "SAVED 1", "SAVE again stores it, and says so");
  m.run(300);
  shows(m, "1 CLASSIC 1 133 32", "the slot names the kit, the tempo and the length");
  m.png(`${out}/page-projects.png`);
  m.tap(BUTTON.SAVE, 60);
  shows(m, "OVERWRITE: SAVE AGAIN", "a slot in use asks first");
  m.run(1500);

  // ---- everything changes, and it comes back, while playing
  m.tap(BUTTON.ENV, 90); m.run(900);
  check(sound(m).join() !== kit.join(), "ENV changes the sound");
  m.tap(BUTTON.SEQ, 90); m.run(200);
  m.tap(BUTTON.ARP, 90);
  m.chordTurn(ENC.KNOB3, -1);
  m.turn(ENC.KNOB2, -13); m.turn(ENC.KNOB3, -7);
  m.run(300);
  check(m.grid().join() !== page1, "ARP changes the pattern");
  m.tap(BUTTON.PLAY);
  m.tap(BUTTON.SAVE, 60);
  m.tap(BUTTON.OCTDN, 60);
  shows(m, "LOADED 1", "OCT- loads it");
  m.tap(BUTTON.SEQ, 90);
  m.run(800);
  check(m.grid().join() === page1, "the pattern is back, while playing", m.grid().join("\n"));
  shows(m, "133 BPM", "with its tempo");
  shows(m, "SWG57", "and swing");
  shows(m, "PAGE 1/2", "and length");
  shows(m, "PLAY", "loading does not stop the pattern");
  m.tap(BUTTON.PLAY);
  m.run(800);
  check(sound(m).join() === kit.join(), `the kit is back (${kit})`, soundValues(m));
  m.chord(BUTTON.SAVE, BUTTON.OCTDN);
  m.run(800);
  shows(m, "UNDO", "a load is undone");
  check(sound(m).join() !== kit.join(), "kit and all", soundValues(m));
  m.chord(BUTTON.SAVE, BUTTON.OCTUP);
  m.run(800);
  check(soundValues(m).join() === kit.join(), "and redone", soundValues(m));

  // ---- what was written, exactly
  const after = m.flash(), wrote = changed(start, after);
  const inUser = wrote.filter((a) => a >= USER);
  check(inUser.length > 0 && inUser.every((a) => a >= STORE && a < STORE_END), `only our sectors changed in the user partition (${inUser.map((a) => a.toString(16))})`, inUser.join());
  check(wrote.every((a) => a >= 471040), "nothing in the image", wrote.join());
  check(wrote.filter((a) => a < USER).every((a) => a >= 471040 && a < 503808), "and at most the SDK's own settings", wrote.join());
  check(after.subarray(0xee000, 0xef000).every((b) => b === 0x63) && after.subarray(0xfa000, 0xfb000).every((b) => b === 0), "factory data is intact");
  check(!wrote.some((a) => a >= USR && a < USR_END), "the vendor's USR area was not touched");

  // ---- a restart
  const saved = keep(m);
  m = await fresh(saved);
  check(sound(m).join() !== kit.join(), "a restart starts from the init kit", soundValues(m));
  m.chord(BUTTON.SAVE, BUTTON.OCTDN);
  shows(m, "NOTHING TO UNDO", "and with no history");
  m.tap(BUTTON.SAVE, 60);
  m.run(300);
  shows(m, "1 CLASSIC 1 133 32", "after a restart the project is listed");
  m.tap(BUTTON.OCTDN, 60);
  m.run(800);
  check(sound(m).join() === kit.join(), "after a restart the kit loads", soundValues(m));
  m.tap(BUTTON.SEQ, 90); m.run(300);
  check(m.grid().join() === page1, "and the first page", m.grid().join("\n"));
  m.turn(ENC.KNOB4, 18); m.run(300);
  check(m.grid().join() === page2, "and the second, note and all", m.grid().join("\n"));
  shows(m, "S19 N72 1:3", "and its condition");
  shows(m, "133 BPM", "and its tempo");

  // ---- a second save beside it, so that there are two copies: then damage to each
  m.tap(BUTTON.SAVE, 60);
  m.run(200);
  m.turn(ENC.SELECT, 1);
  m.tap(BUTTON.SAVE, 60);
  m.run(300);
  shows(m, "2 CLASSIC 1", "SELECT moves to a second slot");
  const two = keep(m), copies = two.filter(([a]) => a >= STORE && a < STORE_END);
  check(copies.length === 2, "two sectors in use, the second save beside the first");
  const swapped = (fn) => two.map(([a, d]) => { const c = d.slice(); fn(a, c); return [a, c]; }).filter(([, d]) => d.some((b) => b !== 255));
  const newest = copies.reduce((p, q) => (new DataView(q[1].buffer).getUint32(16, true) > new DataView(p[1].buffer).getUint32(16, true) ? q : p))[0];
  const older = copies.find(([a]) => a !== newest)[0];
  const open = (n) => { n.tap(BUTTON.SAVE, 60); n.run(300); };

  m = await fresh(swapped((a, d) => { if (a === newest) d[300] ^= 4; }));
  open(m);
  shows(m, "1 CLASSIC 1", "a corrupted sector: the copy before it is used");
  check(!m.text().includes("2 CLASSIC"), "the damaged save is not shown", m.text());
  m.tap(BUTTON.OCTDN, 60);
  shows(m, "LOADED 1", "and loads");
  m.run(300);
  m.turn(ENC.KNOB1, 1);
  m.tap(BUTTON.SAVE, 60);
  m.run(300);
  shows(m, "2 CLASSIC 1", "and saving works past the damage");

  // an interrupted save, as the chip is left: half an erase, half a page
  m = await fresh(swapped((a, d) => { if (a === newest) d.fill(255, 0, 2048); }));
  open(m);
  shows(m, "1 CLASSIC 1", "an erase cut half way: the last whole copy stands");
  m = await fresh(swapped((a, d) => { if (a === newest) { d[4] = d[5] = d[6] = d[7] = 255; } }));
  open(m);
  check(!m.text().includes("2 CLASSIC") && m.text().includes("1 CLASSIC 1"), "a save cut before its commit does not count", m.text());

  // both damaged: nothing believed, nothing crashes, it says so
  m = await fresh(swapped((a, d) => { if (a === newest || a === older) d[300] ^= 4; }));
  open(m);
  shows(m, "SAVED DATA DAMAGED", "both copies corrupted: it says so");
  shows(m, "1 --", "and shows nothing it cannot trust");
  m.tap(BUTTON.OCTDN, 60);
  m.run(200);
  check(!m.text().includes("LOADED"), "and loads nothing");

  // ---- saves from 0.5.0: found, loaded, untouched, and erased only when asked
  const hostKitBytes = hostKit(3, 149, 50);
  const oldKits = oldBank(0, 4, (d, at) => { // slot 2
    d.set([1, 4, 1, 0, 149, 0, 66, 0], at + 256);
    d.set(hostKitBytes.slice(0, 247).map((v) => v & 127), at + 256 + 8);
  });
  const oldPatterns = oldBank(1, 2, (d, at) => { // slot 3: a hit on step 6 of DRUM 2, at 140 BPM
    d.set([1, 60, 50, 30, 0, 1, 0, 0, 9, 0, 140, 0], at + 2 * 256);
    d.set([100, 0], at + 2 * 256 + 12 + 32 + 10);
  });
  m = await fresh([[STORE, oldKits], [STORE + 3 * SECTOR, oldPatterns]]);
  const chip = m.flash();
  open(m);
  shows(m, "4 HELD BY OLD SAVES", "old saves hold their sectors, and it says so");
  m.png(`${out}/page-projects-old.png`);
  m.turn(ENC.SELECT, 10);
  m.run(300);
  shows(m, "OLD KIT 2 INDUSTRIAL 149", "an old kit is listed");
  m.tap(BUTTON.OCTDN, 60);
  shows(m, "LOADED", "and loads");
  m.run(800);
  check(sound(m).join() === [hostKitBytes[PAR.COARSE1], hostKitBytes[PAR.VELOD1], hostKitBytes[PAR.FILTER1], hostKitBytes[PAR.DRIVE1]].join(), "the old kit is on SOUND", soundValues(m));
  m.tap(BUTTON.SAVE, 60);
  m.run(200);
  m.turn(ENC.SELECT, 13);
  m.run(300);
  shows(m, "OLD PAT 3 MACHINE 140", "an old pattern is listed");
  m.tap(BUTTON.SAVE, 60);
  shows(m, "OLD SAVES ONLY LOAD", "and is not written to");
  m.run(1400);
  m.tap(BUTTON.OCTDN, 60);
  m.tap(BUTTON.SEQ, 90);
  m.run(300);
  shows(m, "140 BPM", "the old pattern loads, with its tempo");
  check(m.grid()[1][5] === "1", "and its step", m.grid().join("\n"));
  shows(m, "PAGE 1/1", "one page");
  check(changed(chip, m.flash()).filter((a) => a >= USER).length === 0, "loading wrote nothing");
  m.tap(BUTTON.SAVE, 60);
  m.run(200);
  m.turn(ENC.SELECT, -40);
  m.tap(BUTTON.SAVE, 60);
  m.run(300);
  shows(m, "SAVED 1", "a project goes beside them");
  const beside = m.flash();
  check(beside.subarray(STORE, STORE + SECTOR).every((b, i) => b === chip[STORE + i]) && beside.subarray(STORE + 3 * SECTOR, STORE_END).every((b, i) => b === chip[STORE + 3 * SECTOR + i]), "and the old banks are byte for byte as they were");
  m.turn(ENC.SELECT, 40);
  m.run(300);
  shows(m, "ERASE OLD SAVES", "erasing them is a row of its own");
  m.tap(BUTTON.SAVE, 60);
  shows(m, "ERASE OLD: SAVE AGAIN", "and asks");
  check(m.flash().subarray(STORE, STORE + SECTOR).every((b, i) => b === chip[STORE + i]), "one press erases nothing");
  m.tap(BUTTON.SAVE, 60);
  shows(m, "OLD SAVES ERASED", "the second does");
  m.run(300);
  shows(m, "1 ", "the rows are projects again");
  m.run(1400);
  m.turn(ENC.SELECT, 8);
  m.run(300);
  shows(m, "9 --", "and every project is free");
  check(!m.text().includes("HELD BY"), "nothing is held back", m.text());

  // someone else's data in our sectors: never touched
  const foreign = new Uint8Array(SECTOR).fill(0x42);
  m = await fresh([[STORE, foreign]]);
  const held = m.flash();
  open(m);
  shows(m, "FLASH IN USE: SAVING OFF", "data that is not ours closes the store");
  m.tap(BUTTON.SAVE, 60);
  m.tap(BUTTON.SAVE, 60);
  m.run(300);
  check(changed(held, m.flash()).filter((a) => a >= USER).length === 0, "and it stays as it was");
  // a sample bank long enough to reach them, even with them blank
  const bank = new Uint8Array(SECTOR).fill(255);
  new DataView(bank.buffer).setUint32(0, 0x53314d46, true);
  new DataView(bank.buffer).setUint32(8, 400000, true);
  m = await fresh([[USER + 69632, bank]]);
  open(m);
  shows(m, "FLASH IN USE: SAVING OFF", "a stock sample bank over them closes the store too");

  // ---- SAFE MODE, with saves on the chip: not a read of them, and every control tried
  m = await new Machine().boot(pkg);
  m.restore(saved);
  m.x.fm1_pot(1023);
  m.run(300);
  m.x.fm1_key(BUTTON.HOME, 1);
  m.run(2200);
  m.x.fm1_key(BUTTON.HOME, 0);
  m.run(400);
  const safe = m.flash();
  m.tap(BUTTON.SAVE, 60);
  m.tap(BUTTON.SAVE, 60);
  m.chord(BUTTON.SAVE, BUTTON.OCTDN);
  m.chord(BUTTON.SAVE, BUTTON.OCTUP);
  m.tap(BUTTON.OCTDN, 60);
  m.run(300);
  shows(m, "SAFE MODE - ENGINE OFF", "SAFE MODE ignores the save controls");
  check(changed(safe, m.flash()).filter((a) => a >= USER).length === 0, "and writes nothing");
}

// Longer patterns and conditions, heard in the emulated audio; the one history; the worst case.
async function longPatterns(m) {
  m.tap(BUTTON.SEQ, 90);
  m.run(300);
  // ---- every length, across its pages
  for (const [pages, by] of [[2, 1], [3, 1], [4, 1]]) {
    m.chordTurn(ENC.KNOB3, by);
    m.run(200);
    shows(m, `PAGE 1/${pages}`, `SAVE+KNOB3: ${pages * 16} steps`);
    m.tap(BUTTON.ARP, 90);
    m.run(300);
    m.tap(BUTTON.PLAY);
    const seen = [];
    for (let i = 0; i < 16 * pages * 2.4 + 20; i++) {
      m.run(50);
      const p = m.pagePlaying();
      if (p >= 0 && seen[seen.length - 1] !== p) seen.push(p);
    }
    const order = Array.from({ length: pages }, (_, p) => p);
    check(seen.length >= pages && seen.every((p, i) => i === 0 || p === (seen[i - 1] + 1) % pages) && order.every((p) => seen.includes(p)), `${pages * 16} steps play through ${pages} pages in order and wrap (${seen.join("")})`, seen.join());
    m.tap(BUTTON.PLAY);
    m.run(300);
  }
  m.png(`${out}/page-pattern-long.png`);
  // ---- pages turn by a chord, and a long pattern repeats at its own length
  m.chordTurn(ENC.KNOB4, 2);
  shows(m, "PAGE 3/4", "SAVE+KNOB4 turns the page");
  m.chordTurn(ENC.KNOB4, -2);
  m.tap(BUTTON.PLAY);
  m.run(300);
  const loop = (RATE * 15 * 64) / 120;
  const heard = m.listen(17000);
  const period = heard.filter((v) => v - heard[0] < loop - 300);
  const again = period.filter((v) => heard.some((w) => near(w, v + loop, 140)));
  check(period.length >= 8 && again.length >= period.length * 0.8, `64 steps repeat every ${loop} samples (${again.length} of ${period.length} onsets recur)`, heard.join());
  m.tap(BUTTON.PLAY);
  m.run(1800);

  // ---- conditions, heard: one closed hat on step 1, and nothing else
  m.chordTurn(ENC.KNOB3, -3);
  shows(m, "PAGE 1/1", "back to 16 steps");
  m.tap(BUTTON.REC, 60);
  const setRow = (t, row) => {
    const was = m.grid()[t];
    for (let s = 0; s < STEPS; s++) if (was[s] !== row[s]) { moveStep(m, s); m.tap(NOTE(DRUM_NOTE[t]), 30); }
    m.run(200);
  };
  for (let t = 0; t < TRACKS; t++) setRow(t, "0".repeat(STEPS));
  setRow(5, "1" + "0".repeat(STEPS - 1));
  m.tap(BUTTON.REC, 60);
  moveStep(m, 0);
  selectVoice(m, 5);
  const bar = (RATE * 15 * 16) / 120;
  // a pass is 2 s: the passes with a hit, counted from PLAY
  const passes = (count) => {
    const o = m.listenTo(() => m.x.fm1_key(BUTTON.PLAY, 1), count * 2000 - 150);
    m.x.fm1_key(BUTTON.PLAY, 0);
    m.run(60);
    m.tap(BUTTON.PLAY);
    m.run(1800);
    return o.map((v) => Math.floor(v / bar));
  };
  for (const [clicks, name, every, turn] of [[1, "1:2", 2, 0], [1, "2:2", 2, 1], [1, "1:3", 3, 0], [1, "2:3", 3, 1], [2, "1:4", 4, 0], [3, "4:4", 4, 3]]) {
    m.chordTurn(ENC.KNOB1, clicks);
    shows(m, name, `SAVE+KNOB1: ${name}`);
    const want = Array.from({ length: 3 }, (_, n) => turn + n * every);
    const heardPasses = passes(every * 3);
    check(heardPasses.join() === want.join(), `${name}: passes ${want.join(" ")} of ${every * 3} (${heardPasses.join(" ")})`, heardPasses.join(" "));
    // and again: STOP and PLAY start the count from the top
    const again2 = passes(every * 3);
    check(again2.join() === want.join(), `${name}: the same after STOP and PLAY`, again2.join(" "));
  }
  m.chordTurn(ENC.KNOB1, -20);
  m.run(200);

  // ---- the one history, in your order
  m.tap(BUTTON.HOME, 90);
  const kitLine = () => /KIT (.*)/.exec(m.text())?.[1];
  m.tap(BUTTON.ENV, 90); m.run(900);
  const kitA = kitLine();
  m.tap(BUTTON.SEQ, 90);
  const gridA = m.grid().join();
  m.tap(BUTTON.ARP, 90); m.run(300);
  const gridB = m.grid().join();
  m.tap(BUTTON.EDIT, 90); m.run(200);
  selectVoice(m, 0);
  const soundA = soundValues(m);
  m.turn(ENC.KNOB2, 5);
  const soundB = soundValues(m);
  m.run(1800);
  check(soundA.join() !== soundB.join(), "a sound edited");
  m.chord(BUTTON.SAVE, BUTTON.OCTDN); m.run(400);
  shows(m, "UNDO", "UNDO");
  check(soundValues(m).join() === soundA.join(), "undo: the sound", soundValues(m));
  m.tap(BUTTON.SEQ, 90);
  m.chord(BUTTON.SAVE, BUTTON.OCTDN); m.run(400);
  check(m.grid().join() === gridA, "undo again: the pattern, and the kit stays", m.grid().join("\n"));
  check(m.text().includes("PATTERN"), "the chord opened no other page", m.text());
  m.chord(BUTTON.SAVE, BUTTON.OCTUP); m.run(400);
  shows(m, "REDO", "REDO");
  check(m.grid().join() === gridB, "redo: the same pattern exactly");
  m.tap(BUTTON.EDIT, 90); m.run(200);
  m.chord(BUTTON.SAVE, BUTTON.OCTUP); m.run(400);
  check(soundValues(m).join() === soundB.join(), "redo: the sound", soundValues(m));
  m.chord(BUTTON.SAVE, BUTTON.OCTUP);
  shows(m, "NOTHING TO REDO", "and then nothing");
  m.run(1400);
  let levels = 0;
  for (; levels < 900 && !m.text().includes("NOTHING TO UNDO"); levels++) m.chord(BUTTON.SAVE, BUTTON.OCTDN, 30);
  shows(m, "NOTHING TO UNDO", `all the way back, ${levels} actions`);
  m.run(1400);
  for (let i = 0; i < levels + 2; i++) m.chord(BUTTON.SAVE, BUTTON.OCTUP, 30);
  shows(m, "NOTHING TO REDO", "and all the way forward");
  m.tap(BUTTON.HOME, 90);
  m.run(1500);
  check(kitLine() === kitA + "+", `and the kit is what it was (${kitLine()})`, m.text());
  m.tap(BUTTON.SEQ, 90);
  check(m.text().includes("PAGE 1/1"), "the length too", m.text());
  // a chord is not a tap
  for (const b of [BUTTON.OCTDN, BUTTON.OCTUP, BUTTON.ENV, BUTTON.ARP, BUTTON.LFO]) {
    m.chord(BUTTON.SAVE, b);
    m.run(300);
    check(!m.text().includes("PROJECTS"), `SAVE and button ${b} opens no save page`, m.text());
  }

  // ---- the worst it can be asked: 64 dense steps of variations, at the fastest tempo, with hits
  m.chordTurn(ENC.KNOB3, 3);
  m.turn(ENC.KNOB2, 180);
  setting(m, ROW.DENSITY, 100, BUTTON.SEQ);
  setting(m, ROW.COMPLEX, 100, BUTTON.SEQ);
  m.tap(BUTTON.ARP, 90);
  m.run(300);
  m.tap(BUTTON.REC, 60);
  m.tap(BUTTON.PLAY);
  systemPage(m);
  for (let i = 0; i < 180; i++) {
    m.tap(KEY(i % 27), 12);
    if (i % 40 === 0) m.turn(ENC.KNOB1, 0);
  }
  m.run(700);
  const sys = m.text();
  const mean = /CPU MEAN +([\d.]+)%/.exec(sys), peak = /CPU PEAK +([\d.]+)%/.exec(sys);
  console.log(`     dense variations: CPU mean ${mean?.[1]} %, peak ${peak?.[1]} %`);
  check(Number(peak?.[1]) < 60, `dense variations with hits over them: peak ${peak?.[1]} %`, sys);
  check(/OVERRUNS +0\b/.test(sys) && /DROPPED +0\b/.test(sys), "no overruns or dropped events", sys);
  m.tap(BUTTON.REC, 60);
  m.tap(BUTTON.PLAY);
  m.run(1800);
  setting(m, ROW.DENSITY, -100, BUTTON.SEQ);
  setting(m, ROW.COMPLEX, -100, BUTTON.SEQ);
  m.turn(ENC.KNOB2, -180);
  m.chordTurn(ENC.KNOB3, -3);
  m.tap(BUTTON.HOME, 90);
}

async function sequencer(m) {
  m.tap(BUTTON.SEQ, 90);
  m.run(300);
  shows(m, "PATTERN", "SEQ opens PATTERN");
  shows(m, "STOP", "stopped until PLAY");
  shows(m, "120 BPM");
  const first = m.grid();
  const host = hostPattern(1, 50, 30, 0);
  check(m.gridHits() > 8, `PATTERN shows a grid (${m.gridHits()} hits)`, first.join("\n"));
  check(first.join() === host.join(), "the first pattern is the host's seed 1, bit for bit", `${first.join("\n")}\nhost\n${host.join("\n")}`);
  m.png(`${out}/seq-1-pattern.png`);

  // ---- PLAY, a repeating rhythm, a moving playhead
  m.takePeak();
  m.tap(BUTTON.PLAY);
  m.run(100);
  shows(m, "PLAY", "PLAY shows the transport running");
  const heads = new Set();
  for (let i = 0; i < 40; i++) { m.run(47); heads.add(m.playhead()); }
  check(heads.size >= 12 && !heads.has(-1), `the playhead advances (${heads.size} positions of 16)`, [...heads].join());
  check(m.takePeak() > LOUD, "PLAY starts the rhythm");
  m.png(`${out}/seq-2-playing.png`);
  const loop = (RATE * 15 * 16) / 120;
  const heard = m.listen(4500);
  const period = heard.filter((v) => v - heard[0] < loop - 300);
  const again = period.filter((v) => heard.some((w) => near(w, v + loop, 140)));
  // overlapping voices can hide an onset from this detector, so most, not all
  check(period.length >= 3 && again.length >= period.length * 0.85, `the pattern repeats every ${loop} samples (${again.length} of ${period.length} onsets recur a loop later)`, heard.join());

  // ---- STOP
  m.tap(BUTTON.PLAY);
  m.run(100);
  shows(m, "STOP", "PLAY again stops");
  m.run(1800);
  check(m.playhead() === -1, "the playhead leaves on STOP");
  check(m.listen(1500).length === 0, "nothing plays after STOP");
  m.takePeak();
  m.run(500);
  check(m.takePeak() < SILENT, "silent after the voices ring out");

  // ---- step entry, with the controls alone: clear the grid, write a pulse
  const setRow = (t, row) => { // stopped: REC on, the cursor on KNOB4, the track's key
    const was = m.grid()[t];
    for (let s = 0; s < STEPS; s++) if (was[s] !== row[s]) { moveStep(m, s); m.tap(NOTE(DRUM_NOTE[t]), 30); }
    m.run(300);
  };
  const OFF = "0".repeat(STEPS);
  m.tap(BUTTON.REC, 60);
  for (let t = 0; t < TRACKS; t++) setRow(t, OFF);
  check(m.gridHits() === 0, "REC and the keys clear every step, by the controls alone", m.grid().join("\n"));
  // timing is measured on the closed hat: short, so each onset stands clear of the last
  setRow(5, "1000100010001000");
  check(m.grid()[5] === "1000100010001000", "REC and a key write steps", m.grid().join("\n"));
  m.png(`${out}/seq-3-edited.png`);
  m.run(1500);
  m.tap(BUTTON.PLAY);
  const quarter = gaps(m.listen(3000));
  check(quarter.length >= 4, `four hits a bar are heard (${quarter.length + 1} in 3 s)`, quarter.join());
  check(near(mean(quarter), (RATE * 60) / 120, 64), `120 BPM measured: a beat is ${mean(quarter).toFixed(1)} samples (${(RATE * 60) / 120} exact)`, quarter.join());
  check(quarter.every((g) => near(g, (RATE * 60) / 120, 130)), `no beat is off by more than the 128-frame output block (${Math.min(...quarter)} to ${Math.max(...quarter)})`, quarter.join());

  // an edit between takes, heard
  m.tap(BUTTON.PLAY);
  m.run(1800);
  setRow(5, "1010101010101010");
  m.tap(BUTTON.PLAY);
  m.run(600);
  const eighths = gaps(m.listen(2500));
  check(near(mean(eighths), (RATE * 30) / 120, 64), `steps entered are heard: eighths, ${mean(eighths).toFixed(1)} samples`, eighths.join());

  // ---- tempo: KNOB2, heard
  m.turn(ENC.KNOB2, 60);
  shows(m, "180 BPM", "KNOB2 sets the tempo");
  m.run(300);
  const fast = gaps(m.listen(2500));
  check(near(mean(fast), (RATE * 30) / 180, 64), `180 BPM measured: ${mean(fast).toFixed(1)} samples an eighth (${(RATE * 30) / 180} exact)`, fast.join());
  m.turn(ENC.KNOB2, -60);

  // ---- swing: KNOB3, heard between each beat and the sixteenth after it
  m.tap(BUTTON.PLAY);
  m.run(1800);
  setRow(5, "1100110011001100");
  m.tap(BUTTON.PLAY);
  m.run(600);
  const pair = (RATE * 15 * 2) / 120;
  const even = gaps(m.listen(2500)).filter((g) => g < pair);
  check(near(mean(even), pair * 0.5, 64), `straight sixteenths measured: ${mean(even).toFixed(1)} samples (${pair * 0.5} exact)`, even.join());
  m.turn(ENC.KNOB3, 16);
  shows(m, "SWG66", "KNOB3 sets the swing");
  m.run(600);
  const swung = gaps(m.listen(3000)).filter((g) => g < pair);
  check(swung.length >= 3 && near(mean(swung), pair * 0.66, 64), `swing 66 measured: ${mean(swung).toFixed(1)} samples (${(pair * 0.66).toFixed(1)} exact)`, swung.join());
  m.turn(ENC.KNOB3, -16);

  // ---- GENERATE, MUTATE, undo: while it plays
  m.tap(BUTTON.REC, 60);
  const hand = m.grid();
  m.tap(BUTTON.ARP, 90);
  m.run(400);
  const generated = m.grid();
  check(generated.join() !== hand.join(), "ARP generates a new pattern");
  check(generated.join() === hostPattern(2, 50, 30, 0).join(), "ARP is the next seed's pattern, bit for bit", generated.join("\n"));
  m.tap(BUTTON.GLO, 90);
  m.turn(ENC.KNOB1, ROW.SEED);
  m.run(300);
  shows(m, "SEED 2", "GLOBAL shows the generator's settings");
  m.png(`${out}/seq-4-generator.png`);
  m.tap(BUTTON.SEQ, 90);
  setting(m, ROW.DENSITY, 40, BUTTON.SEQ);
  setting(m, ROW.COMPLEX, 70, BUTTON.SEQ);
  setting(m, ROW.SEED, 40, BUTTON.SEQ);
  m.turn(ENC.ALGORITHM, 8);
  shows(m, "STYLE GLITCH", "ALGORITHM chooses the style");
  m.tap(BUTTON.ARP, 90);
  m.run(400);
  check(m.grid().join() === hostPattern(42, 90, 100, 8).join(), "DENSITY, COMPLEXITY, SEED and style all reach the generator", m.grid().join("\n"));
  m.chord(BUTTON.SAVE, BUTTON.OCTDN);
  m.run(300);
  check(m.grid().join() === generated.join(), "SAVE+OCT- undoes it");
  const mutated = [];
  let unrelated = 0;
  for (let i = 0; i < 5; i++) {
    const was = m.grid().join("");
    m.tap(BUTTON.LFO, 90);
    m.run(300);
    const now = m.grid().join("");
    let diff = 0;
    for (let c = 0; c < now.length; c++) diff += was[c] !== now[c];
    mutated.push(diff);
    unrelated += diff > 30;
  }
  check(mutated.some((d) => d > 0), `LFO mutates the pattern (${mutated.join(" ")} steps changed)`);
  check(unrelated === 0, "and leaves it recognisable", mutated.join(" "));
  m.png(`${out}/seq-5-mutated.png`);
  shows(m, "PLAY", "still playing through all of it");
  check(m.listen(2000).length > 0, "and it is heard");

  // ---- the other pages, with the sequence running
  m.tap(BUTTON.HOME, 90);
  m.run(300);
  shows(m, "PLAY", "MAIN shows the transport");
  shows(m, "TEMPO 120 BPM", "MAIN shows the tempo");
  const seqHits = /SEQ HITS +(\d+)/.exec(m.text());
  m.run(1000);
  const seqHitsLater = /SEQ HITS +(\d+)/.exec(m.text());
  check(seqHits && seqHitsLater && Number(seqHitsLater[1]) > Number(seqHits[1]), `MAIN shows pattern activity (${seqHits?.[1]} to ${seqHitsLater?.[1]})`, m.text());
  check(m.playhead(161) >= 0, "MAIN shows the playhead");
  m.png(`${out}/seq-6-main.png`);
  m.tap(BUTTON.EDIT, 90);
  m.run(300);
  const sound = m.text();
  m.turn(ENC.KNOB2, -6);
  check(m.text() !== sound, "SOUND edits while the sequence plays", m.text());
  m.turn(ENC.KNOB2, 6);
  m.tap(BUTTON.FX, 90);
  m.run(300);
  const mixer = m.text();
  m.turn(ENC.KNOB1, -8);
  check(m.text() !== mixer, "MIXER edits while the sequence plays", m.text());
  m.turn(ENC.KNOB1, 8);
  m.tap(BUTTON.HOME, 90);
  m.run(200);
  m.midi(0x99, 38, 120);
  m.run(90);
  shows(m, "MIDI NOTE 38 SNARE", "MIDI still triggers over the sequence");
  systemPage(m);
  m.run(800);
  const sys = m.text();
  check(/OVERRUNS +0\b/.test(sys) && /DROPPED +0\b/.test(sys), "SYSTEM: no overruns or dropped events with the sequence", sys);
  m.tap(BUTTON.PLAY);
  m.run(1800);
  m.tap(BUTTON.HOME, 90);
  m.run(300);
  // what the generator's settings were, for what follows
  m.turn(ENC.ALGORITHM, -8);
  setting(m, ROW.DENSITY, -40, BUTTON.HOME);
  setting(m, ROW.COMPLEX, -70, BUTTON.HOME);
  setting(m, ROW.SEED, 1 - 42, BUTTON.HOME); // ARP's next is seed 2
  m.tap(BUTTON.SEQ, 90);
  m.tap(BUTTON.ARP, 90);
  m.tap(BUTTON.HOME, 90);
}

async function main() {
  const pkg = readFileSync(fwsc);
  console.log(`${fwsc} at ${MHZ} MHz, emulated`);

  // ---- start-up
  let m = await new Machine().boot(pkg);
  m.x.fm1_pot(1023);
  m.run(700);
  shows(m, "CLATTER POCKET", "start screen names CLATTER POCKET");
  shows(m, "LXR DRUM MACHINE");
  shows(m, "HOLD HOME NOW FOR SAFE MODE");
  shows(m, "UNVERIFIED", "start screen says UNVERIFIED");
  m.png(`${out}/1-start.png`);
  m.run(1800);
  shows(m, "CLATTER POCKET", "CLATTER POCKET after start-up");
  shows(m, "KEYS: DRUM VEL 127", "the engine started: the status line shows the keyboard");
  shows(m, "MIDI WAITING");
  check(!/UNTITLED|PRESET|TRACK/.test(m.text()), "no stock instrument text", m.text());
  check(m.takePeak() < SILENT, "silent until something plays");
  m.png(`${out}/2-main.png`);

  // ---- pages
  for (const [button, page, sees] of [["SEQ", "PATTERN", "120 BPM"], ["EDIT", "SOUND", "PITCH"], ["FX", "MIXER", "LEVEL"], ["GLO", "GLOBAL", "OCTAVE"], ["GLO", "SYSTEM", "CPU MEAN"], ["HOME", "MAIN", "TEMPO 120 BPM"]]) {
    m.tap(BUTTON[button], 90);
    m.run(150);
    shows(m, page, `${button} opens ${page}`);
    shows(m, sees);
    m.png(`${out}/page-${page.toLowerCase()}.png`);
  }
  m.tap(BUTTON.OCTUP, 90); m.run(150);
  shows(m, "TEMPO 120 BPM", "OCT+ no longer changes the page");
  m.tap(BUTTON.OCTDN, 90); m.run(150);
  shows(m, "TEMPO 120 BPM", "nor does OCT-");
  m.run(1400);
  m.tap(BUTTON.OCTDN, 90); m.run(150); // what they did do: back to 127, the first check's
  m.tap(BUTTON.OCTUP, 90); m.tap(BUTTON.OCTUP, 90); m.run(150);

  // ---- voice selection and triggering
  for (const name of ["DRUM 2", "DRUM 3", "SNARE", "CYMBAL", "CL HAT", "OP HAT", "DRUM 1"]) {
    m.turn(ENC.SELECT, 1);
    shows(m, `VOICE ${name}`, `SELECT reaches ${name}`);
  }
  m.takePeak();
  m.tap(KEY(9));
  m.run(100);
  const played = m.takePeak();
  check(played > LOUD, `a key sounds (peak ${played.toFixed(3)})`);
  m.run(1500);
  m.takePeak();
  m.tap(KEY(10));
  m.run(100);
  check(m.takePeak() > LOUD, "another key sounds");
  m.run(600);
  shows(m, "HITS 2", "the hit counter follows");
  m.run(1500);

  // ---- parameter editing
  m.tap(BUTTON.EDIT, 90);
  m.run(150);
  const before = m.text();
  m.turn(ENC.KNOB1, 5);
  m.run(200);
  const after = m.text();
  check(before !== after, "KNOB1 changes the shown pitch", after);
  m.png(`${out}/sound-edited.png`);

  // ---- MIDI in (DIN/TRS): note 38 is the snare, on any channel
  m.tap(BUTTON.HOME, 90);
  m.run(600);
  m.takePeak();
  m.midi(0x99, 38, 120);
  m.run(90);
  shows(m, "MIDI NOTE 38 SNARE", "MIDI names the note and the voice");
  m.png(`${out}/midi-hit.png`);
  m.run(100);
  check(m.takePeak() > LOUD, "MIDI note-on sounds");
  m.midi(0x90, 36, 100);
  m.run(250);
  shows(m, "MIDI NOTE 36 DRUM 1", "another MIDI voice");

  // ---- diagnostics
  systemPage(m);
  m.run(800);
  const first = m.text();
  shows(m, "0.6.0 UNVERIFIED", "SYSTEM shows the version, UNVERIFIED");
  const heap = /HEAP +(\d+) LINKED/.exec(first);
  check(heap && Number(heap[1]) >= 49152, `SYSTEM shows the heap (${heap?.[1]})`, first);
  const cpu = /CPU MEAN +([\d.]+)%/.exec(first), peak = /CPU PEAK +([\d.]+)%/.exec(first);
  check(cpu && Number(cpu[1]) > 0, `CPU mean is measured (${cpu?.[1]} %)`, first);
  check(peak && Number(peak[1]) >= Number(cpu?.[1]), `CPU peak (${peak?.[1]} %)`, first);
  shows(m, "OVERRUNS", "SYSTEM shows overruns");
  check(/OVERRUNS +0\b/.test(first) && /DROPPED +0\b/.test(first), "no overruns or dropped events at 240 MHz", first);
  m.png(`${out}/page-system-live.png`);

  await sequencer(m);
  await performance(m);
  await longPatterns(m);

  // ---- a long run: the pattern playing, hits through it, no reset (the start screen would come back)
  let resets = 0;
  m.tap(BUTTON.PLAY);
  for (let s = 0; s < 20; s++) {
    m.tap(KEY(12));
    m.midi(0x99, 42, 100);
    m.run(880);
    if (m.text().includes("LXR DRUM MACHINE")) resets++;
  }
  check(resets === 0, "20 s of the sequence and hits without a reset");
  shows(m, "CLATTER POCKET", "still running after 20 s");
  systemPage(m);
  m.run(700);
  const longRun = m.text();
  check(/OVERRUNS +0\b/.test(longRun) && /DROPPED +0\b/.test(longRun), "no overruns or dropped events in the long run", longRun);
  const cpuPlaying = /CPU MEAN +([\d.]+)%/.exec(longRun), peakPlaying = /CPU PEAK +([\d.]+)%/.exec(longRun);
  console.log(`     playing: CPU mean ${cpuPlaying?.[1]} %, peak ${peakPlaying?.[1]} %`);
  m.tap(BUTTON.PLAY);

  await storage(pkg);

  // ---- SAFE MODE: HOME held at the start point
  m = await new Machine().boot(pkg);
  m.x.fm1_pot(1023);
  m.run(300);
  m.x.fm1_key(BUTTON.HOME, 1);
  m.run(2200);
  m.x.fm1_key(BUTTON.HOME, 0);
  m.run(400);
  shows(m, "SAFE MODE - ENGINE OFF", "HOME at start-up: SAFE MODE");
  shows(m, "CLATTER POCKET");
  m.png(`${out}/3-safe-mode.png`);
  m.takePeak();
  m.tap(BUTTON.PLAY);
  m.midi(0x90, 36, 100);
  for (const b of [BUTTON.ENV, BUTTON.ARP, BUTTON.LFO, BUTTON.SCL, BUTTON.REC, KEY(7)]) m.tap(b);
  m.turn(ENC.PRESETS, 3);
  m.run(300);
  check(m.takePeak() < SILENT, "SAFE MODE stays silent: the engine is off");
  shows(m, "SAFE MODE - ENGINE OFF", "controls do not leave SAFE MODE");
  check(m.listen(1500).length === 0, "PLAY starts no sequence in SAFE MODE");

  // ---- negative control: this script must be able to tell the stock firmware from ours
  if (stock) {
    m = await new Machine().boot(readFileSync(stock));
    m.run(3000);
    check(!m.text().includes("CLATTER POCKET"), "the stock package does NOT read as CLATTER POCKET", m.text());
    m.png(`${out}/stock-control.png`);
  }
  console.log(`emulator: ${checks} checks, ${failures} failed; screens in ${out}/`);
  process.exit(failures ? 1 : 0);
}
main().catch((e) => { console.error(e); process.exit(1); });
