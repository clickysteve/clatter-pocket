#!/usr/bin/env node
// Put one firmware package on the website's browser demo. Deliberate, by hand: the site never
// picks up a build by itself, and CI cannot build firmware.
//
//   node tools/promote-emulator-build.mjs <package.fwsc> <commit>
//
// The package goes in site/demo/emu/ under a name that is not .fwsc, for the emulator only.
// site/release.json is not touched: this says nothing about hardware.
//
// Copyright (C) 2026 The CLATTER contributors. SPDX-License-Identifier: GPL-3.0-only

import { createHash } from 'node:crypto';
import { readFileSync, writeFileSync, copyFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const emu = join(root, 'site/demo/emu');
const NAME = 'clatter-pocket-emulator-only.bin';

const [pkg, commit] = process.argv.slice(2);
if (!pkg || !/^[0-9a-f]{7,40}$/.test(commit ?? '')) {
  console.error('usage: promote-emulator-build.mjs <package.fwsc> <commit>');
  process.exit(2);
}
const sha = bytes => createHash('sha256').update(bytes).digest('hex');
const data = readFileSync(pkg);
const version = /POCKET_FW_VERSION "([0-9.]+)/.exec(readFileSync(join(root, 'ui/pocket_panel.h'), 'utf8'))[1];

copyFileSync(pkg, join(emu, NAME));
const build = {
  note: 'The firmware the browser demo runs. Emulator use only: not a release, not for flashing.',
  version, commit, file: NAME, bytes: data.length, sha256: sha(data),
  emulator: {
    repository: 'https://github.com/joakimeriksson/fm1-emu',
    commit: '93cb7ee',
    wasmSha256: sha(readFileSync(join(emu, 'fm1_web.wasm'))),
  },
};
writeFileSync(join(root, 'site/demo/emulator-build.json'), JSON.stringify(build, null, 2) + '\n');
writeFileSync(join(emu, 'firmwares.json'), JSON.stringify({
  note: "CLATTER Pocket's demo: this one entry replaces fm1-emu's catalogue. Written by tools/promote-emulator-build.mjs.",
  default: 'clatter-pocket',
  firmwares: [{
    id: 'clatter-pocket', name: 'CLATTER Pocket', version: `${version} UNVERIFIED`, author: 'The CLATTER contributors',
    license: 'GPL-3.0-only',
    about: 'A generative drum machine on the LXR sound engine. An emulator build: it has never run on a physical FM-1.',
    repo: 'https://github.com/clickysteve/clatter-pocket', package: NAME,
  }],
}, null, 2) + '\n');
console.log(`demo now runs ${version} from ${commit}: ${data.length} bytes, SHA-256 ${build.sha256}`);
