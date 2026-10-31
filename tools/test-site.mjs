// Tests of the website. Run with: node --test tools/test-site.mjs
//
// The safety properties first: the gate fails closed, a closed gate leaves no firmware or
// device code in the output, and a firmware file alone opens nothing. Then the installer, with
// M-VAVE's real update code and the real package against a simulated FM-1 (fm1-update-sim.mjs).
// Then the pages: paths under /clatter-pocket/, links, the control guide against the documents.
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
import { NOT_AVAILABLE, STATUS, browserSupport, installAllowed, installLabel, refusals, statusLabel } from '../site/installer/gate.mjs';
import { CONTROLS, GEOMETRY, panelSvg } from '../site/assets/controls.mjs';
import { loadFM1Web, metaFor } from './fm1-web.mjs';
import { SimulatedFM1, simulatedAccess } from './fm1-update-sim.mjs';
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
const packageFile = () => join(root, 'releases', release().version, release().firmware.filename);
const packageBytes = () => new Uint8Array(readFileSync(packageFile()));

/** What the site says when no release is enabled. */
const closed = () => ({ ...release(), status: STATUS.EMULATOR_ONLY, installEnabled: false, firmware: null, releaseDate: null });
function verified() {
  const r = release();
  return {
    ...r, status: STATUS.HARDWARE_VERIFIED, version: '0.6.0', firmware: { ...r.firmware, filename: 'clatter-pocket-0.6.0.fwsc' },
    verification: { firmwareSha256: r.firmware.sha256, date: '2027-01-01', units: 1, report: 'https://example.org/report', recoveryTested: true },
  };
}

// ---------------------------------------------------------------- the gate
test('the published release is experimental, enabled, and says UNVERIFIED', () => {
  const r = release();
  assert.equal(r.status, STATUS.EXPERIMENTAL);
  assert.equal(r.installEnabled, true);
  assert.deepEqual(refusals(r), []);
  assert.match(r.firmware.filename, /UNVERIFIED/);
  assert.equal(r.verification, null);
  assert.equal(statusLabel(r), 'UNVERIFIED: EXPERIMENTAL');
  assert.equal(installLabel(r), 'INSTALL EXPERIMENTAL FIRMWARE');
});

test('an experimental release needs no hardware verification', () => {
  const r = release();
  r.verification = null;
  assert.equal(installAllowed(r), true);
});

test('a verified release must prove it, for this package', () => {
  assert.deepEqual(refusals(verified()), []);
  assert.equal(statusLabel(verified()), 'HARDWARE VERIFIED');
  const r = verified();
  r.verification = null;
  assert.equal(installAllowed(r), false);
  const newer = verified();
  newer.firmware.sha256 = 'e'.repeat(64);
  assert.deepEqual(refusals(newer), ['the verification record is for a different package']);
});

test('installing is never automatic: the maintainer must enable it', () => {
  for (const r of [release(), verified()]) {
    r.installEnabled = false;
    assert.equal(installAllowed(r), false);
    for (const value of ['true', 1, null, undefined]) { r.installEnabled = value; assert.equal(installAllowed(r), false); }
  }
});

test('every technical field still closes the gate', () => {
  const breaks = [
    r => { r.status = 'emulator-only'; }, r => { r.status = 'unverified'; }, r => { r.status = undefined; },
    r => { r.schema = 2; }, r => { r.releaseDate = null; }, r => { r.notes = []; }, r => { r.version = ''; },
    r => { r.firmware = null; }, r => { r.firmware.filename = 'other.fwsc'; },
    r => { r.firmware.filename = 'clatter-pocket-0.5.0.fwsc'; },  // experimental, not saying so
    r => { r.firmware.sha256 = 'abc'; }, r => { r.firmware.buildId = null; }, r => { r.firmware.loaderSha256 = null; }, r => { r.firmware.bytes = 12; },
    r => { r.firmware.product = 'FM-1_015'; }, r => { r.firmware.family = 6; },
    r => { r.firmware.checks = null; }, r => { r.firmware.checks.commit = 'nope'; }, r => { r.firmware.checks.singlePrecision = false; },
    r => { r.firmware.checks.firmwareChecks = false; }, r => { r.firmware.checks.audioPath = false; }, r => { r.firmware.checks.emulator = false; },
    r => { r.compatibility = null; }, r => { r.compatibility.requires = []; },
  ];
  breaks.forEach((change, i) => {
    const r = structuredClone(release());
    change(r);
    assert.equal(installAllowed(r), false, `change ${i}: ${change}`);
  });
});

