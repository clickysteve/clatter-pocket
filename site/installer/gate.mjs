// The installer's gate: whether site/release.json allows installing on a device.
//
// One pure function, used by tools/build-site.mjs at build time and by the installer page in the
// browser, so both refuse for the same reasons. It fails closed: anything missing, malformed or
// inconsistent keeps installation off. A firmware file existing anywhere plays no part in it:
// the maintainer enables one named package, by hash.
//
// Two kinds of release can be installed. An experimental one has passed the build's checks and
// the emulator, and is how the first hardware test happens; it stays marked UNVERIFIED. A
// verified one has also passed on a device, and states that, by hash, for this exact package.
//
// Copyright (C) 2026 The CLATTER contributors. SPDX-License-Identifier: GPL-3.0-only

export const STATUS = Object.freeze({
  EMULATOR_ONLY: 'emulator-only',       // tested in the emulator; nothing has run on a device
  EXPERIMENTAL: 'experimental',         // installable for testing; not yet verified on a device
  HARDWARE_VERIFIED: 'hardware-verified',
});

export const NOT_AVAILABLE =
  'Hardware installation is not yet available. CLATTER Pocket is currently verified in the FM-1 emulator only.';

const SHA256 = /^[0-9a-f]{64}$/;
const DATE = /^\d{4}-\d{2}-\d{2}$/;
const FILENAME = /^clatter-pocket-[A-Za-z0-9.-]+\.fwsc$/;
// The update identities WebUpdate.js treats as Open firmware (openIdentity), except the two
// that name other projects' packages.
const PRODUCT = /^FM-1_8\d{6}$/;

/** Every reason this release may not be installed. Empty means it may. */
export function refusals(release) {
  const no = [];
  if (!release || typeof release !== 'object') return ['there is no release configuration'];
  const experimental = release.status === STATUS.EXPERIMENTAL;
  const verified = release.status === STATUS.HARDWARE_VERIFIED;
  if (release.schema !== 1) no.push('unknown release schema');
  if (!experimental && !verified) no.push(`status "${release.status}" cannot be installed`);
  if (release.installEnabled !== true) no.push('installEnabled is not true');
  if (typeof release.version !== 'string' || !release.version) no.push('no version');
  if (!DATE.test(release.releaseDate ?? '')) no.push('no release date');
  if (!Array.isArray(release.notes) || !release.notes.length) no.push('no release notes');

  const fw = release.firmware;
  if (!fw || typeof fw !== 'object') {
    no.push('no firmware named');
  } else {
    if (!FILENAME.test(fw.filename ?? '')) no.push('firmware filename is not a clatter-pocket-*.fwsc package');
    else if (experimental && !/UNVERIFIED/.test(fw.filename)) no.push('an experimental package must say UNVERIFIED in its name');
    else if (verified && /UNVERIFIED/i.test(fw.filename)) no.push('firmware filename says UNVERIFIED');
    if (!SHA256.test(fw.sha256 ?? '')) no.push('firmware has no SHA-256');
    if (!SHA256.test(fw.buildId ?? '')) no.push('firmware has no build identity');
    if (!SHA256.test(fw.loaderSha256 ?? '')) no.push('firmware has no loader SHA-256');
    if (!Number.isInteger(fw.bytes) || fw.bytes < 1024 || fw.bytes > 2097152) no.push('firmware has no plausible size');
    if (!PRODUCT.test(fw.product ?? '')) no.push('firmware has no fm1_open update identity');
    if (fw.family !== 7) no.push('firmware update family is not 7');

    // what tools/promote-release.mjs establishes before it names a package
    const checks = fw.checks;
    if (!checks || typeof checks !== 'object') {
      no.push('no record of the firmware checks');
    } else {
      if (!/^[0-9a-f]{7,40}$/.test(checks.commit ?? '')) no.push('the package names no commit');
      if (checks.singlePrecision !== true) no.push('not recorded as built --single-precision');
      if (checks.firmwareChecks !== true) no.push('check-firmware.py did not pass');
      if (checks.audioPath !== true) no.push('check-audio-path.py did not pass');
      if (checks.emulator !== true) no.push('the emulator suite did not pass');
    }
  }

  if (verified) {
    const proof = release.verification;
    if (!proof || typeof proof !== 'object') {
      no.push('no hardware verification record');
    } else {
      if (!fw || proof.firmwareSha256 !== fw.sha256) no.push('the verification record is for a different package');
      if (!DATE.test(proof.date ?? '')) no.push('verification has no date');
      if (!Number.isInteger(proof.units) || proof.units < 1) no.push('verification names no device');
      if (typeof proof.report !== 'string' || !proof.report.startsWith('https://')) no.push('verification links no report');
      if (proof.recoveryTested !== true) no.push('return to official firmware was not tested');
    }
  }

  const need = release.compatibility;
  if (!need || !Array.isArray(need.requires) || !need.requires.length) no.push('no compatibility requirements');
  return no;
}

export const installAllowed = release => refusals(release).length === 0;

/** The status as the site shows it. Only a verified release loses the word UNVERIFIED. */
export function statusLabel(release) {
  if (release?.status === STATUS.HARDWARE_VERIFIED && refusals(release).length === 0) return 'HARDWARE VERIFIED';
  if (release?.status === STATUS.EXPERIMENTAL) return 'UNVERIFIED: EXPERIMENTAL';
  return 'UNVERIFIED: EMULATOR ONLY';
}

/** The words on the install buttons. */
export const installLabel = release =>
  (release?.status === STATUS.EXPERIMENTAL && installAllowed(release) ? 'INSTALL EXPERIMENTAL FIRMWARE' : 'INSTALL FIRMWARE');

/** What this browser offers, from feature checks only. Nothing here asks for MIDI access. */
export function browserSupport(env) {
  const secure = env.isSecureContext === true;
  const webMidi = env.hasWebMidi === true;
  const mobile = /Android|iPhone|iPad|Mobile/i.test(env.userAgent ?? '');
  const message = !secure ? 'Web MIDI needs a secure page: open this page over HTTPS.'
    : !webMidi ? 'This browser has no Web MIDI. The FM-1 updater needs Chrome or Edge on a computer.'
    : mobile ? 'The FM-1 updater needs Chrome or Edge on a computer, not a phone or tablet.'
    : 'This browser has Web MIDI. M-VAVE and the community installers name Chrome and Edge.';
  return { secure, webMidi, mobile, ok: secure && webMidi && !mobile, message };
}
