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
const PAPER = 0xffff, ACCENT = 0xfbe0;
const TRACKS = 7, STEPS = 16;

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
  // The emulator delivers detents over time: wait for them.
  turn(enc, clicks) { this.x.fm1_enc(enc, clicks); this.run(120 + 10 * Math.abs(clicks)); }
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
    if (fb[0] === 0) for (const [x, y, s] of [[36, 56, 3], [56, 84, 2], [8, 136, 2], [8, 152, 2], [8, 200, 2], [8, 216, 2]]) lines.push(read(x, y, s));
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

  // ---- step edits, with the controls alone: clear the grid, write a pulse
  let track = 0, step = 0;
  const go = (t, s) => { moveTo(m, ENC.SELECT, track, t); moveTo(m, ENC.ALGORITHM, step, s); track = t; step = s; };
  const setRow = (t, row) => {
    const was = m.grid()[t];
    for (let s = 0; s < STEPS; s++) if (was[s] !== row[s]) { go(t, s); m.tap(BUTTON.REC, 30); }
    m.run(300);
  };
  const OFF = "0".repeat(STEPS);
  for (let t = 0; t < TRACKS; t++) setRow(t, OFF);
  check(m.gridHits() === 0, "REC clears every step, by the controls alone", m.grid().join("\n"));
  // timing is measured on the closed hat: short, so each onset stands clear of the last
  setRow(5, "1000100010001000");
  check(m.grid()[5] === "1000100010001000", "REC writes steps", m.grid().join("\n"));
  m.png(`${out}/seq-3-edited.png`);
  m.tap(BUTTON.PLAY);
  const quarter = gaps(m.listen(3000));
  check(quarter.length >= 4, `four hits a bar are heard (${quarter.length + 1} in 3 s)`, quarter.join());
  check(near(mean(quarter), (RATE * 60) / 120, 64), `120 BPM measured: a beat is ${mean(quarter).toFixed(1)} samples (${(RATE * 60) / 120} exact)`, quarter.join());
  check(quarter.every((g) => near(g, (RATE * 60) / 120, 130)), `no beat is off by more than the 128-frame output block (${Math.min(...quarter)} to ${Math.max(...quarter)})`, quarter.join());

  // a live edit, heard while it runs
  setRow(5, "1010101010101010");
  m.run(600);
  const eighths = gaps(m.listen(2500));
  check(near(mean(eighths), (RATE * 30) / 120, 64), `steps edited while playing are heard: eighths, ${mean(eighths).toFixed(1)} samples`, eighths.join());

  // ---- tempo: KNOB2, heard
  m.turn(ENC.KNOB2, 60);
  shows(m, "180 BPM", "KNOB2 sets the tempo");
  m.run(300);
  const fast = gaps(m.listen(2500));
  check(near(mean(fast), (RATE * 30) / 180, 64), `180 BPM measured: ${mean(fast).toFixed(1)} samples an eighth (${(RATE * 30) / 180} exact)`, fast.join());
  m.turn(ENC.KNOB2, -60);

  // ---- swing: KNOB3, heard between each beat and the sixteenth after it
  setRow(5, "1100110011001100");
  m.run(600);
  const pair = (RATE * 15 * 2) / 120;
  const even = gaps(m.listen(2500)).filter((g) => g < pair);
  check(near(mean(even), pair * 0.5, 64), `straight sixteenths measured: ${mean(even).toFixed(1)} samples (${pair * 0.5} exact)`, even.join());
  m.turn(ENC.KNOB3, 16);
  shows(m, "SWING 66", "KNOB3 sets the swing");
  m.run(600);
  const swung = gaps(m.listen(3000)).filter((g) => g < pair);
  check(swung.length >= 3 && near(mean(swung), pair * 0.66, 64), `swing 66 measured: ${mean(swung).toFixed(1)} samples (${(pair * 0.66).toFixed(1)} exact)`, swung.join());
  m.turn(ENC.KNOB3, -16);

  // ---- GENERATE, MUTATE, undo: while it plays
  const hand = m.grid();
  m.tap(BUTTON.ARP, 90);
  m.run(400);
  const generated = m.grid();
  check(generated.join() !== hand.join(), "ARP generates a new pattern");
  check(generated.join() === hostPattern(2, 50, 30, 0).join(), "ARP is the next seed's pattern, bit for bit", generated.join("\n"));
  m.tap(BUTTON.SCL, 90);
  m.run(300);
  shows(m, "SEED2", "SCL shows the generator's knobs");
  m.png(`${out}/seq-4-generator.png`);
  m.turn(ENC.KNOB1, 8);
  m.turn(ENC.KNOB2, 14);
  m.turn(ENC.KNOB3, 40);
  m.turn(ENC.KNOB4, 8);
  shows(m, "GLITCH", "KNOB4 chooses the style");
  m.tap(BUTTON.ARP, 90);
  m.run(400);
  check(m.grid().join() === hostPattern(42, 90, 100, 8).join(), "DENSITY, COMPLEXITY, SEED and style all reach the generator", m.grid().join("\n"));
  m.tap(BUTTON.ENV, 90);
  m.run(300);
  check(m.grid().join() === generated.join(), "ENV undoes it");
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
  m.tap(BUTTON.SCL, 90);
  m.run(300);
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
  m.tap(BUTTON.GLO, 90);
  m.run(800);
  const sys = m.text();
  check(/OVERRUNS +0\b/.test(sys) && /DROPPED +0\b/.test(sys), "SYSTEM: no overruns or dropped events with the sequence", sys);
  m.tap(BUTTON.PLAY);
  m.run(1800);
  m.tap(BUTTON.HOME, 90);
  m.run(300);
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
  shows(m, "LXR ENGINE READY", "the engine started");
  shows(m, "MIDI WAITING");
  check(!/UNTITLED|PRESET|TRACK/.test(m.text()), "no stock instrument text", m.text());
  check(m.takePeak() < SILENT, "silent until something plays");
  m.png(`${out}/2-main.png`);

  // ---- pages
  for (const [button, page, sees] of [["SEQ", "PATTERN", "120 BPM"], ["EDIT", "SOUND", "PITCH"], ["FX", "MIXER", "LEVEL"], ["GLO", "SYSTEM", "CPU MEAN"], ["HOME", "MAIN", "TEMPO 120 BPM"]]) {
    m.tap(BUTTON[button], 90);
    m.run(150);
    shows(m, page, `${button} opens ${page}`);
    shows(m, sees);
    m.png(`${out}/page-${page.toLowerCase()}.png`);
  }
  m.tap(BUTTON.OCTUP, 90); m.run(150);
  shows(m, "PATTERN", "OCT+ steps to PATTERN");
  m.tap(BUTTON.OCTDN, 90); m.run(150);
  shows(m, "TEMPO 120 BPM", "OCT- steps back");
  m.tap(BUTTON.OCTDN, 90); m.run(150);
  shows(m, "SYSTEM", "OCT- wraps to SYSTEM");
  m.tap(BUTTON.HOME, 90); m.run(150);

  // ---- voice selection and triggering
  for (const name of ["DRUM 2", "DRUM 3", "SNARE", "CYMBAL", "CL HAT", "OP HAT", "DRUM 1"]) {
    m.turn(ENC.SELECT, 1);
    shows(m, `VOICE ${name}`, `SELECT reaches ${name}`);
  }
  m.takePeak();
  m.tap(KEY(12));
  m.run(100);
  const played = m.takePeak();
  check(played > LOUD, `the programmed-pitch key sounds (peak ${played.toFixed(3)})`);
  m.run(1500);
  m.takePeak();
  m.tap(KEY(13));
  m.run(100);
  check(m.takePeak() > LOUD, "a note key sounds");
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
  m.tap(BUTTON.GLO, 90);
  m.run(800);
  const first = m.text();
  shows(m, "0.3.0 UNVERIFIED", "SYSTEM shows the version, UNVERIFIED");
  const heap = /HEAP +(\d+) LINKED/.exec(first);
  check(heap && Number(heap[1]) >= 49152, `SYSTEM shows the heap (${heap?.[1]})`, first);
  const cpu = /CPU MEAN +([\d.]+)%/.exec(first), peak = /CPU PEAK +([\d.]+)%/.exec(first);
  check(cpu && Number(cpu[1]) > 0, `CPU mean is measured (${cpu?.[1]} %)`, first);
  check(peak && Number(peak[1]) >= Number(cpu?.[1]), `CPU peak (${peak?.[1]} %)`, first);
  shows(m, "OVERRUNS", "SYSTEM shows overruns");
  check(/OVERRUNS +0\b/.test(first) && /DROPPED +0\b/.test(first), "no overruns or dropped events at 240 MHz", first);
  m.png(`${out}/page-system-live.png`);

  await sequencer(m);

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
  m.tap(BUTTON.GLO, 90);
  m.run(700);
  const longRun = m.text();
  check(/OVERRUNS +0\b/.test(longRun) && /DROPPED +0\b/.test(longRun), "no overruns or dropped events in the long run", longRun);
  const cpuPlaying = /CPU MEAN +([\d.]+)%/.exec(longRun), peakPlaying = /CPU PEAK +([\d.]+)%/.exec(longRun);
  console.log(`     playing: CPU mean ${cpuPlaying?.[1]} %, peak ${peakPlaying?.[1]} %`);
  m.tap(BUTTON.PLAY);

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
