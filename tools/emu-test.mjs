#!/usr/bin/env node
// Runs a CLATTER Pocket package in the FM-1 emulator (joakimeriksson/fm1-emu) and checks what
// the firmware itself puts on the LCD and the audio output. No mock-up: the emulated pi32v2
// executes the package's app.bin; this script only presses keys and reads the screen.
//
//   node tools/emu-test.mjs --wasm fm1_web.wasm [--fwsc PACKAGE] [--stock STOCK.fwsc] [--out DIR]
//
// --wasm is the emulator's WebAssembly core (web/build.sh, or fm1_web.wasm from its site).
// --stock is a negative control: a stock package must fail the identity check.
// Writes a PNG of each screen to --out. Exit status 0 only if every check passed.
import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
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
    return this;
  }
  // Emulated time passes in audio blocks; frames that are not audio yet still advance the machine.
  run(ms) {
    const x = this.x, until = x.fm1_ms() + ms;
    let guard = 0;
    while (x.fm1_ms() < until) {
      const have = x.fm1_render(128, this.budget);
      const l = new Float32Array(x.memory.buffer, x.fm1_out_l(), have), r = new Float32Array(x.memory.buffer, x.fm1_out_r(), have);
      for (let i = 0; i < have; i++) this.peak = Math.max(this.peak, Math.abs(l[i]), Math.abs(r[i]));
      if (x.fm1_halted()) throw new Error(`machine halted at ${x.fm1_ms()} ms`);
      if (++guard > 5e6) throw new Error("time does not advance");
    }
  }
  takePeak() { const p = this.peak; this.peak = 0; return p; }
  tap(id, ms = 60) { this.x.fm1_key(id, 1); this.run(ms); this.x.fm1_key(id, 0); this.run(ms); }
  turn(enc, clicks) { this.x.fm1_enc(enc, clicks); this.run(120); }
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
  for (const [button, page, sees] of [["SEQ", "PATTERN", "NO SEQUENCER IN THIS BUILD"], ["EDIT", "SOUND", "PITCH"], ["FX", "MIXER", "LEVEL"], ["GLO", "SYSTEM", "CPU MEAN"], ["HOME", "MAIN", "LIVE HITS"]]) {
    m.tap(BUTTON[button], 90);
    m.run(150);
    shows(m, page, `${button} opens ${page}`);
    shows(m, sees);
    m.png(`${out}/page-${page.toLowerCase()}.png`);
  }
  m.tap(BUTTON.OCTUP, 90); m.run(150);
  shows(m, "PATTERN", "OCT+ steps to PATTERN");
  m.tap(BUTTON.OCTDN, 90); m.run(150);
  shows(m, "LIVE HITS", "OCT- steps back");
  m.tap(BUTTON.OCTDN, 90); m.run(150);
  shows(m, "SYSTEM", "OCT- wraps to SYSTEM");
  m.tap(BUTTON.HOME, 90); m.run(150);

  // ---- voice selection and triggering
  for (const name of ["DRUM 2", "DRUM 3", "SNARE", "CYMBAL", "CL HAT", "OP HAT", "DRUM 1"]) {
    m.turn(ENC.SELECT, 1);
    shows(m, `VOICE ${name}`, `SELECT reaches ${name}`);
  }
  m.takePeak();
  m.tap(BUTTON.PLAY);
  m.run(100);
  const played = m.takePeak();
  check(played > LOUD, `PLAY sounds (peak ${played.toFixed(3)})`);
  m.run(1500);
  m.takePeak();
  m.tap(KEY(13));
  m.run(100);
  check(m.takePeak() > LOUD, "a note key sounds");
  m.run(600);
  shows(m, "HITS 2", "the hit counter follows");

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
  shows(m, "VERSION 0.2.0 UNVERIFIED".replace("VERSION ", ""), "SYSTEM shows the version, UNVERIFIED");
  const heap = /HEAP +(\d+) LINKED/.exec(first);
  check(heap && Number(heap[1]) >= 49152, `SYSTEM shows the heap (${heap?.[1]})`, first);
  const cpu = /CPU MEAN +([\d.]+)%/.exec(first), peak = /CPU PEAK +([\d.]+)%/.exec(first);
  check(cpu && Number(cpu[1]) > 0, `CPU mean is measured (${cpu?.[1]} %)`, first);
  check(peak && Number(peak[1]) >= Number(cpu?.[1]), `CPU peak (${peak?.[1]} %)`, first);
  shows(m, "OVERRUNS", "SYSTEM shows overruns");
  check(/OVERRUNS +0\b/.test(first) && /DROPPED +0\b/.test(first), "no overruns or dropped events at 240 MHz", first);
  m.png(`${out}/page-system-live.png`);

  // ---- a long run: hits through it, no reset (the start screen would come back)
  let resets = 0;
  for (let s = 0; s < 20; s++) {
    m.tap(BUTTON.PLAY);
    m.midi(0x99, 42, 100);
    m.run(880);
    if (m.text().includes("LXR DRUM MACHINE")) resets++;
  }
  check(resets === 0, "20 s of hits without a reset");
  shows(m, "CLATTER POCKET", "still running after 20 s");

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
