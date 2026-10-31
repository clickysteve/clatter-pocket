#!/usr/bin/env node
// A golden capture of the synthesis, from the firmware itself: each of the seven voices played by
// MIDI in the emulated FM-1, the output recorded and hashed, aligned on its first non-silent sample.
// Two packages with the same hashes render the same audio, whatever else changed in them.
//
//   node tools/emu-golden.mjs --wasm fm1_web.wasm [--fwsc PACKAGE]
import { readFileSync } from "node:fs";
import { createHash } from "node:crypto";

const arg = (name, fallback) => {
  const i = process.argv.indexOf(name);
  return i > 0 ? process.argv[i + 1] : fallback;
};
const wasmPath = arg("--wasm", "build/fm1_web.wasm");
const pkg = readFileSync(arg("--fwsc", "build/fm1-replace/clatter-pocket-fm1-replace-UNVERIFIED.fwsc"));
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
x.fm1_jit(1, 16);
const at = x.fm1_alloc(pkg.length);
new Uint8Array(x.memory.buffer, at, pkg.length).set(pkg);
if (x.fm1_boot_fwsc(at, pkg.length, 0, 0, 240) !== 0) throw new Error("the emulator refused the package");
x.fm1_pot(1023);
const budget = 240e6 * 0.02;
function record(ms) {
  const out = [], until = ms * 44.118;
  while (out.length < until) {
    const have = x.fm1_render(128, budget);
    const l = new Float32Array(x.memory.buffer, x.fm1_out_l(), have), r = new Float32Array(x.memory.buffer, x.fm1_out_r(), have);
    for (let i = 0; i < have; i++) out.push(l[i], r[i]);
  }
  return out;
}
// The noise source runs from the moment audio starts, so to hear the same noise a note must
// arrive the same number of samples after that. The first frame the status line is on screen marks
// it, whatever the start-up took; --lead fine-tunes it, in output blocks.
const lcd = () => new Uint16Array(x.memory.buffer, x.fm1_lcd(), 240 * 240);
const started = () => lcd()[16 * 240 + 5] === 0xffff || lcd()[19 * 240 + 8] === 0xffff; // firmware before 0.6.0 was black on white
let frames = 0, guard = 0;
while (!started()) { record(2); frames += 128; if (++guard > 20000) throw new Error("no start"); }
console.error(`audio start: about ${(frames / 44.118).toFixed(0)} ms of output, ${x.fm1_ms()} ms of clock`);
for (let n = Number(arg("--lead", 0)) + 40; n > 0; n--) record(2);
record(1000);
const notes = { "DRUM 1": 36, "DRUM 2": 41, "DRUM 3": 47, SNARE: 38, CYMBAL: 49, "CL HAT": 42, "OP HAT": 46 };
const all = createHash("sha256");
for (const [voice, note] of Object.entries(notes)) {
  x.fm1_midi(0x90); x.fm1_midi(note); x.fm1_midi(112);
  const take = record(1400);
  const first = take.findIndex((v) => v !== 0);
  const bytes = Buffer.from(Float32Array.from(take.slice(first, first + 2 * 44100)).buffer);
  const hash = createHash("sha256").update(bytes).digest("hex").slice(0, 16);
  all.update(hash);
  console.log(`${voice.padEnd(7)} ${hash}  peak ${take.reduce((a, v) => Math.max(a, Math.abs(v)), 0).toFixed(4)}`);
}
console.log(`all     ${all.digest("hex").slice(0, 16)}`);