test('a verified release cannot say UNVERIFIED, and only a verified one drops the word', () => {
  const r = verified();
  r.firmware.filename = 'clatter-pocket-0.6.0-UNVERIFIED.fwsc';
  assert.equal(installAllowed(r), false);
  assert.match(statusLabel(release()), /UNVERIFIED/);
  assert.match(statusLabel(closed()), /UNVERIFIED/);
  const broken = verified();
  broken.verification = null;
  assert.match(statusLabel(broken), /UNVERIFIED/);
});

test('nothing unusable as a release opens the gate', () => {
  for (const r of [null, undefined, 0, 'x', [], {}]) assert.equal(installAllowed(r), false);
});

test('browser messages follow what the browser offers', () => {
  const chrome = { isSecureContext: true, hasWebMidi: true, userAgent: 'Mozilla/5.0 (Macintosh) Chrome/130' };
  assert.equal(browserSupport(chrome).ok, true);
  assert.match(browserSupport({ ...chrome, hasWebMidi: false }).message, /no Web MIDI/);
  assert.equal(browserSupport({ ...chrome, hasWebMidi: false }).ok, false);
  assert.match(browserSupport({ ...chrome, userAgent: 'Mozilla/5.0 (Linux; Android 14) Mobile Chrome/130' }).message, /computer/);
  assert.match(browserSupport({ ...chrome, isSecureContext: false }).message, /secure/);
  // an insecure page has no navigator.requestMIDIAccess either: the cause is the page, not the browser
  assert.match(browserSupport({ ...chrome, isSecureContext: false, hasWebMidi: false }).message, /secure/);
});

// ---------------------------------------------------------------- the build
const site = scratch();
const built = buildSite('--out', site);
const pages = ['index.html', 'demo/index.html', 'installer/index.html'];
const page = name => readFileSync(join(site, name), 'utf8');

function closedSite() {
  const dir = scratch(), file = join(dir, 'release.json'), out = join(dir, 'site');
  writeFileSync(file, JSON.stringify(closed()));
  const result = buildSite('--out', out, '--release', file);
  return { out, result, page: name => readFileSync(join(out, name), 'utf8') };
}
const off = closedSite();

test('the site builds, with the experimental package in it', () => {
  assert.equal(built.status, 0, built.stderr);
  assert.match(built.stdout, /installation ENABLED/);
  const served = readFileSync(join(site, 'installer/firmware', release().firmware.filename));
  assert.equal(sha(served), release().firmware.sha256);
  assert.equal(served.length, release().firmware.bytes);
  assert.equal(sha(readFileSync(join(site, 'installer/WebUpdate.js'))), sha(readFileSync(join(root, 'third_party/fm1_open/sdk/WebUpdate.js'))), 'WebUpdate.js must be unchanged');
  assert.equal(sha(readFileSync(join(site, 'installer/I18n.js'))), sha(readFileSync(join(root, 'third_party/fm1_open/sdk/I18n.js'))), 'I18n.js must be unchanged');
});

test('the installer runs the same package the emulator suite ran', () => {
  const demo = JSON.parse(readFileSync(join(root, 'site/demo/emulator-build.json'), 'utf8'));
  assert.equal(release().firmware.sha256, demo.sha256);
  assert.equal(release().firmware.checks.commit, demo.commit);
  assert.equal(release().version, demo.version);
});

test('the release is not an outdated build: the firmware sources have not moved', () => {
  const commit = execFileSync('git', ['log', '-1', '--format=%h', '--', 'firmware', 'ui', 'seq', 'engine', 'lxr', 'third_party', 'tools/derive-fm1-build.py'],
    { cwd: root, encoding: 'utf8' }).trim();
  const named = release().firmware.checks.commit;
  assert.ok(commit.startsWith(named) || named.startsWith(commit), `released ${named}, sources at ${commit}: run tools/promote-release.mjs`);
  const version = /POCKET_FW_VERSION "([0-9.]+)/.exec(readFileSync(join(root, 'ui/pocket_panel.h'), 'utf8'))[1];
  assert.equal(release().version, version);
});

