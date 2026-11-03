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
import { NOT_AVAILABLE, OPEN_REPOSITORY, STATUS, browserSupport, installAllowed, installLabel, openRefusals, refusals, statusLabel } from '../site/installer/gate.mjs';
import { CONTROLS, GEOMETRY, panelSvg } from '../site/assets/controls.mjs';
import { ensureOpenFirmware, releaseFile, verifyRelease, workbenchPackage } from './fetch-open-firmware.mjs';
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
const openFile = await ensureOpenFirmware(release().openFirmware);
const openBytes = () => new Uint8Array(readFileSync(openFile));

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
    r => { r.openFirmware = null; }, r => { r.openFirmware.repository = 'someone/fm1_open'; }, r => { r.openFirmware.tag = 'V13'; },
    r => { r.openFirmware.commit = 'nope'; }, r => { r.openFirmware.filename = 'other.fwsc'; }, r => { r.openFirmware.filename = 'FM1-Open-V14-00000000.fwsc'; },
    r => { r.openFirmware.asset = 'other.html'; }, r => { r.openFirmware.sums = 'other'; }, r => { r.openFirmware.assetSha256 = 'abc'; },
    r => { r.openFirmware.sumsSha256 = null; }, r => { r.openFirmware.sha256 = 'abc'; }, r => { r.openFirmware.buildId = null; },
    r => { r.openFirmware.loaderSha256 = 'e'.repeat(64); }, r => { r.openFirmware.bytes = 12; },
    r => { r.openFirmware.product = 'FM-1_8000015'; }, r => { r.openFirmware.product = 'FM-1_015'; }, r => { r.openFirmware.family = 6; },
  ];
  breaks.forEach((change, i) => {
    const r = structuredClone(release());
    change(r);
    assert.equal(installAllowed(r), false, `change ${i}: ${change}`);
  });
});

test('official Open must come from M-VAVE, and be what CLATTER Pocket is built on', () => {
  const r = release(), open = r.openFirmware;
  assert.equal(OPEN_REPOSITORY, 'm-vave/fm1_open');
  assert.equal(open.repository, OPEN_REPOSITORY);
  assert.deepEqual(openRefusals(open, r.firmware), []);
  assert.deepEqual(openRefusals({ ...open, loaderSha256: 'e'.repeat(64) }, r.firmware), ['this firmware is not built on the pinned official Open firmware']);
  assert.equal(r.firmware.product, 'FM-1_8133700', 'its own identity, not Open\'s');
  assert.notEqual(open.product, r.firmware.product);
  assert.equal(open.loaderSha256, r.firmware.loaderSha256);
  assert.equal(open.bytes, r.firmware.bytes, 'the flash layout is stock Open\'s, so the packages are the same size');
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
  const open = release().openFirmware, servedOpen = readFileSync(join(site, 'installer/firmware', open.filename));
  assert.equal(sha(servedOpen), open.sha256);
  assert.equal(servedOpen.length, open.bytes);
  assert.equal(sha(readFileSync(join(site, 'installer/WebUpdate.js'))), sha(readFileSync(join(root, 'third_party/fm1_open/sdk/WebUpdate.js'))), 'WebUpdate.js must be unchanged');
  assert.equal(sha(readFileSync(join(site, 'installer/I18n.js'))), sha(readFileSync(join(root, 'third_party/fm1_open/sdk/I18n.js'))), 'I18n.js must be unchanged');
});

test('official Open is published with the site, and is not a file in this repository', () => {
  const tracked = execFileSync('git', ['ls-files'], { cwd: root, encoding: 'utf8' }).split('\n');
  assert.deepEqual(tracked.filter(name => /FM1-Open/i.test(name)), []);
  assert.deepEqual(tracked.filter(name => /\.fwsc$/.test(name) && !/^releases\/[\d.]+\/clatter-pocket-/.test(name)), []);
  assert.ok(page('installer/index.html').includes(release().openFirmware.sha256));
  assert.ok(page('installer/index.html').includes('https://github.com/m-vave/fm1_open/releases/tag/V14'));
  const source = readFileSync(join(site, 'legal/fm1_open-SOURCE.txt'), 'utf8');
  assert.ok(source.includes(`https://github.com/m-vave/fm1_open/tree/${release().openFirmware.commit}`));
  for (const name of ['fm1_open-LICENSE.txt', 'fm1_open-THIRD_PARTY.txt']) assert.ok(existsSync(join(site, 'legal', name)), name);
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
  assert.doesNotMatch(off.page('installer/index.html'), /stage-open|INSTALL OPEN FIRMWARE/);
});

