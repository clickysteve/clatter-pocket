// Tests of the website. Run with: node --test tools/test-site.mjs
//
// The safety properties first: the gate fails closed, a closed gate leaves no firmware or
// device code in the output, and a firmware file alone opens nothing. Then the pages: paths under
// /clatter-pocket/, links, the control guide against the documents.
//
// Copyright (C) 2026 The CLATTER contributors. SPDX-License-Identifier: GPL-3.0-only

import assert from 'node:assert/strict';
import { execFileSync, spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { cpSync, existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, statSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, relative, resolve } from 'node:path';
import { test } from 'node:test';
import { fileURLToPath } from 'node:url';
import { NOT_AVAILABLE, browserSupport, installAllowed, refusals, statusLabel } from '../site/installer/gate.mjs';
import { CONTROLS, GEOMETRY, panelSvg } from '../site/assets/controls.mjs';
import { serve } from './serve-site.mjs';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const tool = join(root, 'tools/build-site.mjs');
const sha = bytes => createHash('sha256').update(bytes).digest('hex');
const scratch = () => mkdtempSync(join(tmpdir(), 'clatter-site-'));
const walk = dir => readdirSync(dir).flatMap(name => {
  const path = join(dir, name);
  return statSync(path).isDirectory() ? walk(path) : [path];
});
const buildSite = (...args) => spawnSync(process.execPath, [tool, ...args], { encoding: 'utf8' });

const release = () => JSON.parse(readFileSync(join(root, 'site/release.json'), 'utf8'));
const HASH = 'a'.repeat(64);
function verified() {
  return {
    ...release(), status: 'hardware-verified', installEnabled: true, releaseDate: '2027-01-01',
    firmware: { filename: 'clatter-pocket-0.6.0.fwsc', sha256: HASH, buildId: 'b'.repeat(64), loaderSha256: 'c'.repeat(64),
      bytes: 2048, product: 'FM-1_8000014', family: 7 },
    verification: { firmwareSha256: HASH, date: '2027-01-01', units: 1, report: 'https://example.org/report', recoveryTested: true },
  };
}

// ---------------------------------------------------------------- the gate
test('the published release keeps installation off and says UNVERIFIED', () => {
  const r = release();
  assert.equal(r.installEnabled, false);
  assert.equal(r.firmware, null);
  assert.equal(installAllowed(r), false);
  assert.match(statusLabel(r), /^UNVERIFIED/);
});

test('the gate opens only for a complete, verified release', () => {
  assert.deepEqual(refusals(verified()), []);
  assert.equal(statusLabel(verified()), 'HARDWARE VERIFIED');
});

test('every single missing or wrong field closes the gate', () => {
  const breaks = [
    r => { r.status = 'experimental'; }, r => { r.status = 'emulator-only'; }, r => { r.status = undefined; },
    r => { r.installEnabled = false; }, r => { r.installEnabled = 'true'; }, r => { r.installEnabled = 1; },
    r => { r.schema = 2; }, r => { r.releaseDate = null; }, r => { r.notes = []; }, r => { r.version = ''; },
    r => { r.firmware = null; }, r => { r.firmware.filename = 'other.fwsc'; }, r => { r.firmware.filename = 'clatter-pocket-fm1-replace-UNVERIFIED.fwsc'; },
    r => { r.firmware.sha256 = 'abc'; }, r => { r.firmware.buildId = null; }, r => { r.firmware.loaderSha256 = null; }, r => { r.firmware.bytes = 12; },
    r => { r.firmware.product = 'FM-1_015'; }, r => { r.firmware.family = 6; },
    r => { r.verification = null; }, r => { r.verification.firmwareSha256 = 'd'.repeat(64); },
    r => { r.verification.date = 'soon'; }, r => { r.verification.units = 0; }, r => { r.verification.report = ''; },
    r => { r.verification.recoveryTested = false; }, r => { r.compatibility = null; }, r => { r.compatibility.requires = []; },
  ];
  breaks.forEach((change, i) => {
    const r = structuredClone(verified());
    change(r);
    assert.equal(installAllowed(r), false, `change ${i}: ${change}`);
    assert.notEqual(statusLabel(r), 'HARDWARE VERIFIED', `change ${i}`);
  });
});

test('nothing unusable as a release opens the gate', () => {
  for (const r of [null, undefined, 0, 'x', [], {}]) assert.equal(installAllowed(r), false);
});

test('a new build cannot inherit an older build\'s verification', () => {
  const r = verified();
  r.firmware.sha256 = 'e'.repeat(64);
  assert.deepEqual(refusals(r), ['the verification record is for a different package']);
});

test('browser messages follow what the browser offers', () => {
  const chrome = { isSecureContext: true, hasWebMidi: true, userAgent: 'Mozilla/5.0 (Macintosh) Chrome/130' };
  assert.equal(browserSupport(chrome).ok, true);
  assert.match(browserSupport({ ...chrome, hasWebMidi: false }).message, /no Web MIDI/);
  assert.equal(browserSupport({ ...chrome, hasWebMidi: false }).ok, false);
  assert.match(browserSupport({ ...chrome, userAgent: 'Mozilla/5.0 (Linux; Android 14) Mobile Chrome/130' }).message, /computer/);
  assert.match(browserSupport({ ...chrome, isSecureContext: false }).message, /secure/);
});

// ---------------------------------------------------------------- the build
const site = scratch();
const built = buildSite('--out', site);
const pages = ['index.html', 'demo/index.html', 'installer/index.html'];
const page = name => readFileSync(join(site, name), 'utf8');

test('the site builds with installation off', () => {
  assert.equal(built.status, 0, built.stderr);
  assert.match(built.stdout, /installation off/);
});

test('the output holds no firmware file and no update code', () => {
  const names = walk(site).map(path => relative(site, path));
  assert.deepEqual(names.filter(n => /\.(fwsc|ufw)$/i.test(n)), []);
  assert.deepEqual(names.filter(n => /(^|\/)(driver|WebUpdate|I18n)\.js$/.test(n)), []);
  assert.deepEqual(names.filter(n => n.startsWith('installer/firmware')), []);
});

test('outside the vendored emulator, nothing can reach a device', () => {
  for (const path of walk(site)) {
    const name = relative(site, path);
    if (name.startsWith('demo/emu') || !/\.(html|js|mjs)$/.test(name)) continue;
    assert.doesNotMatch(readFileSync(path, 'utf8'), /requestMIDIAccess\s*\(|sysex\s*:\s*true|FM1Web/, name);
  }
});

test('no page offers the emulator package for download', () => {
  for (const name of pages) assert.doesNotMatch(page(name), /clatter-pocket-emulator-only|download/i, name);
  const emu = page('demo/emu/firmwares.json');
  assert.doesNotMatch(emu, /"(installer|manifest|embedded|hub|download)"/);
});

test('installer controls are disabled in the page as shipped', () => {
  const html = page('installer/index.html');
  for (const id of ['connect', 'install', 'backup']) assert.match(html, new RegExp(`id="${id}"[^>]* disabled`), id);
});

test('installEnabled without verification fails the build, loudly', () => {
  const file = join(scratch(), 'release.json');
  writeFileSync(file, JSON.stringify({ ...release(), installEnabled: true }));
  const r = buildSite('--out', scratch(), '--release', file);
  assert.equal(r.status, 1);
  assert.match(r.stderr, /installEnabled is true but/);
});

test('a firmware file in the repository opens nothing', () => {
  const dir = scratch(), out = scratch();
  mkdirSync(join(dir, release().version), { recursive: true });
  writeFileSync(join(dir, release().version, 'clatter-pocket-0.5.0.fwsc'), Buffer.alloc(2048));
  const r = buildSite('--out', out, '--releases', dir);
  assert.equal(r.status, 0, r.stderr);
  assert.deepEqual(walk(out).filter(p => /\.fwsc$/.test(p)), []);
});

function verifiedFixture(tamper) {
  const dir = scratch(), bytes = Buffer.alloc(2048, 1);
  const r = verified();
  r.firmware.sha256 = r.verification.firmwareSha256 = sha(bytes);
  mkdirSync(join(dir, r.version), { recursive: true });
  writeFileSync(join(dir, r.version, r.firmware.filename), tamper ? Buffer.alloc(2048, 2) : bytes);
  const file = join(dir, 'release.json');
  writeFileSync(file, JSON.stringify(r));
  return { dir, file };
}

test('a verified release with a matching file adds firmware and update code', () => {
  const { dir, file } = verifiedFixture(false), out = scratch();
  const r = buildSite('--out', out, '--release', file, '--releases', dir);
  assert.equal(r.status, 0, r.stderr);
  assert.match(r.stdout, /installation ENABLED/);
  const pair = [join(root, 'third_party/fm1_open/sdk/WebUpdate.js'), join(out, 'installer/WebUpdate.js')];
  assert.equal(sha(readFileSync(pair[0])), sha(readFileSync(pair[1])), 'WebUpdate.js must be unchanged');
  assert.ok(existsSync(join(out, 'installer/driver.js')));
  assert.ok(existsSync(join(out, 'installer/firmware/clatter-pocket-0.6.0.fwsc')));
});

test('a file that does not match its hash fails the build', () => {
  const { dir, file } = verifiedFixture(true);
  const r = buildSite('--out', scratch(), '--release', file, '--releases', dir);
  assert.equal(r.status, 1);
  assert.match(r.stderr, /does not match/);
});

// ---------------------------------------------------------------- the driver
const { Session, packageMeta } = await import('../site/installer/driver.js');

function fakeSdk(answer) {
  const calls = [];
  class Updater {
    constructor(access, pkg, status) { this.status = status; }
    async connect(options) { calls.push(['connect', options]); return answer; }
    async install(options) { calls.push(['install', options]); }
  }
  return { FM1Web: { Updater }, calls };
}
const ui = () => { const u = { lines: [], ready: [], log: t => u.lines.push(t), phase() {}, }; u.ready = () => {}; return u; };

test('the driver never uses the updater\'s force mode', async () => {
  const { FM1Web, calls } = fakeSdk({ identity: 'FM-1_8000014', current: 'abc', same: false });
  const s = new Session(FM1Web, {}, {}, ui());
  assert.equal(await s.connect(), true);
  await s.install();
  assert.deepEqual(calls, [['connect', { force: false }], ['install', { force: false }]]);
  assert.doesNotMatch(readFileSync(join(root, 'site/installer/driver.js'), 'utf8'), /force:\s*true/);
});

test('the driver refuses firmware that is not fm1_open, and the same build', async () => {
  for (const answer of [{ identity: 'FM-1_015', migration: true }, { identity: 'X', legacy: true }, { identity: 'FM-1_8000014', same: true }]) {
    const { FM1Web, calls } = fakeSdk(answer);
    const s = new Session(FM1Web, {}, {}, ui());
    assert.equal(await s.connect(), false);
    await assert.rejects(s.install(), /No device is ready/);
    assert.equal(calls.filter(c => c[0] === 'install').length, 0);
  }
});

test('the package description has what FM1Web.prepare checks', () => {
  const meta = packageMeta(verified());
  assert.equal(meta.schema, 1);
  assert.equal(meta.family, 7);
  assert.equal(meta.url, 'firmware/clatter-pocket-0.6.0.fwsc');
  assert.equal(meta.data, undefined);
});

// ---------------------------------------------------------------- the demo
test('the demo package is the promoted build, and the emulator reads it', async () => {
  const build = JSON.parse(readFileSync(join(root, 'site/demo/emulator-build.json'), 'utf8'));
  const { fetchPackage, appFromPackage } = await import(join(root, 'site/demo/emu/fwsc.js'));
  const catalog = JSON.parse(readFileSync(join(root, 'site/demo/emu/firmwares.json'), 'utf8'));
  assert.equal(catalog.firmwares.length, 1);
  assert.equal(catalog.default, 'clatter-pocket');
  const entry = catalog.firmwares[0];
  assert.match(entry.version, /UNVERIFIED/);
  assert.doesNotMatch(entry.package, /\.fwsc$/);
  const read = async url => new Response(readFileSync(join(root, 'site/demo/emu', url)));
  const got = await fetchPackage(entry, read);
  assert.equal(sha(got.bytes), build.sha256);
  assert.ok(appFromPackage(got.bytes).length > 400000);
});

test('the demo opens CLATTER Pocket, at the FM-1\'s clock, embedded', () => {
  assert.match(page('demo/index.html'), /src="emu\/\?fw=clatter-pocket&amp;mhz=240&amp;embed=1"/);
});

test('the emulator stays under its own licence', () => {
  assert.match(readFileSync(join(root, 'site/demo/emu/LICENSE'), 'utf8'), /MIT License[\s\S]*Joakim Eriksson/);
});

// ---------------------------------------------------------------- the pages
test('every page shows the status, and the installer the exact message', () => {
  for (const name of pages) assert.match(page(name), /UNVERIFIED: EMULATOR ONLY/, name);
  assert.ok(page('index.html').includes(NOT_AVAILABLE));
  assert.ok(page('installer/index.html').includes(NOT_AVAILABLE));
  assert.equal(NOT_AVAILABLE, 'Hardware installation is not yet available. CLATTER Pocket is currently verified in the FM-1 emulator only.');
});

test('no marker is left unfilled', () => {
  for (const name of pages) assert.doesNotMatch(page(name), /\{\{|<!-- @/, name);
});

test('the hero carries the agreed words', () => {
  const html = page('index.html');
  for (const text of ['A generative drum machine hiding inside your FM-1.',
    'Seven voices of authentic LXR drum synthesis. Generative kits, evolving rhythms, live recording and melodic percussion, running directly on the M-VAVE FM-1.',
    'TRY IN BROWSER', 'INSTALL FIRMWARE', 'VIEW SOURCE ON GITHUB']) assert.ok(html.includes(text), text);
  assert.match(html, /<a class="button" href="#install">INSTALL FIRMWARE/);
});

test('MIDI: only input is claimed, and the rest is denied', () => {
  const text = page('index.html').replace(/<[^>]+>/g, ' ');
  for (const m of text.matchAll(/[^.]*MIDI (output|thru|clock)[^.]*\./gi)) assert.match(m[0], /\bno\b/i, m[0]);
  for (const m of text.matchAll(/[^.]*external clock[^.]*\./gi)) assert.match(m[0], /\bno\b/i, m[0]);
});

test('the page does not present the firmware as finished', () => {
  const text = page('index.html');
  assert.doesNotMatch(text, /production[- ]ready|stable release|safe to (flash|install)(?! yet)|download (now|firmware)/i);
});

const attrs = html => [...html.matchAll(/\s(?:href|src)="([^"]+)"/g)].map(m => m[1].replace(/&amp;/g, '&'));
const headingSlug = line => line.replace(/^#+\s*/, '').toLowerCase().replace(/[^\w\s-]/g, '').trim().replace(/\s+/g, '-');

test('internal links and anchors resolve, with no path from the domain\'s root', () => {
  for (const name of pages) {
    const html = page(name), base = dirname(join(site, name));
    for (const link of attrs(html)) {
      if (/^(https?:|data:)/.test(link)) continue;
      assert.ok(!link.startsWith('/'), `${name}: ${link} would miss /clatter-pocket/`);
      const [path, hash] = link.split('#');
      const clean = path.split('?')[0];
      const target = clean ? resolve(base, clean) : join(site, name);
      const file = target.endsWith('/') || (existsSync(target) && statSync(target).isDirectory()) ? join(target, 'index.html') : target;
      assert.ok(existsSync(file), `${name}: ${link}`);
      if (hash) assert.ok(readFileSync(file, 'utf8').includes(`id="${hash}"`), `${name}: ${link}`);
    }
  }
});

test('links into this repository name files and headings that exist', () => {
  const inRepo = /^https:\/\/github\.com\/clickysteve\/clatter-pocket\/(blob|tree)\/main\/([^#]+)(?:#(.+))?$/;
  let checked = 0;
  for (const name of pages) {
    for (const link of attrs(page(name))) {
      const m = inRepo.exec(link);
      if (!m) continue;
      const file = join(root, m[2]);
      assert.ok(existsSync(file), link);
      if (m[3]) assert.ok(readFileSync(file, 'utf8').split('\n').filter(l => l.startsWith('#')).map(headingSlug).includes(m[3]), link);
      checked++;
    }
  }
  assert.ok(checked >= 8);
});

test('only known sites are linked, and no page loads anything from elsewhere', () => {
  const hosts = new Set(['github.com', 'open.m-vave.com', 'thefirmwarehub.com']);
  for (const name of pages) {
    const html = page(name);
    for (const link of attrs(html)) if (/^https?:/.test(link)) assert.ok(hosts.has(new URL(link).host), `${name}: ${link}`);
    for (const m of html.matchAll(/<(script|link|img|iframe)\b[^>]*\s(?:src|href)="(https?:[^"]+)"/g)) assert.fail(`${name} loads ${m[2]}`);
    assert.doesNotMatch(html, /google-analytics|googletagmanager|plausible|fathom|segment\.io/);
  }
  assert.doesNotMatch(readFileSync(join(site, 'styles.css'), 'utf8'), /url\(['"]?https?:|@import/);
});

test('under /clatter-pocket/, everything a page names is served, and nothing outside it', async () => {
  const server = await serve(site, 0);
  const origin = `http://127.0.0.1:${server.address().port}`;
  try {
    assert.equal((await fetch(`${origin}/index.html`)).status, 404);
    for (const name of pages) {
      const url = `${origin}/clatter-pocket/${name.replace('index.html', '')}`;
      const res = await fetch(url);
      assert.equal(res.status, 200, url);
      for (const link of attrs(await res.text())) {
        if (/^(https?:|data:|#)/.test(link)) continue;
        const to = new URL(link, url);
        assert.equal((await fetch(to)).status, 200, to.href);
      }
    }
  } finally { server.close(); }
});

test('the release configuration is served, for the installer to read', () => {
  assert.equal(JSON.parse(page('release.json')).installEnabled, false);
  assert.match(page('installer/installer.js'), /fetch\('\.\.\/release\.json'/);
});

test('licences travel with the demo', () => {
  for (const f of ['LICENSE.txt', 'NOTICE.txt', 'THIRD_PARTY_NOTICES.txt', 'fm1_open-LICENSE.txt', 'fm1_open-THIRD_PARTY.txt'])
    assert.ok(existsSync(join(site, 'legal', f)), f);
  assert.match(page('index.html'), /not made, approved, endorsed or supported by Erica Synths or M-VAVE/);
  assert.match(page('index.html'), /modified version of the sound engine/);
});

// ---------------------------------------------------------------- the control guide
const docs = ['docs/POCKET-CONTROLS.md', 'docs/POCKET-STORAGE.md'].map(f => readFileSync(join(root, f), 'utf8')).join('\n');

test('every control is one the documents name', () => {
  for (const c of CONTROLS) if (c.id !== 'keys') assert.ok(docs.includes(c.label), c.label);
});

test('every chord is one the documents name', () => {
  const chords = CONTROLS.filter(c => c.chord).map(c => c.chord);
  assert.deepEqual(chords.sort(), ['SAVE + ARP', 'SAVE + EDIT', 'SAVE + ENV', 'SAVE + LFO', 'SAVE + OCT+', 'SAVE + OCT−', 'SAVE + SEQ']);
  for (const chord of ['SAVE+ENV', 'SAVE+ARP', 'SAVE+LFO', 'SAVE+OCT−', 'SAVE+OCT+', 'SAVE+EDIT', 'SAVE+SEQ']) assert.ok(docs.includes(chord), chord);
});

test('the brief\'s assignments hold', () => {
  const does = id => CONTROLS.find(c => c.id === id);
  assert.match(does('scl').does, /DRUM.*PITCH/);
  assert.match(does('octdown').does, /octave/);
  assert.match(does('env').does, /new kit/);
  assert.match(does('env').save, /selected voice/);
  assert.match(does('arp').does, /new pattern/);
  assert.match(does('arp').save, /selected track/);
  assert.match(does('lfo').does, /Mutate the pattern/);
  assert.match(does('lfo').save, /selected track/);
  assert.match(does('edit').save, /Saved kits/);
  assert.match(does('seq').save, /Saved patterns/);
  assert.match(does('glo').does, /SYSTEM/);
  assert.match(does('select').kind, /no push switch/);
  assert.equal(does('select').save, undefined);
});

test('the demo keys are the emulator\'s', () => {
  const main = readFileSync(join(site, 'demo/emu/main.js'), 'utf8');
  const map = /R REC, 1\.\.0 (FX SCL ENV LFO EDIT GLO HOME SAVE ARP SEQ)/.exec(main)[1].split(' ');
  const keys = [...'1234567890'];
  map.forEach((label, i) => assert.equal(CONTROLS.find(c => c.label === label).key, keys[i], label));
  assert.match(main, /const PIANO = "awsedftgyhujkolp;'"/);
  assert.match(main, /ArrowLeft: \[0, -1\], ArrowRight: \[0, 1\], ArrowDown: \[1, -1\], ArrowUp: \[1, 1\], ",": \[2, -1\], ".": \[2, 1\]/);
});

test('every control has a place on the panel, and the panel has every control', () => {
  for (const c of CONTROLS) if (c.id !== 'keys') assert.ok(GEOMETRY[c.id], c.id);
  const svg = panelSvg({ interactive: true });
  assert.equal([...svg.matchAll(/data-ctl="/g)].length, CONTROLS.length);
  assert.equal([...svg.matchAll(/class="key( blk)?"/g)].length, 27);
});

test('with no script, the whole control map is in the page', () => {
  const html = page('index.html');
  for (const c of CONTROLS) assert.ok(html.includes(`<th scope="row">${c.label}</th>`), c.label);
});

test('images have text, frames have titles, the document has one h1', () => {
  for (const name of pages) {
    const html = page(name);
    assert.equal([...html.matchAll(/<h1\b/g)].length, 1, name);
    assert.match(html, /<html lang="en">/);
    assert.match(html, /<title>[^<]+<\/title>/);
    for (const m of html.matchAll(/<img\b[^>]*>/g)) assert.match(m[0], /\balt="/, m[0]);
    for (const m of html.matchAll(/<iframe\b[^>]*>/g)) assert.match(m[0], /\btitle="/, m[0]);
  }
});

test('reduced motion is respected, and focus is visible', () => {
  const css = page('styles.css');
  assert.match(css, /prefers-reduced-motion: reduce/);
  assert.match(css, /:focus-visible/);
  assert.match(page('app.js'), /prefers-reduced-motion: reduce/);
});

const luminance = hex => {
  const [r, g, b] = [1, 3, 5].map(i => parseInt(hex.slice(i, i + 2), 16) / 255).map(c => (c <= .03928 ? c / 12.92 : ((c + .055) / 1.055) ** 2.4));
  return .2126 * r + .7152 * g + .0722 * b;
};
const contrast = (a, b) => { const [x, y] = [luminance(a), luminance(b)].sort((p, q) => q - p); return (x + .05) / (y + .05); };

test('text colours reach AA contrast on their backgrounds', () => {
  const css = page('styles.css');
  const token = name => new RegExp(`--${name}: (#[0-9a-f]{3,6})`).exec(css)[1].replace(/^#(.)(.)(.)$/, '#$1$1$2$2$3$3');
  for (const [fg, bg] of [['ink', 'desktop'], ['dim', 'desktop'], ['amber', 'desktop'], ['paper-ink', 'paper']])
    assert.ok(contrast(token(fg), token(bg)) >= 4.5, `${fg} on ${bg}`);
  assert.ok(contrast('#000000', token('amber')) >= 4.5, 'black on amber');
  assert.ok(contrast('#3c3c38', token('paper')) >= 4.5, 'readout kind');
  assert.ok(contrast(token('dim'), '#0c0c0c') >= 4.5, 'dim on a window');
});

test('the Pages workflow builds, tests and publishes the site alone', () => {
  const yml = readFileSync(join(root, '.github/workflows/deploy-pages.yml'), 'utf8');
  assert.match(yml, /node --test tools\/test-site\.mjs/);
  assert.match(yml, /node tools\/build-site\.mjs/);
  assert.match(yml, /path: build\/site/);
  assert.doesNotMatch(yml, /force|installEnabled/);
});