test('with installation off, the output holds no firmware and no update code', () => {
  assert.equal(off.result.status, 0, off.result.stderr);
  assert.match(off.result.stdout, /installation off/);
  const names = walk(off.out).map(path => relative(off.out, path));
  assert.deepEqual(names.filter(n => /\.(fwsc|ufw)$/i.test(n)), []);
  assert.deepEqual(names.filter(n => /(^|\/)(driver|WebUpdate|I18n)\.js$/.test(n)), []);
  assert.deepEqual(names.filter(n => n.startsWith('installer/firmware')), []);
  for (const path of walk(off.out)) {
    const name = relative(off.out, path);
    if (name.startsWith('demo/emu') || !/\.(html|js|mjs)$/.test(name)) continue;
    assert.doesNotMatch(readFileSync(path, 'utf8'), /requestMIDIAccess\s*\(|sysex\s*:\s*true|FM1Web/, name);
  }
});

test('with installation off, the pages say so, in the agreed words', () => {
  assert.ok(off.page('index.html').includes(NOT_AVAILABLE));
  assert.ok(off.page('installer/index.html').includes(NOT_AVAILABLE));
  assert.match(off.page('index.html'), /<a class="button" href="#install">INSTALL FIRMWARE/);
  assert.doesNotMatch(off.page('index.html'), /EXPERIMENTAL FIRMWARE/);
  assert.match(off.page('installer/index.html'), /Installing is off/);
});

test('no page offers a firmware file for download', () => {
  for (const name of pages) assert.doesNotMatch(page(name), /clatter-pocket-emulator-only|href="[^"]*\.fwsc"|download/i, name);
  assert.doesNotMatch(page('demo/emu/firmwares.json'), /"(installer|manifest|embedded|hub|download)"/);
});

test('installer controls ship disabled, until the script has checked the package', () => {
  const html = page('installer/index.html');
  for (const id of ['connect', 'install', 'confirm']) assert.match(html, new RegExp(`id="${id}"[^>]* disabled`), id);
});

test('installEnabled on a release the gate refuses fails the build, loudly', () => {
  const file = join(scratch(), 'release.json');
  writeFileSync(file, JSON.stringify({ ...closed(), installEnabled: true }));
  const r = buildSite('--out', scratch(), '--release', file);
  assert.equal(r.status, 1);
  assert.match(r.stderr, /installEnabled is true but/);
});

test('a firmware file in the repository opens nothing', () => {
  const dir = scratch(), out = scratch(), file = join(dir, 'release.json');
  mkdirSync(join(dir, release().version), { recursive: true });
  cpSync(packageFile(), join(dir, release().version, 'clatter-pocket-0.5.0-UNVERIFIED.fwsc'));
  writeFileSync(file, JSON.stringify(closed()));
  const r = buildSite('--out', out, '--release', file, '--releases', dir);
  assert.equal(r.status, 0, r.stderr);
  assert.deepEqual(walk(out).filter(p => /\.fwsc$/.test(p)), []);
});

function fixture(change, bytes = readFileSync(packageFile())) {
  const dir = scratch(), r = release();
  change?.(r);
  mkdirSync(join(dir, r.version), { recursive: true });
  writeFileSync(join(dir, r.version, r.firmware.filename), bytes);
  const file = join(dir, 'release.json');
  writeFileSync(file, JSON.stringify(r));
  return buildSite('--out', scratch(), '--release', file, '--releases', dir);
}

test('a package that does not match its hash fails the build', () => {
  const bytes = readFileSync(packageFile());
  bytes[5000] ^= 1;
  const r = fixture(null, bytes);
  assert.equal(r.status, 1);
  assert.match(r.stderr, /does not match/);
});

test('a package M-VAVE\'s checks refuse fails the build, even with the right hash', () => {
  const bytes = readFileSync(packageFile());
  bytes[5000] ^= 1;                                    // a corrupt entry, whose hash is then declared
  const r = fixture(x => { x.firmware.sha256 = sha(bytes); }, bytes);
  assert.equal(r.status, 1);
  assert.match(r.stderr, /failed M-VAVE's package checks/);
  const wrongLoader = fixture(x => { x.firmware.loaderSha256 = 'f'.repeat(64); });
  assert.equal(wrongLoader.status, 1);
  assert.match(wrongLoader.stderr, /failed M-VAVE's package checks/);
});

test('a missing package fails the build', () => {
  const file = join(scratch(), 'release.json');
  writeFileSync(file, JSON.stringify(release()));
  const r = buildSite('--out', scratch(), '--release', file, '--releases', scratch());
  assert.equal(r.status, 1);
  assert.match(r.stderr, /is missing/);
});

// ---------------------------------------------------------------- the installer
// M-VAVE's real code and the real package, against the simulated FM-1.
const { NOT_OPEN, Session, packageMeta } = await import('../site/installer/driver.js');
const FM1Web = loadFM1Web();
const t = text => globalThis.FM1I18n.t(String(text));
const pkg = await FM1Web.prepare(metaFor(release().firmware, release().version, packageBytes()));
const TIMING = { idle: 700, writeIdle: 900, reconnect: 700, settle: 20 };

function rig(options = {}) {
  const access = simulatedAccess();
  const device = new SimulatedFM1(FM1Web, access, { imageBytes: pkg.image.length, newBuild: pkg.build_id, ...options });
  const events = { states: [], lines: [], phases: [] };
  const ui = {
    state: name => events.states.push(name), log: text => events.lines.push(t(text)),
    phase: (text, percent) => events.phases.push([text, percent]),
  };
  return { access, device, events, session: new Session(FM1Web, access, pkg, ui, t, TIMING) };
}
const hasCJK = text => /[㐀-鿿]/.test(text);

test('a full install: every byte reaches the device, and it ends on the build it reports', async () => {
  const { device, events, session } = rig();
  assert.equal(await session.connect(), true);
  assert.equal(await session.install(), true);
  assert.deepEqual(events.states, ['connecting', 'ready', 'installing', 'done']);
  assert.equal(Buffer.compare(Buffer.from(device.written), Buffer.from(pkg.image)), 0, 'the image written must be the package');
  assert.deepEqual(device.log.filter(x => ['arm', 'start', 'stage-one-done', 'stage-two-done'].includes(x)), ['arm', 'start', 'stage-one-done', 'start', 'stage-two-done']);
  assert.equal(device.build, pkg.build_id);
  assert.deepEqual(events.phases.at(-1), [t('更新完成'), 100]);
  for (const [text] of events.phases) assert.equal(hasCJK(text), false, text);
});

test('Connect writes nothing, and does not arm the device', async () => {
  const { device, session } = rig();
  await session.connect();
  assert.deepEqual([...new Set(device.log)].sort(), ['capability', 'identity']);
  assert.equal(device.writes, 0);
});

test('nothing installs before Connect has found a device', async () => {
  const { device, session } = rig();
  await assert.rejects(session.install(), /No device is ready/);
  assert.equal(device.writes, 0);
});

test('the driver never uses the updater\'s force mode', () => {
  assert.doesNotMatch(readFileSync(join(root, 'site/installer/driver.js'), 'utf8'), /force:\s*true/);
  assert.match(page('installer/index.html'), /id="install"[^>]*>INSTALL EXPERIMENTAL FIRMWARE/);
});

for (const [name, options] of [
  ["M-VAVE's stock firmware", { identity: 'FM-1_015', fault: 'no-capability' }],
  ['another identity', { identity: 'SOMETHING-ELSE' }],
  ['a third-party firmware using Open\'s prefix, with no capability reply', { identity: 'FM-1_8123456', fault: 'no-capability' }],
  ['another update family', { family: 6 }],
]) {
  test(`blocked, with no write: ${name}`, async () => {
    const { device, events, session } = rig(options);
    assert.equal(await session.connect(), false);
    assert.equal(session.state, 'blocked');
    assert.ok(events.lines.at(-1).includes(NOT_OPEN), events.lines.at(-1));
    await assert.rejects(session.install(), /No device is ready/);
    assert.equal(device.writes, 0);
    assert.equal(device.log.includes('start'), false);
    assert.equal(device.log.includes('arm'), false);
  });
}

test('the same build is not installed again', async () => {
  const { device, events, session } = rig({ build: pkg.build_id });
  assert.equal(await session.connect(), false);
  assert.match(events.lines.at(-1), /already installed/);
  assert.equal(device.writes, 0);
});

test('an interrupted install resumes from the loader', async () => {
  const { device, session } = rig({ mode: 'loader' });
  assert.equal(await session.connect(), true);
  assert.equal(await session.install(), true);
  assert.equal(device.log.includes('arm'), false);
  assert.equal(Buffer.compare(Buffer.from(device.written), Buffer.from(pkg.image)), 0);
});

test('unplugged while writing: it stops, says so, and resumes when reconnected', async () => {
  const { access, device, events, session } = rig({ fault: 'unplug' });
  await session.connect();
  assert.equal(await session.install(), false);
  assert.equal(session.state, 'failed');
  assert.match(events.lines.at(-1), /Install did not finish/);
  assert.equal(hasCJK(events.lines.at(-1)), false, events.lines.at(-1));
  assert.ok(device.writes > 0 && device.writes < pkg.image.length);
  device.replug();
  const again = new Session(FM1Web, access, pkg, { state() {}, log() {}, phase() {} }, t, TIMING);
  assert.equal(await again.connect(), true);
  assert.equal(await again.install(), true);
  assert.equal(Buffer.compare(Buffer.from(device.written), Buffer.from(pkg.image)), 0);
});

for (const [name, fault, connects] of [
  ['the device refuses the package', 'refuse', true],
  ['the device stops requesting data', 'stall', true],
  ['the device asks past the end of the image', 'out-of-range', true],
  ['the device finishes the wrong stage', 'wrong-stage', true],
  ['the device never comes back', 'never-returns', true],
  ['the device restarts on a different build', 'wrong-build', true],
  ['the device\'s identity changes before Install', 'identity-changes', true],
  ['the capability reply is malformed', 'bad-capability', false],
]) {
  test(`stops, and never reports success: ${name}`, async () => {
    const { events, session } = rig({ fault });
    assert.equal(await session.connect(), connects);
    if (connects) assert.equal(await session.install(), false);
    assert.equal(session.state, 'failed');
    assert.equal(events.states.includes('done'), false);
    assert.equal(hasCJK(events.lines.at(-1)), false, events.lines.at(-1));
  });
}

test('a device that is mid-update refuses Connect', async () => {
  const { events, session } = rig({ state: 1 });
  assert.equal(await session.connect(), false);
  assert.equal(session.state, 'failed');
  assert.equal(hasCJK(events.lines.at(-1)), false);
});

test('no device: a clear message', async () => {
  const lines = [];
  const session = new Session(FM1Web, simulatedAccess(), pkg, { state() {}, log: x => lines.push(t(x)), phase() {} }, t, TIMING);
  assert.equal(await session.connect(), false);
  assert.equal(session.state, 'failed');
  assert.match(lines.at(-1), /Connect failed/);
  assert.equal(hasCJK(lines.at(-1)), false, lines.at(-1));
});

test('a device that does not answer: a clear message', async () => {
  const { device, events, session } = rig({ fault: 'silent' });
  assert.equal(await session.connect(), false);
  assert.equal(hasCJK(events.lines.at(-1)), false, events.lines.at(-1));
  assert.equal(device.writes, 0);
});

test('two FM-1s at once: it will not guess', async () => {
  const access = simulatedAccess();
  new SimulatedFM1(FM1Web, access, { imageBytes: pkg.image.length, name: 'FM-1 A' });
  new SimulatedFM1(FM1Web, access, { imageBytes: pkg.image.length, name: 'FM-1 B' });
  const lines = [];
  const session = new Session(FM1Web, access, pkg, { state() {}, log: x => lines.push(t(x)), phase() {} }, t, TIMING);
  assert.equal(await session.connect(), false);
  assert.equal(session.state, 'failed');
  assert.equal(hasCJK(lines.at(-1)), false, lines.at(-1));
});

test('every message the updater can raise reaches the page in English', () => {
  const source = readFileSync(join(root, 'third_party/fm1_open/sdk/WebUpdate.js'), 'utf8');
  const updater = source.slice(0, source.indexOf('const crc32Table'));
  const messages = [...updater.matchAll(/'([^'\n]*[㐀-鿿][^'\n]*)'/g)].map(m => m[1]);
  assert.ok(messages.length > 30);
  assert.deepEqual(messages.filter(m => hasCJK(t(m))), []);
});

test('package checks: integrity, format, identity and loader all stop an install', async () => {
  const good = packageBytes();
  const meta = () => metaFor(release().firmware, release().version, good);
  const flipped = good.slice();
  flipped[5000] ^= 1;
  const cases = [
    ['a changed byte', { ...meta(), data: Buffer.from(flipped).toString('base64') }],
    ['a truncated file', { ...meta(), data: Buffer.from(good.subarray(0, 4096)).toString('base64') }],
    ['the wrong hash', { ...meta(), sha256: 'a'.repeat(64) }],
    ['the wrong loader hash', { ...meta(), loader_sha256: 'a'.repeat(64) }],
    ['the wrong identity', { ...meta(), product: 'FM-1_8000099' }],
    ['the wrong family', { ...meta(), family: 6 }],
  ];
  for (const [, bad] of cases) await assert.rejects(FM1Web.prepare(bad), Error);
  const notPackage = new Uint8Array(4096).fill(7);
  await assert.rejects(FM1Web.prepare({ ...meta(), data: Buffer.from(notPackage).toString('base64'), sha256: sha(notPackage), bytes: 4096 }), Error);
});

test('the package is what the docs and the release say it is', () => {
  assert.equal(pkg.product, release().firmware.product);
  assert.equal(pkg.loaderIdentity, 'ota-FM-1_015');
  assert.equal(pkg.build_id, release().firmware.buildId);
  const meta = packageMeta(release());
  assert.equal(meta.schema, 1);
  assert.equal(meta.family, 7);
  assert.equal(meta.url, `firmware/${release().firmware.filename}`);
  assert.equal(meta.hardware_validated, false);
  assert.equal(packageMeta(verified()).hardware_validated, true);
  assert.equal(meta.data, undefined);
});

test('the experimental warning is the agreed one, before Install, with a confirmation', () => {
  const html = page('installer/index.html');
  for (const line of ['EXPERIMENTAL FIRMWARE',
    'CLATTER Pocket has passed emulator testing but has not yet been verified on physical hardware.',
    'Installing it replaces the FM-1’s stock instrument firmware.',
    'There is a risk of installation failure, loss of settings or requiring firmware recovery.']) assert.ok(html.includes(line), line);
  assert.ok(html.indexOf('id="warning"') < html.indexOf('id="confirm"'));
  assert.ok(html.indexOf('id="confirm"') < html.indexOf('id="install"'));
  assert.match(html, /<dd>M-VAVE FM-1<\/dd>/);
  assert.ok(html.includes(release().firmware.sha256));
});

test('the installer page carries the recovery route', () => {
  const html = page('installer/index.html');
  for (const text of ['FIRST-HARDWARE-TEST.md#recovery', 'FM1-REPLACEMENT.md#start-up-safe-mode-diagnostics', 'open.m-vave.com/fm1-workbench', 'id="after-failure"'])
    assert.ok(html.includes(text), text);
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
test('every page keeps the UNVERIFIED designation', () => {
  for (const name of pages) assert.match(page(name), /UNVERIFIED: EXPERIMENTAL/, name);
  assert.equal(NOT_AVAILABLE, 'Hardware installation is not yet available. CLATTER Pocket is currently verified in the FM-1 emulator only.');
  assert.doesNotMatch(page('index.html'), /HARDWARE VERIFIED/);
});

test('every page moves itself to HTTPS: Web MIDI, crypto and the audio worklet need it', () => {
  for (const name of pages) {
    const html = page(name);
    assert.match(html, /location\.protocol === 'http:' && !isSecureContext/, name);
    assert.ok(html.indexOf('location.replace') < html.indexOf('<script type="module"') || !html.includes('<script type="module"'), name);
    assert.ok(html.indexOf('location.replace') < html.indexOf('<title>'), name);
  }
});

test('no marker is left unfilled', () => {
  for (const name of pages) assert.doesNotMatch(page(name), /\{\{|<!-- @/, name);
});

test('the hero carries the agreed words', () => {
  const html = page('index.html');
  for (const text of ['A generative drum machine hiding inside your FM-1.',
    'Seven voices of authentic LXR drum synthesis. Generative kits, evolving rhythms, live recording and melodic percussion, running directly on the M-VAVE FM-1.',
    'TRY IN BROWSER', 'VIEW SOURCE ON GITHUB']) assert.ok(html.includes(text), text);
  assert.match(html, /<a class="button" href="installer\/">INSTALL EXPERIMENTAL FIRMWARE/);
});

test('MIDI: only input is claimed, and the rest is denied', () => {
  const text = page('index.html').replace(/<[^>]+>/g, ' ');
  for (const m of text.matchAll(/[^.]*MIDI (output|thru|clock)[^.]*\./gi)) assert.match(m[0], /\bno\b/i, m[0]);
  for (const m of text.matchAll(/[^.]*external clock[^.]*\./gi)) assert.match(m[0], /\bno\b/i, m[0]);
});

test('the page does not present the firmware as finished', () => {
  const text = page('index.html');
  assert.doesNotMatch(text, /production[- ]ready|stable release|safe to (flash|install)(?! yet)|download (now|firmware)/i);
  assert.match(text, /<b>It is unproven\.<\/b>/);
  assert.doesNotMatch(text, /has been tested on a physical FM-1/);
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
  assert.equal(JSON.parse(page('release.json')).status, 'experimental');
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
  assert.doesNotMatch(yml, /force: true|installEnabled|firmware\/fm1\/build/);
});