test('no page offers a firmware file for download', () => {
  for (const name of pages) assert.doesNotMatch(page(name), /clatter-pocket-emulator-only|href="[^"]*\.fwsc"|download/i, name);
  assert.doesNotMatch(page('demo/emu/firmwares.json'), /"(installer|manifest|embedded|hub|download)"/);
});

test('installer controls ship disabled, until the script has checked the package', () => {
  const html = page('installer/index.html');
  for (const id of ['connect', 'install', 'confirm', 'install-open', 'confirm-open']) assert.match(html, new RegExp(`id="${id}"[^>]* disabled`), id);
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
  const wrongLoader = fixture(x => { x.firmware.loaderSha256 = x.openFirmware.loaderSha256 = 'f'.repeat(64); });
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
const { FIRMWARE, NOT_IDENTIFIED, RECOVERY, Session, classify, openPackageMeta, packageMeta, pageMemory } = await import('../site/installer/driver.js');
const FM1Web = loadFM1Web();
const t = text => globalThis.FM1I18n.t(String(text));
const pkg = await FM1Web.prepare(metaFor(release().firmware, release().version, packageBytes()));
const openPkg = await FM1Web.prepare(metaFor(release().openFirmware, release().openFirmware.version, openBytes()));
const packages = { clatter: pkg, open: openPkg };
const TIMING = { idle: 700, writeIdle: 900, reconnect: 700, settle: 20 };
const quiet = { state() {}, log() {}, phase() {} };
const openStorage = (held = new Map()) => pageMemory({ getItem: key => held.get(key) ?? null, setItem: (key, value) => held.set(key, value), removeItem: key => held.delete(key) });
const newSession = (access, ui = quiet, memory = openStorage(), offered = packages) => new Session(FM1Web, access, offered, ui, t, TIMING, memory);
const images = [
  { image: openPkg.image, identity: openPkg.product, build: openPkg.build_id },
  { image: pkg.image, identity: pkg.product, build: pkg.build_id },
];
const same = (written, expected) => assert.equal(Buffer.compare(Buffer.from(written), Buffer.from(expected)), 0, 'the image written must be the package');

function rig(options = {}, memory) {
  const access = simulatedAccess();
  const device = new SimulatedFM1(FM1Web, access, { imageBytes: pkg.image.length, newBuild: pkg.build_id, ...options });
  const events = { states: [], lines: [], phases: [] };
  const ui = {
    state: name => events.states.push(name), log: text => events.lines.push(t(text)),
    phase: (text, percent) => events.phases.push([text, percent]),
  };
  return { access, device, events, memory, session: newSession(access, ui, memory) };
}
/** A device as stock V15 reports itself, which restarts as whichever package it is sent. */
const stockRig = (options = {}, memory) => rig({ identity: 'FM-1_015', protocol: 'stock', build: '00'.repeat(32), newBuild: undefined, images, ...options }, memory);
const openRig = (options = {}, memory) => rig({ build: openPkg.build_id, ...options }, memory);
const wrote = device => device.log.includes('start') || device.writes > 0;
const hasCJK = text => /[㐀-鿿]/.test(text);

test('a full install: every byte reaches the device, and it ends on the build it reports', async () => {
  const { device, events, session } = openRig({ newBuild: pkg.build_id });
  assert.equal(await session.connect(), true);
  assert.equal(await session.install(true), true);
  assert.deepEqual(events.states, ['connecting', 'ready', 'installing', 'done']);
  same(device.written, pkg.image);
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
  await assert.rejects(session.install(true), /No device is ready/);
  await assert.rejects(session.installOpen(true), /No device is ready/);
  assert.equal(device.writes, 0);
});

test('force mode: official Open over stock, and a loader the unforced route does not name', () => {
  const source = readFileSync(join(root, 'site/installer/driver.js'), 'utf8');
  assert.equal(source.match(/force:\s*true/g).length, 2, 'one connect and one install');
  const stage = source.slice(source.indexOf('async installOpen('), source.indexOf('/** Stage 2: CLATTER Pocket. */'));
  assert.equal(stage.match(/force:\s*true/g).length, 2);
  assert.match(stage, /this\.updaterFor\(open, fm1Ports\(this\.access\)\)/);
  // Otherwise force follows `forced`, which only a renamed loader found at Connect sets.
  assert.deepEqual(source.match(/this\.forced = [^;]+;/g), ['this.forced = false;', 'this.forced = renamed;']);
  const loader = source.slice(source.indexOf('async inLoader('), source.indexOf('/** Stage 1'));
  assert.match(loader, /renamed \? fm1Ports\(this\.access\) : this\.access/);
  assert.match(loader, /connect\(\{ force: renamed \}\)/);
  assert.match(source.slice(source.indexOf('/** Stage 2: CLATTER Pocket. */')), /install\(\{ force: this\.forced \}\)/);
  assert.match(page('installer/index.html'), /id="install"[^>]*>INSTALL CLATTER POCKET/);
  assert.match(page('installer/index.html'), /id="install-open"[^>]*>INSTALL OPEN FIRMWARE/);
});

for (const [name, options] of [
  ['another identity', { identity: 'SOMETHING-ELSE' }],
  ['a third-party firmware using Open\'s prefix, with no capability reply', { identity: 'FM-1_8123456', fault: 'no-capability' }],
  ['another update family', { family: 6 }],
]) {
  test(`blocked, with no write: ${name}`, async () => {
    const { device, events, session } = rig(options);
    assert.equal(await session.connect(), false);
    assert.equal(session.state, 'blocked');
    assert.equal(session.firmware, FIRMWARE.UNKNOWN);
    assert.ok(events.lines.at(-1).includes(NOT_IDENTIFIED), events.lines.at(-1));
    await assert.rejects(session.install(true), /No device is ready/);
    await assert.rejects(session.installOpen(true), /No device is ready/);
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
  const memory = openStorage();
  memory.write({ stage: 'clatter', sha256: pkg.sha256 });
  const { device, session } = rig({ mode: 'loader' }, memory);
  assert.equal(await session.connect(), true);
  assert.equal(session.state, 'ready');
  assert.equal(await session.install(true), true);
  assert.equal(device.log.includes('arm'), false);
  same(device.written, pkg.image);
  assert.equal(memory.read(), null);
});

// A real FM-1 came back from the handover under this name, and the page waited for ota-FM-1_015.
const RENAMED = 'ota-FM1C_0000000000000000';

test('the loader comes back under another ota- name: the install still completes', async () => {
  const { device, events, session } = openRig({ newBuild: pkg.build_id, loaderName: RENAMED });
  assert.equal(await session.connect(), true);
  assert.equal(await session.install(true), true);
  assert.deepEqual(events.states, ['connecting', 'ready', 'installing', 'done']);
  same(device.written, pkg.image);
  assert.equal(device.build, pkg.build_id);
});

test('both stages from stock, with the loader under another ota- name', async () => {
  const { device, session } = stockRig({ loaderName: RENAMED });
  await session.connect();
  assert.equal(await session.installOpen(true), true);
  same(device.written, openPkg.image);
  assert.equal(await session.install(true), true);
  same(device.written, pkg.image);
  assert.equal(device.build, pkg.build_id);
});

test('in a renamed loader at Connect, with a record of CLATTER Pocket: it resumes by force', async () => {
  const memory = openStorage();
  memory.write({ stage: 'clatter', sha256: pkg.sha256 });
  const { device, session } = rig({ mode: 'loader', loaderName: RENAMED }, memory);
  assert.equal(await session.connect(), true);
  assert.equal(session.state, 'ready');
  assert.equal(session.firmware, FIRMWARE.LOADER);
  assert.equal(session.forced, true);
  assert.equal(device.writes, 0, 'Connect writes nothing');
  assert.equal(await session.install(true), true);
  assert.equal(device.log.includes('arm'), false);
  same(device.written, pkg.image);
  assert.equal(memory.read(), null);
});

test('in a renamed loader at Connect, with no record: official Open, by force', async () => {
  const { device, session } = stockRig({ mode: 'loader', loaderName: RENAMED }, openStorage());
  assert.equal(await session.connect(), true);
  assert.equal(session.state, 'recover');
  assert.equal(session.forced, true);
  await assert.rejects(session.install(true), /No device is ready/);
  assert.equal(await session.installOpen(true), true);
  same(device.written, openPkg.image);
  assert.equal(device.identity, openPkg.product);
  assert.equal(session.forced, false, 'Stage 2 goes the normal route');
});

test('a renamed loader does not open force to other MIDI devices', async () => {
  const memory = openStorage();
  memory.write({ stage: 'clatter', sha256: pkg.sha256 });
  const { access, device, session } = rig({ mode: 'loader', loaderName: RENAMED }, memory);
  const other = new SimulatedFM1(FM1Web, access, { name: 'Other Synth', imageBytes: 0 });
  await session.connect();
  assert.equal(await session.install(true), true);
  assert.deepEqual(other.log.filter(x => x !== 'identity'), []);
  same(device.written, pkg.image);
});

test('unplugged while writing: it stops, says so, and resumes when reconnected', async () => {
  const memory = openStorage();
  const { access, device, events, session } = openRig({ fault: 'unplug', newBuild: pkg.build_id }, memory);
  await session.connect();
  assert.equal(await session.install(true), false);
  assert.equal(session.state, 'failed');
  assert.equal(session.failedStage, 'clatter');
  assert.match(events.lines.at(-1), /Install did not finish/);
  assert.equal(hasCJK(events.lines.at(-1)), false, events.lines.at(-1));
  assert.ok(device.writes > 0 && device.writes < pkg.image.length);
  device.replug();
  const again = newSession(access, quiet, memory);
  assert.equal(await again.connect(), true);
  assert.equal(again.state, 'ready', 'the page remembers that it was writing CLATTER Pocket');
  assert.equal(await again.install(true), true);
  same(device.written, pkg.image);
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
    if (connects) assert.equal(await session.install(true), false);
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
  const session = newSession(simulatedAccess(), { state() {}, log: x => lines.push(t(x)), phase() {} });
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
  const session = newSession(access, { state() {}, log: x => lines.push(t(x)), phase() {} });
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

// ---------------------------------------------------------------- official Open
// SIMULATED. Everything from here to the next heading ran M-VAVE's real updater and the real
// packages against tools/fm1-update-sim.mjs. None of it has touched an FM-1.
const releaseFiles = () => {
  const open = release().openFirmware;
  const meta = {
    schema: 1, product: open.product, profile: 'dx7', family: open.family, release_version: open.version, build_id: open.buildId,
    sha256: open.sha256, bytes: open.bytes, loader_sha256: open.loaderSha256, hardware_validated: false,
    data: Buffer.from(openBytes()).toString('base64'),
  };
  const page = held => Buffer.from(`<script>const FM1_PACKAGE = ${JSON.stringify(held)};\nconst a = "}";</script>`);
  const pinFor = workbench => {
    const sums = Buffer.from(`${sha(workbench)}  ${open.asset}\n`);
    return { sums, pin: { ...open, assetSha256: sha(workbench), sumsSha256: sha(sums) } };
  };
  return { meta, page, pinFor };
};

test('the pinned release is official, and its firmware is what the pin says', () => {
  const open = release().openFirmware;
  assert.equal(sha(openBytes()), open.sha256);
  assert.equal(openBytes().length, open.bytes);
  assert.equal(openPkg.product, open.product);
  assert.equal(openPkg.build_id, open.buildId);
  assert.equal(openPkg.loaderIdentity, 'ota-FM-1_015');
  assert.equal(releaseFile(open, open.asset), 'https://github.com/m-vave/fm1_open/releases/download/V14/FM1-Open-V14-Workbench.html');
  assert.equal(openPackageMeta(release()).url, `firmware/${open.filename}`);
  assert.equal(openPackageMeta(release()).data, undefined);
  assert.equal(openPkg.image.length, pkg.image.length);
});

test('the package is found inside a workbench page, past a brace in a string', () => {
  const kit = releaseFiles();
  assert.equal(workbenchPackage(kit.page({ ...kit.meta, note: 'a } in "text" {' }).toString()).sha256, kit.meta.sha256);
  assert.throws(() => workbenchPackage('<p>no firmware</p>'), /no firmware package/);
});

test('a real release passes, and every way of differing from the pin is refused', () => {
  const kit = releaseFiles(), workbench = kit.page(kit.meta), { sums, pin } = kit.pinFor(workbench);
  assert.equal(sha(verifyRelease(pin, sums, workbench)), pin.sha256);

  const altered = Buffer.from(workbench);
  altered[altered.length - 20] ^= 1;
  assert.throws(() => verifyRelease(pin, sums, altered), /published SHA-256/);
  assert.throws(() => verifyRelease(pin, Buffer.from('other'), workbench), /not the pinned file/);
  const silent = Buffer.from('nothing listed\n');
  assert.throws(() => verifyRelease({ ...pin, sumsSha256: sha(silent) }, silent, workbench), /does not list/);

  for (const [name, change] of [
    ['a newer version', { release_version: 'V15' }], ['another build', { build_id: 'a'.repeat(64) }], ['another identity', { product: 'FM-1_8000015' }],
    ['another family', { family: 6 }], ['another loader', { loader_sha256: 'a'.repeat(64) }], ['another hash', { sha256: 'a'.repeat(64) }],
  ]) {
    const other = kit.page({ ...kit.meta, ...change }), files = kit.pinFor(other);
    assert.throws(() => verifyRelease(files.pin, files.sums, other), /the workbench's package has/, name);
  }
  const flipped = openBytes();
  flipped[5000] ^= 1;
  const swapped = kit.page({ ...kit.meta, data: Buffer.from(flipped).toString('base64') }), files = kit.pinFor(swapped);
  assert.throws(() => verifyRelease(files.pin, files.sums, swapped), /does not match its pin/);
});

test('a download is checked before anything is kept, and a bad cached copy is not trusted', { timeout: 30000 }, async () => {
  const kit = releaseFiles(), workbench = kit.page(kit.meta), { sums, pin } = kit.pinFor(workbench), cache = scratch();
  const served = files => async url => new Response(url.endsWith(pin.sums) ? files.sums : files.workbench);
  const altered = Buffer.from(workbench);
  altered[altered.length - 20] ^= 1;
  await assert.rejects(ensureOpenFirmware(pin, { cache, fetcher: served({ sums, workbench: altered }) }), /published SHA-256/);
  assert.deepEqual(readdirSync(cache), []);

  const path = await ensureOpenFirmware(pin, { cache, fetcher: served({ sums, workbench }) });
  assert.equal(sha(readFileSync(path)), pin.sha256);
  await ensureOpenFirmware(pin, { cache, fetcher: async () => { throw new Error('the cache should answer'); } });

  writeFileSync(path, 'something else');
  assert.equal(sha(readFileSync(await ensureOpenFirmware(pin, { cache, fetcher: served({ sums, workbench }) }))), pin.sha256);
  await assert.rejects(ensureOpenFirmware({ ...pin, filename: 'FM1-Open-V14-00000000.fwsc' }, { cache, fetcher: async () => new Response('gone', { status: 404 }) }), /could not download/);
});

test('official Open that M-VAVE\'s checks refuse fails the build', () => {
  const dir = scratch(), bytes = Buffer.from(openBytes());
  bytes[5000] ^= 1;                                    // a corrupt entry, whose hash is then declared
  const r = release(), name = `FM1-Open-V14-${sha(bytes).slice(0, 8)}.fwsc`;
  r.openFirmware = { ...r.openFirmware, sha256: sha(bytes), filename: name };
  writeFileSync(join(dir, name), bytes);
  const file = join(dir, 'release.json');
  writeFileSync(file, JSON.stringify(r));
  const refused = buildSite('--out', scratch(), '--release', file, '--open-cache', dir);
  assert.equal(refused.status, 1);
  assert.match(refused.stderr, /failed M-VAVE's package checks/);
});

test('detection names each kind of firmware from the protocol alone', () => {
  const is = (result, kind) => assert.equal(classify(result, pkg, openPkg), kind);
  is({ identity: 'FM-1_015', migration: true }, FIRMWARE.STOCK);
  is({ identity: 'FM-1_015', legacy: true }, FIRMWARE.STOCK);
  is({ identity: 'FM-1_8123456', migration: true }, FIRMWARE.UNKNOWN);
  is({ identity: 'SOMETHING-ELSE', migration: true }, FIRMWARE.UNKNOWN);
  is({ identity: 'ota-FM-1_015', resume: true }, FIRMWARE.LOADER);
  is({ identity: 'ota-FM1C_0000000000000000', migration: true }, FIRMWARE.LOADER);
  is({ identity: 'FM-1_8000014', current: openPkg.build_id }, FIRMWARE.OPEN);
  is({ identity: 'FM-1_8000014', current: pkg.build_id, same: true }, FIRMWARE.CLATTER);
  is({ identity: 'FM-1_8000014', current: 'c'.repeat(64) }, FIRMWARE.OPEN_COMPATIBLE);
});

test('stock firmware is detected, with no write, and Stage 1 is offered, not started', async () => {
  const { device, events, session } = stockRig();
  assert.equal(await session.connect(), true);
  assert.equal(session.state, 'prepare');
  assert.equal(session.firmware, FIRMWARE.STOCK);
  assert.match(events.lines.at(-1), /Stock firmware detected/);
  assert.deepEqual([...new Set(device.log)], ['identity'], 'stock is only asked what it is');
  assert.equal(wrote(device), false);
  await assert.rejects(session.install(true), /No device is ready/);
  assert.equal(wrote(device), false);
});

test('Stage 1 will not start without confirmation', async () => {
  const { device, session } = stockRig();
  await session.connect();
  for (const answer of [undefined, false, 'true', 1]) await assert.rejects(session.installOpen(answer), /explicit confirmation/);
  assert.equal(session.state, 'prepare');
  assert.equal(wrote(device), false);
});

test('both stages complete from stock: Open is verified, then CLATTER Pocket waits to be confirmed', async () => {
  const { device, events, session } = stockRig();
  await session.connect();
  assert.equal(await session.installOpen(true), true);
  same(device.written, openPkg.image);
  assert.equal(device.identity, openPkg.product);
  assert.equal(device.build, openPkg.build_id);
  assert.deepEqual(device.log.filter(x => ['arm', 'start', 'stage-one-done', 'stage-two-done'].includes(x)), ['start', 'stage-one-done', 'start', 'stage-two-done']);
  assert.equal(session.state, 'ready');
  assert.equal(session.firmware, FIRMWARE.OPEN);
  assert.match(events.lines.at(-1), /Official Open V14 detected/);
  assert.deepEqual(events.phases.at(-1), [t('更新完成'), 100]);
  for (const [text] of events.phases) assert.equal(hasCJK(text), false, text);

  // no second-stage install without its own confirmation, however long it is left
  await new Promise(resolve => setTimeout(resolve, 60));
  assert.equal(device.installs.length, 1);
  assert.equal(device.log.includes('arm'), false);
  for (const answer of [undefined, false]) await assert.rejects(session.install(answer), /explicit confirmation/);
  assert.equal(session.state, 'ready');
  assert.equal(device.installs.length, 1);

  assert.equal(await session.install(true), true);
  assert.equal(session.state, 'done');
  same(device.written, pkg.image);
  assert.equal(device.build, pkg.build_id);
  assert.equal(device.installs.length, 2);
  assert.ok(device.log.includes('arm'), 'Stage 2 is the normal, unforced route');
});

test('already on official Open: Stage 1 is skipped', async () => {
  const { device, events, session } = openRig({ images });
  assert.equal(await session.connect(), true);
  assert.equal(session.state, 'ready');
  assert.equal(session.firmware, FIRMWARE.OPEN);
  assert.equal(events.states.includes('prepare'), false);
  await assert.rejects(session.installOpen(true), /No device is ready/);
  assert.equal(await session.install(true), true);
  assert.equal(device.installs.length, 1);
  same(device.written, pkg.image);
});

test('another fm1_open build goes the normal route, and is not called official Open', async () => {
  const { device, events, session } = rig({ build: 'c'.repeat(64) });
  assert.equal(await session.connect(), true);
  assert.equal(session.firmware, FIRMWARE.OPEN_COMPATIBLE);
  assert.match(events.lines.at(-1), /not official Open V14 or this release/);
  assert.equal(wrote(device), false);
});

test('this release, under its own identity or Open\'s: the build tells it apart', async () => {
  for (const identity of [pkg.product, openPkg.product]) {
    const { device, events, session } = rig({ build: pkg.build_id, identity });
    assert.equal(await session.connect(), false, identity);
    assert.equal(session.firmware, FIRMWARE.CLATTER, identity);
    assert.match(events.lines.at(-1), /already installed/, identity);
    assert.equal(wrote(device), false, identity);
  }
});

test('another CLATTER Pocket build is named, and can be installed over', async () => {
  const { device, events, session } = rig({ build: 'c'.repeat(64), identity: pkg.product });
  assert.equal(await session.connect(), true);
  assert.equal(session.firmware, FIRMWARE.OPEN_COMPATIBLE);
  assert.match(events.lines.at(-1), /Another CLATTER Pocket build detected/);
  assert.equal(wrote(device), false);
});

test('Stage 1 asks the FM-1 again: a device that has changed is left alone', async () => {
  for (const [name, change] of [
    ['now reports Open', device => Object.assign(device, { identity: 'FM-1_8000014', protocol: 'open', build: openPkg.build_id })],
    ['now reports something else', device => { device.identity = 'SOMETHING-ELSE'; }],
    ['now in its loader', device => { device.mode = 'loader'; }],
  ]) {
    const { device, events, session } = stockRig();
    await session.connect();
    change(device);
    assert.equal(await session.installOpen(true), false, name);
    assert.equal(session.state, 'blocked', name);
    assert.match(events.lines.at(-1), /Nothing was written/, name);
    assert.equal(wrote(device), false, name);
  }
});

test('forced connect does not ask other MIDI devices what they are', async () => {
  const { access, device, session } = stockRig();
  const other = new SimulatedFM1(FM1Web, access, { name: 'Other Synth', imageBytes: 0 });
  await session.connect();
  assert.equal(await session.installOpen(true), true);
  assert.deepEqual(other.log, []);
  same(device.written, openPkg.image);
});

test('stock, with the Open package unavailable: stopped, with no write', async () => {
  const access = simulatedAccess(), lines = [];
  const device = new SimulatedFM1(FM1Web, access, { identity: 'FM-1_015', protocol: 'stock', imageBytes: pkg.image.length });
  const session = newSession(access, { ...quiet, log: x => lines.push(x) }, openStorage(), { clatter: pkg, open: null });
  assert.equal(await session.connect(), false);
  assert.equal(session.state, 'blocked');
  assert.match(lines.at(-1), /failed its checks/);
  assert.equal(wrote(device), false);
});

test('interrupted before the loader: the FM-1 is still stock, and Stage 1 can start again', async () => {
  const memory = openStorage();
  const { access, device, session } = stockRig({ fault: 'unplug-first-stage' }, memory);
  await session.connect();
  assert.equal(await session.installOpen(true), false);
  assert.equal(session.state, 'failed');
  assert.equal(session.failedStage, 'open');
  assert.equal(device.writes, 0);
  device.replugRunning();
  const again = newSession(access, quiet, memory);
  assert.equal(await again.connect(), true);
  assert.equal(again.state, 'prepare');
  assert.equal(memory.read(), null, 'the device is not mid-update');
  assert.equal(await again.installOpen(true), true);
  same(device.written, openPkg.image);
});

test('interrupted in the loader, then reloaded: Stage 1 resumes unforced, verifies, and Stage 2 waits', async () => {
  const memory = openStorage();
  const { access, device, session } = stockRig({ fault: 'unplug' }, memory);
  await session.connect();
  assert.equal(await session.installOpen(true), false);
  assert.equal(session.state, 'failed');
  assert.match(RECOVERY[session.failedStage], /Nothing will be retried/);
  assert.ok(device.writes > 0 && device.writes < openPkg.image.length);
  assert.equal(device.installs, undefined, 'never reported as installed');

  // a new page, as after a browser refresh, on the same device
  device.replug();
  const reloaded = newSession(access, quiet, memory);
  assert.equal(await reloaded.connect(), true);
  assert.equal(reloaded.state, 'recover');
  assert.equal(reloaded.firmware, FIRMWARE.LOADER);
  await assert.rejects(reloaded.installOpen(), /explicit confirmation/);
  assert.equal(await reloaded.installOpen(true), true);
  same(device.written, openPkg.image);
  assert.equal(reloaded.state, 'ready');
  assert.equal(memory.read(), null);
});

test('in the loader with no record of why: official Open, never CLATTER Pocket by default', async () => {
  const blocked = () => { throw new Error('blocked'); };
  for (const memory of [openStorage(), pageMemory({ getItem: blocked, setItem: blocked, removeItem: blocked })]) {
    const { device, session } = stockRig({ mode: 'loader' }, memory);
    assert.equal(await session.connect(), true);
    assert.equal(session.state, 'recover');
    await assert.rejects(session.install(true), /No device is ready/);
    assert.equal(await session.installOpen(true), true);
    same(device.written, openPkg.image);
    assert.equal(device.identity, openPkg.product);
  }
});

test('a record for another package does not resume it', async () => {
  const memory = openStorage();
  memory.write({ stage: 'clatter', sha256: 'a'.repeat(64) });
  const { session } = stockRig({ mode: 'loader' }, memory);
  await session.connect();
  assert.equal(session.state, 'recover');
});

for (const [name, fault] of [
  ['the device stops requesting data', 'stall'],
  ['the device asks past the end of the image', 'out-of-range'],
  ['the device finishes the wrong stage', 'wrong-stage'],
  ['the restart times out', 'never-returns'],
  ['the device restarts on a different build', 'wrong-build'],
  ['the device restarts as other firmware', 'boots-other'],
]) {
  test(`Stage 1 fails safely, and offers no Stage 2: ${name}`, async () => {
    const { device, events, session } = stockRig({ fault });
    await session.connect();
    assert.equal(await session.installOpen(true), false);
    assert.equal(session.state, 'failed');
    assert.equal(session.failedStage, 'open');
    assert.match(events.lines.at(-1), /did not finish/);
    assert.equal(hasCJK(events.lines.at(-1)), false, events.lines.at(-1));
    assert.equal(events.states.includes('ready'), false);
    assert.equal(events.states.includes('done'), false);
    await assert.rejects(session.install(true), /No device is ready/);
    await assert.rejects(session.installOpen(true), /No device is ready/);

    // nothing tries again behind the person's back
    const starts = device.log.filter(x => x === 'start').length;
    await new Promise(resolve => setTimeout(resolve, 60));
    assert.equal(device.log.filter(x => x === 'start').length, starts);
  });
}

test('Stage 1 is not called done by a device that goes silent after restarting', async () => {
  const { device, session } = stockRig();
  await session.connect();
  const installed = session.installOpen(true);
  while (!device.installs) await new Promise(resolve => setTimeout(resolve, 2));
  device.fault = 'silent';
  assert.equal(await installed, false);
  assert.equal(session.state, 'failed');
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
  assert.equal(entry.version, JSON.parse(readFileSync(join(root, 'site/release.json'), 'utf8')).version);
  assert.doesNotMatch(entry.package, /\.fwsc$/);
  const read = async url => new Response(readFileSync(join(root, 'site/demo/emu', url)));
  const got = await fetchPackage(entry, read);
  assert.equal(sha(got.bytes), build.sha256);
  assert.ok(appFromPackage(got.bytes).length > 400000);
});

test('a returning visitor gets the promoted build, not the one their browser kept', () => {
  const build = JSON.parse(readFileSync(join(root, 'site/demo/emulator-build.json'), 'utf8'));
  const catalog = JSON.parse(readFileSync(join(root, 'site/demo/emu/firmwares.json'), 'utf8'));
  assert.equal(catalog.firmwares[0].sha256, build.sha256);
  assert.match(readFileSync(join(root, 'site/demo/emu/main.js'), 'utf8'), /kept && \(!f\.sha256 \|\| kept\.hash === f\.sha256\)/);
  assert.match(readFileSync(join(root, 'tools/promote-emulator-build.mjs'), 'utf8'), /sha256: build\.sha256/);
});

test('the emulator draws the main page\'s panel, and each drawn control is the emulator\'s', async () => {
  const html = readFileSync(join(root, 'site/demo/emu/index.html'), 'utf8');
  assert.match(html, /href="\.\.\/\.\.\/assets\/fm1\.css"/);
  assert.match(html, /<div class="skin"><canvas id="panel"/);
  assert.match(page('index.html'), /href="assets\/fm1\.css"/);
  const main = readFileSync(join(root, 'site/demo/emu/main.js'), 'utf8');
  assert.match(main, /paintPanel\(frame\?\.leds, down, encClicks, pot, turning\)/);
  assert.match(main, /const lcd = ui\.panel_lcd\(\), leds = ui\.panel_leds\(\);/, 'a pointer is taken before the buffer: the call can grow it');

  const wasm = readFileSync(join(root, 'site/demo/emu/fm1_web.wasm'));
  const { exports: ui } = (await WebAssembly.instantiate(wasm, { env: { fm1_jit_compile: () => 0 } })).instance;
  ui.panel_board(0);
  assert.deepEqual([ui.panel_width(), ui.panel_height()], [1280, 820]);
  const svg = panelSvg({ interactive: true });
  const attr = (tag, name) => Number(new RegExp(`\\b${name}="([^"]+)"`).exec(tag)[1]);
  const pressable = [...svg.matchAll(/<rect class="(?:btn|key(?: blk)?)"[^>]*>/g)]
    .map(([tag]) => ui.panel_hit(attr(tag, 'x') + attr(tag, 'width') / 2, attr(tag, 'y') + attr(tag, 'height') / 2) - 100);
  assert.deepEqual(pressable.sort((a, b) => a - b), [...Array(41).keys()]);
  const knobs = [...svg.matchAll(/<circle class="knob"[^>]*>/g)].map(([tag]) => ui.panel_hit(attr(tag, 'cx'), attr(tag, 'cy')));
  assert.deepEqual(knobs.sort((a, b) => a - b), [...Array(8).keys()]);
});

test('the demo sends visitors only to controls the embedded emulator shows', () => {
  const css = readFileSync(join(root, 'site/demo/emu/style.css'), 'utf8');
  assert.match(css, /\[data-embed\] \.save-box/);
  assert.doesNotMatch(page('demo/index.html'), /under the emulator/);
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

test('MIDI: notes and clock in are claimed, and the rest is denied', () => {
  const text = page('index.html').replace(/<[^>]+>/g, ' ');
  for (const m of text.matchAll(/[^.]*MIDI (output|thru)[^.]*\./gi)) assert.match(m[0], /\bno\b/i, m[0]);
  assert.match(text, /MIDI clock in is followed/);
  assert.doesNotMatch(text, /MIDI clock out|sends? MIDI clock/i);
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
  assert.deepEqual(chords.sort(), ['SAVE + ARP', 'SAVE + ENV', 'SAVE + FX', 'SAVE + LFO', 'SAVE + OCT+', 'SAVE + OCT−']);
  for (const chord of ['SAVE+ENV', 'SAVE+ARP', 'SAVE+LFO', 'SAVE+OCT−', 'SAVE+OCT+']) assert.ok(docs.includes(chord), chord);
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
  assert.match(does('fx').save, /Lock or unlock/);
  assert.match(does('save').does, /PROJECTS/);
  assert.match(does('seq').does, /keyboard becomes the selected track's steps/);
  assert.match(does('glo').does, /SYSTEM/);
  assert.match(does('select').kind, /no push switch/);
  assert.equal(does('select').save, undefined);
});

test('the demo keys are the emulator\'s', () => {
  const main = readFileSync(join(site, 'demo/emu/main.js'), 'utf8');
  const map = /R REC, 1\.\.0 (FX SCL ENV LFO EDIT GLO HOME SAVE ARP SEQ)/.exec(main)[1].split(' ');
  const keys = [...'1234567890'];
  map.forEach((label, i) => assert.equal(CONTROLS.find(c => c.id === label.toLowerCase()).key, keys[i], label));
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
  assert.ok(yml.indexOf('node tools/fetch-open-firmware.mjs') < yml.indexOf('node --test tools/test-site.mjs'));
  assert.match(yml, /- tools\/fetch-open-firmware\.mjs/);
});
