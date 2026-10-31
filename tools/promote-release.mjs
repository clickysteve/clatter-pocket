#!/usr/bin/env node
// Name one firmware package as the site's installable release. By hand, and only after the build
// and the emulator suite have run on it: nothing on the site changes by itself.
//
//   node tools/promote-release.mjs <emulator-suite.log> [--status experimental]
//
// It refuses unless the firmware sources are committed and unchanged since the package was built,
// the package is the one the emulator suite ran and the demo runs, it reports the version in
// ui/pocket_panel.h, it passes tools/check-firmware.py and tools/check-audio-path.py, and M-VAVE's
// own FM1Web.prepare accepts it. Then it copies it to releases/ and writes site/release.json.
//
// Copyright (C) 2026 The CLATTER contributors. SPDX-License-Identifier: GPL-3.0-only

import { execFileSync, spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { copyFileSync, existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { appFromPackage } from '../site/demo/emu/fwsc.js';
import { STATUS, refusals } from '../site/installer/gate.mjs';
import { loadFM1Web, metaFor } from './fm1-web.mjs';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const stop = text => { console.error(`promote-release: ${text}`); process.exit(1); };
const sha = bytes => createHash('sha256').update(bytes).digest('hex');
const git = (...args) => execFileSync('git', args, { cwd: root, encoding: 'utf8' }).trim();
const SOURCES = ['firmware', 'ui', 'seq', 'engine', 'lxr', 'third_party', 'tools/derive-fm1-build.py'];

const emuLog = process.argv[2];
const status = process.argv[3] === '--status' ? process.argv[4] : STATUS.EXPERIMENTAL;
if (!emuLog || !existsSync(emuLog)) stop('name the log of the emulator suite run on this package');
if (status !== STATUS.EXPERIMENTAL) stop('only experimental releases are made here: docs/WEBSITE.md has the verified route');

if (git('status', '--porcelain', '--', ...SOURCES)) stop('the firmware sources have uncommitted changes');
const commit = git('log', '-1', '--format=%h', '--', ...SOURCES);

const dir = join(root, 'build/fm1-replace');
const pkg = join(dir, 'clatter-pocket-fm1-replace-UNVERIFIED.fwsc');
const data = readFileSync(pkg);
const hash = sha(data);

const demo = JSON.parse(readFileSync(join(root, 'site/demo/emulator-build.json'), 'utf8'));
if (demo.sha256 !== hash) stop(`the demo runs ${demo.sha256.slice(0, 12)}, the build is ${hash.slice(0, 12)}: promote the build to the demo first`);
if (demo.commit !== commit) stop(`the demo's build names ${demo.commit}, the firmware sources are at ${commit}: an outdated build`);

const emu = readFileSync(emuLog, 'utf8');
if (!/emulator: \d+ checks, 0 failed/.test(emu)) stop('the emulator log does not show a clean run');
if (!emu.includes(hash)) stop('the emulator log does not name this package');

const version = /POCKET_FW_VERSION "([0-9.]+)/.exec(readFileSync(join(root, 'ui/pocket_panel.h'), 'utf8'))[1];
if (!Buffer.from(appFromPackage(new Uint8Array(data))).includes(`${version} UNVERIFIED`)) stop(`the package does not report ${version} UNVERIFIED`);

const run = (...cmd) => spawnSync(cmd[0], cmd.slice(1), { cwd: root, encoding: 'utf8' });
const firmware = run('python3', 'tools/check-firmware.py');
if (firmware.status !== 0) stop(`check-firmware.py failed:\n${firmware.stdout}`);
if (!firmware.stdout.includes('ok   built --single-precision')) stop('the package is not --single-precision');
const dis = join(root, 'build/sdk.dis');
if (!existsSync(dis)) stop('build/sdk.dis is missing: docs in tools/check-audio-path.py');
const audio = run('python3', 'tools/check-audio-path.py', 'build/sdk.dis');
if (audio.status !== 0) stop(`check-audio-path.py failed:\n${audio.stdout}`);

const identity = JSON.parse(readFileSync(join(dir, 'tree/build/dx7/build-identity.json'), 'utf8').replace(/^﻿/, ''));
const product = /FM1_UPDATE_PRODUCT "([^"]+)"/.exec(readFileSync(join(dir, 'tree/app/version.h'), 'utf8'))[1];
const named = {
  filename: `clatter-pocket-${version}-${hash.slice(0, 8)}-UNVERIFIED.fwsc`, sha256: hash, bytes: data.length,
  buildId: identity.build_id, loaderSha256: sha(readFileSync(join(root, 'third_party/fm1_open/sdk/tools/pack/ota.bin'))),
  product, family: 7,
  checks: { commit, singlePrecision: true, firmwareChecks: true, audioPath: true, emulator: true },
};
await loadFM1Web().prepare(metaFor(named, version, data)).catch(error => stop(`FM1Web.prepare refused it: ${error.message}`));

const file = join(root, 'site/release.json');
const release = {
  ...JSON.parse(readFileSync(file, 'utf8')), version, status, installEnabled: true,
  releaseDate: new Date().toISOString().slice(0, 10), firmware: named, verification: null,
};
const no = refusals(release);
if (no.length) stop(`the gate refuses this release:\n  ${no.join('\n  ')}`);
mkdirSync(join(root, 'releases', version), { recursive: true });
copyFileSync(pkg, join(root, 'releases', version, named.filename));
writeFileSync(file, JSON.stringify(release, null, 2) + '\n');
console.log(`release ${version} (${status}): ${named.filename}, commit ${commit}, SHA-256 ${hash}`);
