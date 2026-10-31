#!/usr/bin/env node
// Build the website into build/site. No dependencies; Node 20 or later.
//
//   node tools/build-site.mjs [--out dir] [--release file] [--releases dir]
//
// The site/ folder is copied, its pages' markers filled in, and the licences added.
// Installation is decided here as well as in the browser (site/installer/gate.mjs): while the
// gate is closed the output holds no firmware and no code that can talk to a device, and this
// script refuses to finish if it does. installEnabled set on a release the gate refuses is an
// error, not a quiet "off". With the gate open, the named package must match its hash and be
// accepted by M-VAVE's own package checks before the site is built around it.
//
// Copyright (C) 2026 The CLATTER contributors. SPDX-License-Identifier: GPL-3.0-only

import { createHash } from 'node:crypto';
import { cpSync, existsSync, mkdirSync, readFileSync, readdirSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { dirname, join, relative } from 'node:path';
import { fileURLToPath } from 'node:url';
import { NOT_AVAILABLE, STATUS, installAllowed, installLabel, refusals, statusLabel } from '../site/installer/gate.mjs';
import { CONTROLS, controlTable, panelSvg, pixelText } from '../site/assets/controls.mjs';
import { loadFM1Web, metaFor } from './fm1-web.mjs';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const sha = bytes => createHash('sha256').update(bytes).digest('hex');
const fail = text => { console.error(`build-site: ${text}`); process.exit(1); };

function option(name, fallback) {
  const at = process.argv.indexOf(`--${name}`);
  return at > 0 ? process.argv[at + 1] : fallback;
}
const out = option('out', join(root, 'build/site'));
const releaseFile = option('release', join(root, 'site/release.json'));
const releases = option('releases', join(root, 'releases'));

const walk = dir => readdirSync(dir).flatMap(name => {
  const path = join(dir, name);
  return statSync(path).isDirectory() ? walk(path) : [path];
});

// ---------------------------------------------------------------- the release
const release = JSON.parse(readFileSync(releaseFile, 'utf8'));
const open = installAllowed(release);
if (release.installEnabled === true && !open)
  fail(`installEnabled is true but this release may not be installed:\n  ${refusals(release).join('\n  ')}`);

const build = JSON.parse(readFileSync(join(root, 'site/demo/emulator-build.json'), 'utf8'));
const emu = join(root, 'site/demo/emu');
if (sha(readFileSync(join(emu, build.file))) !== build.sha256) fail('the demo package does not match emulator-build.json');
if (sha(readFileSync(join(emu, 'fm1_web.wasm'))) !== build.emulator.wasmSha256) fail('the emulator wasm does not match emulator-build.json');

// ---------------------------------------------------------------- the site
rmSync(out, { recursive: true, force: true });
mkdirSync(out, { recursive: true });
cpSync(join(root, 'site'), out, {
  recursive: true,
  filter: path => !path.endsWith('.DS_Store') && !path.endsWith('driver.js'),
});
writeFileSync(join(out, '.nojekyll'), '');
cpSync(releaseFile, join(out, 'release.json'));   // the one the gate judged is the one served

const fill = {
  version: release.version,
  status: statusLabel(release),
  'not-available': NOT_AVAILABLE,
  'install-label': installLabel(release),
  'hardware': release.compatibility?.device ?? 'M-VAVE FM-1',
  'requires': (release.compatibility?.requires ?? []).map(r => `<li>${r.replace(/&/g, '&amp;').replace(/</g, '&lt;')}</li>`).join(''),
  'package': release.firmware?.filename ?? 'None published',
  'package-sha': release.firmware?.sha256 ?? 'none',
  'package-commit': release.firmware?.checks?.commit ?? '',
  'emu-commit': build.commit,
  'emu-version': build.version,
  'emu-sha': build.sha256,
  'emulator-commit': build.emulator.commit,
  'panel-hero': panelSvg({ lcd: 'assets/screens/seq-2-playing.png', title: 'Line drawing of the M-VAVE FM-1 front panel, its screen showing CLATTER Pocket' }),
  'panel-guide': panelSvg({ lcd: 'assets/screens/perf-1-kit.png', interactive: true, title: 'The FM-1 front panel. Choose a control from the list below' }),
  'title-wide': pixelText(['CLATTER POCKET']),
  'title-stack': pixelText(['CLATTER', 'POCKET']),
  'control-table': controlTable(),
  'control-count': String(CONTROLS.length),
};
for (const page of ['index.html', 'demo/index.html', 'installer/index.html']) {
  const path = join(out, page);
  // <!-- @if name --> this <!-- @else --> that <!-- @end -->, innermost first, so that they nest
  const when = { open, experimental: open && release.status === STATUS.EXPERIMENTAL };
  const block = /<!-- @if (\w+) -->((?:(?!<!-- @if )[\s\S])*?)(?:<!-- @else -->((?:(?!<!-- @if )[\s\S])*?))?<!-- @end -->/;
  let chosen = readFileSync(path, 'utf8');
  while (block.test(chosen)) chosen = chosen.replace(block, (all, name, yes, no = '') => {
    if (!(name in when)) fail(`${page}: unknown condition ${name}`);
    return when[name] ? yes : no;
  });
  const text = chosen.replace(/<!-- @([\w-]+) -->|\{\{([\w-]+)\}\}/g, (all, a, b) => {
    const value = fill[a ?? b];
    if (value === undefined) fail(`${page}: unknown marker ${all}`);
    return value;
  });
  writeFileSync(path, text);
}

// What M-VAVE's terms and the GPL ask to travel with a firmware.
mkdirSync(join(out, 'legal'));
const sdk = join(root, 'third_party/fm1_open');
for (const [from, to] of [
  [join(root, 'LICENSE'), 'LICENSE.txt'], [join(root, 'NOTICE.md'), 'NOTICE.txt'],
  [join(root, 'THIRD_PARTY_NOTICES.md'), 'THIRD_PARTY_NOTICES.txt'],
  [join(sdk, 'LICENSE.md'), 'fm1_open-LICENSE.txt'], [join(sdk, 'THIRD_PARTY.md'), 'fm1_open-THIRD_PARTY.txt'],
]) {
  if (!existsSync(from)) fail(`${from} is missing: is the fm1_open submodule checked out?`);
  cpSync(from, join(out, 'legal', to));
}

// ---------------------------------------------------------------- installation
if (open) {
  const fw = release.firmware;
  const file = join(releases, release.version, fw.filename);
  if (!existsSync(file)) fail(`${file} is missing`);
  const data = readFileSync(file);
  if (data.length !== fw.bytes || sha(data) !== fw.sha256) fail(`${fw.filename} does not match site/release.json`);
  await loadFM1Web().prepare(metaFor(fw, release.version, data))
    .catch(error => fail(`${fw.filename} failed M-VAVE's package checks: ${error.message}`));
  mkdirSync(join(out, 'installer/firmware'));
  cpSync(file, join(out, 'installer/firmware', fw.filename));
  cpSync(join(root, 'site/installer/driver.js'), join(out, 'installer/driver.js'));
  // M-VAVE's update code, unchanged, as tools/make-installer-page.py embeds it.
  cpSync(join(sdk, 'sdk/WebUpdate.js'), join(out, 'installer/WebUpdate.js'));
  cpSync(join(sdk, 'sdk/I18n.js'), join(out, 'installer/I18n.js'));
} else {
  const ours = walk(out).filter(path => !relative(out, path).startsWith('demo/emu'));
  for (const path of walk(out)) {
    const name = relative(out, path);
    if (/\.(fwsc|ufw)$/i.test(name)) fail(`firmware in a site with installation off: ${name}`);
  }
  for (const path of ours) {
    const name = relative(out, path);
    if (/(^|\/)(driver|WebUpdate|I18n)\.js$/.test(name)) fail(`update code in a site with installation off: ${name}`);
    if (/\.(html|js|mjs)$/.test(name) && /requestMIDIAccess\s*\(|sysex\s*:\s*true|\.fwsc["'`]/.test(readFileSync(path, 'utf8')))
      fail(`${name} can reach a device, in a site with installation off`);
  }
}

const bytes = walk(out).reduce((sum, path) => sum + statSync(path).size, 0);
console.log(`site: ${relative(root, out) || out}, ${walk(out).length} files, ${(bytes / 1048576).toFixed(1)} MB; ` +
  `${fill.status}; installation ${open ? 'ENABLED' : 'off'}`);
