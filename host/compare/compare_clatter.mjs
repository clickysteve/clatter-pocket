#!/usr/bin/env node
/**
 * Compare CLATTER Pocket's natively compiled engine with CLATTER's WebAssembly engine.
 *
 *   node host/compare/compare_clatter.mjs <clatter>/src/audio/lxr/lxr.wasm build/host/pocket_dump.bin
 *
 * Runs host/compare/script.txt on CLATTER's lxr.wasm (clatter_init(1), clatter_set_param,
 * clatter_trigger(_locked), clatter_render_block) and compares each voice's output, block by
 * block, with the dump written by dump.c from the same script.
 *
 * Both builds compile the same firmware files with the same flags and the same libm, but on
 * different targets (wasm32 and the host CPU) with different compilers, so float results may
 * differ in their last bits (docs: COMPATIBILITY.md). The report gives the error per voice
 * relative to full scale and the first block where they differ.
 *
 * Copyright (C) 2026 The CLATTER contributors. SPDX-License-Identifier: GPL-3.0-only
 */
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const [wasmPath, dumpPath] = process.argv.slice(2);
if (!wasmPath || !dumpPath) {
  console.error('usage: compare_clatter.mjs <lxr.wasm> <pocket_dump.bin>');
  process.exit(2);
}
const here = dirname(fileURLToPath(import.meta.url));
const script = readFileSync(join(here, 'script.txt'), 'utf8')
  .split('\n')
  .filter((l) => l.trim() && !l.startsWith('#'))
  .map((l) => l.trim().split(/\s+/));
const end = script.find((t) => t[0] === 'end');
const blocks = Number(end[1]);
const ops = script.filter((t) => t[0] !== 'end').map(([b, kind, ...args]) => ({ block: Number(b), kind, args: args.map(Number) }));

const { instance } = await WebAssembly.instantiate(readFileSync(wasmPath), {});
const x = instance.exports;
const B = x.clatter_block_size();
const V = x.clatter_voice_count();
x.clatter_init(1);

const pocket = new Float32Array(new Uint8Array(readFileSync(dumpPath)).buffer);
if (pocket.length !== blocks * V * 2 * B) {
  console.error(`dump has ${pocket.length} samples, expected ${blocks * V * 2 * B}`);
  process.exit(1);
}

const maxErr = new Float64Array(V);
const sumSq = new Float64Array(V);
const sigSq = new Float64Array(V);
const firstDiff = new Array(V).fill(-1);
const identical = new Array(V).fill(0);
let next = 0;
for (let b = 0; b < blocks; b++) {
  for (; next < ops.length && ops[next].block === b; next++) {
    const { kind, args } = ops[next];
    if (kind === 'param') x.clatter_set_param(args[0], args[1]);
    else if (kind === 'trig') x.clatter_trigger(args[0], args[1], args[2]);
    else x.clatter_trigger_locked(...args);
  }
  x.clatter_render_block();
  const out = new Float32Array(x.memory.buffer, x.clatter_voice_out(), V * 2 * B);
  for (let v = 0; v < V; v++) {
    let same = true;
    for (let i = 0; i < 2 * B; i++) {
      const c = out[v * 2 * B + i];
      const p = pocket[(b * V + v) * 2 * B + i];
      const e = Math.abs(c - p);
      if (e > maxErr[v]) maxErr[v] = e;
      sumSq[v] += e * e;
      sigSq[v] += c * c;
      if (c !== p) same = false;
    }
    if (same) identical[v]++;
    else if (firstDiff[v] < 0) firstDiff[v] = b;
  }
}

const names = ['DRUM 1', 'DRUM 2', 'DRUM 3', 'SNARE', 'CYMBAL', 'HIHAT'];
const db = (r) => (r > 0 ? (20 * Math.log10(r)).toFixed(1) : '-inf');
console.log(`${blocks} blocks of ${B} samples, ${V} voices`);
let worst = 0;
for (let v = 0; v < V; v++) {
  const snr = sigSq[v] > 0 ? 10 * Math.log10(sigSq[v] / Math.max(sumSq[v], 1e-30)) : Infinity;
  worst = Math.max(worst, maxErr[v]);
  console.log(
    `${names[v].padEnd(7)} identical blocks ${String(identical[v]).padStart(4)}/${blocks}` +
      `  max |error| ${db(maxErr[v]).padStart(6)} dBFS` +
      `  signal/error ${Number.isFinite(snr) ? snr.toFixed(1) + ' dB' : 'exact'}` +
      (firstDiff[v] >= 0 ? `  first difference in block ${firstDiff[v]}` : ''),
  );
}
// One int16 step is -90.3 dBFS. Differences of a few steps are float rounding; anything
// near full scale is a real divergence (a different code path, state or parameter).
const limit = 2 ** -10; // -60 dBFS
console.log(worst <= limit ? `PASS: max error ${db(worst)} dBFS <= -60 dBFS` : `FAIL: max error ${db(worst)} dBFS`);
process.exit(worst <= limit ? 0 : 1);
