// The update driver: only in a site whose release passed the gate (tools/build-site.mjs leaves
// this file out otherwise, with M-VAVE's update code and the firmware).
//
// It is tools/make-installer-page.py's page logic, the route FIRST-HARDWARE-TEST.md follows, with
// a first stage in front of it. The protocol is M-VAVE's sdk/WebUpdate.js, unchanged: this only
// drives FM1Web.prepare, Updater.connect and Updater.install, as fm1_open's workbench does.
//
// CLATTER Pocket installs only over firmware reporting an fm1_open identity and update family, or
// resumes an interrupted install in the vendor's update loader. Force mode is used in two places:
// official Open over stock V15, which is the route M-VAVE's manual gives, because stock V15 has no
// capability command to satisfy the normal route; and a loader found at Connect under a name the
// unforced route does not take, which force reaches as M-VAVE's workbench does.
//
// A real FM-1 came back from the handover as ota-FM1C_0000000000000000, not the ota-FM-1_015 the
// updater waits for, and the install timed out. Why is not known. So after the updater's own
// handover any ota- loader is taken, as tools/make-installer-page.py's page does.
//
// Copyright (C) 2026 The CLATTER contributors. SPDX-License-Identifier: GPL-3.0-only

const script = src => new Promise((resolve, reject) => {
  const s = Object.assign(document.createElement('script'), { src, onload: resolve, onerror: () => reject(new Error(`${src} did not load`)) });
  document.head.append(s);
});

/** The package description sdk/Build.ps1 -Package builds, from the release configuration. */
export const packageMeta = release => ({
  schema: 1, product: release.firmware.product, profile: 'dx7', family: release.firmware.family,
  release_version: release.version, build_id: release.firmware.buildId, sha256: release.firmware.sha256,
  bytes: release.firmware.bytes, loader_sha256: release.firmware.loaderSha256,
  hardware_validated: release.status === 'hardware-verified',
  url: `firmware/${release.firmware.filename}`,
});

/** The same description of the pinned official Open firmware, as M-VAVE's workbench holds it. */
export const openPackageMeta = ({ openFirmware: open }) => ({
  schema: 1, product: open.product, profile: 'dx7', family: open.family, release_version: open.version,
  build_id: open.buildId, sha256: open.sha256, bytes: open.bytes, loader_sha256: open.loaderSha256,
  hardware_validated: false, url: `firmware/${open.filename}`,
});

export const NOT_IDENTIFIED =
  'This FM-1 is running firmware this installer cannot identify, so nothing was installed. Install official Open or V15 with M-VAVE\'s workbench first, check that it works, then come back here.';
export const NO_OPEN =
  'The official Open package failed its checks, so this page cannot prepare the FM-1, and nothing was installed. Reload the page, or install official Open with M-VAVE\'s workbench.';

/** What a person should do after a failure. Each one starts by finding out what the device is. */
export const RECOVERY = {
  connect: 'Nothing was written. Check the cable, close other MIDI software, and press Connect FM-1 again.',
  open: 'Nothing will be retried for you. Keep this page open, check the cable, and press Connect FM-1: it asks the FM-1 what it is running before offering anything. Still on stock, Stage 1 starts again. In its update loader, Stage 1 resumes. If it shows neither, follow the recovery notes below.',
  clatter: 'Nothing will be retried for you. Keep this page open, check the cable, and press Connect FM-1: it asks the FM-1 what it is running before offering anything. In its update loader, the install resumes. If it shows nothing, follow the recovery notes below.',
};

/** What the FM-1 reports it is running. */
export const FIRMWARE = Object.freeze({
  STOCK: 'stock',                     // M-VAVE's V15 identity, and no build to check
  OPEN: 'open',                       // the pinned official Open build, by hash
  OPEN_COMPATIBLE: 'open-compatible', // an fm1_open identity and family, on some other build
  CLATTER: 'clatter',                 // this release, by hash
  LOADER: 'loader',                   // the vendor's update loader, waiting for an image
  UNKNOWN: 'unknown',
});
const STOCK_IDENTITY = 'FM-1_015';
const LOADER_NAME = /^ota-/i;

/**
 * From the updater's normal, unforced connect. CLATTER Pocket reports Open's identity on purpose
 * (FM1-COMMUNITY.md), so only the build hash tells the two apart. Stock V15 has no hash to read.
 */
export function classify(result, clatter, open) {
  if (result.resume) return FIRMWARE.LOADER;
  if (result.migration && LOADER_NAME.test(result.identity)) return FIRMWARE.LOADER;
  if (result.migration || result.legacy) return result.identity === STOCK_IDENTITY ? FIRMWARE.STOCK : FIRMWARE.UNKNOWN;
  if (result.same) return FIRMWARE.CLATTER;
  return result.current === open?.build_id ? FIRMWARE.OPEN : FIRMWARE.OPEN_COMPATIBLE;
}

// Force mode also drops the updater's port-name filter, so it would ask every MIDI device on the
// computer what it is. This view hides them, leaving what the unforced route looks at for a loader.
const FM1_PORT = /fm.?1|felucca|sloop|clipper|ota|sinco|usb.?midi|composite/i;
const fm1Ports = access => ({
  get inputs() { return new Map([...access.inputs].filter(([, port]) => FM1_PORT.test(port.name || ''))); },
  get outputs() { return access.outputs; },
  sysexEnabled: access.sysexEnabled,
});

/**
 * Which package is being written, kept across a reload. Both packages use the vendor's loader,
 * which reports one fixed identity, so a device left in it cannot say which it was receiving.
 */
export function pageMemory(storage, key = 'clatter-pocket-install') {
  const attempt = (fn, fallback) => { try { return fn(); } catch { return fallback; } };
  return {
    read: () => attempt(() => JSON.parse(storage.getItem(key)), null),
    write: value => attempt(() => storage.setItem(key, JSON.stringify(value))),
    clear: () => attempt(() => storage.removeItem(key)),
  };
}

/**
 * One device conversation, from Connect to the end of an install.
 * States: idle, connecting, prepare (stock: Stage 1 offered), recover (in the loader: Stage 1
 * resumes), installing-open, ready (Stage 2 offered), installing, blocked, done, failed.
 * `ui` is { state(name, text), log(text), phase(text, percent) }. `packages` is { clatter, open },
 * and open may be null. Nothing here writes without the confirmed argument from a person, and
 * nothing retries: after a failure, Connect finds out what the device is.
 */
export class Session {
  constructor(FM1Web, access, packages, ui, translate = String, timing = {}, memory = pageMemory(globalThis.localStorage)) {
    Object.assign(this, { FM1Web, access, packages, ui, t: translate, timing, memory, state: 'idle', firmware: null, failedStage: null, forced: false });
    this.updater = this.updaterFor(packages.clatter);
  }

  updaterFor(pkg, access = this.access) {
    const updater = new this.FM1Web.Updater(access, pkg, (phase, percent) => this.ui.phase(this.t(phase), percent), this.timing);
    // After its own handover, any ota- loader: the one wait for the package's loader is widened.
    const handover = updater.waitFor.bind(updater);
    updater.waitFor = (identity, anyLoader) => handover(identity, anyLoader || identity === pkg.loaderIdentity);
    return updater;
  }

  go(state, text) {
    this.state = state;
    this.ui.log(text);
    this.ui.state(state, text);
  }

  fail(stage, text) {
    this.failedStage = stage;
    this.go('failed', text);
  }

  async connect() {
    const { clatter, open } = this.packages;
    this.failedStage = null;
    this.forced = false;
    this.go('connecting', 'Connecting…');
    let result;
    try {
      await this.updater.disconnect();
      this.updater = this.updaterFor(clatter);
      result = await this.updater.connect({ force: false });
    } catch (error) {
      this.fail('connect', `Connect failed: ${this.t(error.message)}`);
      return false;
    }
    this.ui.log(`Connected: ${result.identity}`);
    this.firmware = classify(result, clatter, open);
    if (this.firmware !== FIRMWARE.LOADER) this.memory.clear();

    const build = String(result.current).slice(0, 16);
    if (this.firmware === FIRMWARE.STOCK) this.go(open ? 'prepare' : 'blocked', open ? `Stock firmware detected: it reports ${result.identity}.` : NO_OPEN);
    else if (this.firmware === FIRMWARE.UNKNOWN) this.go('blocked', `${NOT_IDENTIFIED} It reports ${result.identity}.`);
    else if (this.firmware === FIRMWARE.CLATTER) this.go('blocked', 'This build is already installed.');
    else if (this.firmware === FIRMWARE.LOADER) await this.inLoader(!result.resume);
    else if (this.firmware === FIRMWARE.OPEN) this.go('ready', `Official Open ${open.release_version} detected. Ready to install over ${result.identity} (build ${build}).`);
    else this.go('ready', `Ready to install over ${result.identity} (build ${build}). This is fm1_open firmware, but not official Open ${open?.release_version ?? ''} or this release.`);

    if (this.state !== 'ready' && this.state !== 'recover') await this.updater.disconnect().catch(() => {});
    return ['ready', 'prepare', 'recover'].includes(this.state);
  }

  /**
   * Resume what this page was writing. With no record of that, official Open is the safe one.
   * `renamed`: the loader reports a name the unforced route does not take, so it is reached by force.
   */
  async inLoader(renamed) {
    const { clatter, open } = this.packages, writing = this.memory.read();
    const resuming = writing?.stage === 'clatter' && writing.sha256 === clatter.sha256;
    if (resuming && !renamed) {
      this.go('ready', 'The FM-1 is in its update loader: Install resumes this package.');
      return;
    }
    if (!resuming && !open) { this.go('blocked', NO_OPEN); return; }
    try {
      await this.updater.disconnect();
      this.forced = renamed;
      this.updater = this.updaterFor(resuming ? clatter : open, renamed ? fm1Ports(this.access) : this.access);
      if (!(await this.updater.connect({ force: renamed })).resume) throw new Error('The FM-1 has left its update loader.');
      if (resuming) this.go('ready', 'The FM-1 is in its update loader: Install resumes this package.');
      else this.go('recover', `The FM-1 is in its update loader, so an install was interrupted. This page will finish official Open ${open.release_version} first.`);
    } catch (error) {
      this.fail('connect', `Connect failed: ${this.t(error.message)}`);
    }
  }

  /** Stage 1: official Open, over stock firmware or from the loader. */
  async installOpen(confirmed) {
    if (this.state !== 'prepare' && this.state !== 'recover') throw new Error('No device is ready.');
    if (confirmed !== true) throw new Error('Installing official Open needs explicit confirmation.');
    const { open } = this.packages, resuming = this.state === 'recover';
    this.go('installing-open', 'Installing official Open. Do not unplug the FM-1.');
    try {
      if (resuming) {
        this.memory.write({ stage: 'open', sha256: open.sha256 });
        await this.updater.install({ force: this.forced });
      } else {
        // The connect just before this was unforced, and writes nothing either. Ask again now
        // that force is on: only the exact stock report may go on, not a loader or anything else.
        this.updater = this.updaterFor(open, fm1Ports(this.access));
        const device = await this.updater.connect({ force: true });
        if (!device.legacy || device.identity !== STOCK_IDENTITY) {
          await this.updater.disconnect();
          this.go('blocked', `The FM-1 now reports ${device.identity}, not stock firmware. Nothing was written. Press Connect FM-1 again.`);
          return false;
        }
        this.memory.write({ stage: 'open', sha256: open.sha256 });
        await this.updater.install({ force: true });
      }
    } catch (error) {
      this.fail('open', `Official Open did not finish: ${this.t(error.message)}`);
      return false;
    }
    this.memory.clear();
    this.ui.log(`Official Open ${open.release_version} restarted and reports its exact build.`);

    // A fresh, unforced look, as if a person had pressed Connect. Stage 2 stops at its confirmation.
    await this.connect();
    if (this.firmware === FIRMWARE.OPEN && this.state === 'ready') return true;
    await this.updater.disconnect().catch(() => {});
    if (this.state !== 'failed') this.fail('open', `The FM-1 restarted but does not report official Open ${open.release_version}, so Stage 2 is not offered.`);
    return false;
  }

  /** Stage 2: CLATTER Pocket. */
  async install(confirmed) {
    if (this.state !== 'ready') throw new Error('No device is ready.');
    if (confirmed !== true) throw new Error('Installing CLATTER Pocket needs explicit confirmation.');
    this.go('installing', 'Installing. Do not unplug the FM-1.');
    this.memory.write({ stage: 'clatter', sha256: this.packages.clatter.sha256 });
    try {
      await this.updater.install({ force: this.forced });
    } catch (error) {
      this.fail('clatter', `Install did not finish: ${this.t(error.message)}`);
      return false;
    }
    this.memory.clear();
    this.go('done', 'Installed: the FM-1 restarted and reports this build.');
    return true;
  }
}

export async function start({ release, support, $ }) {
  const stopped = text => {
    $('package-state').textContent = text;
    $('package-state').classList.add('stopped');
  };
  try {
    await script('I18n.js');
    await script('WebUpdate.js');
  } catch (error) {
    stopped(`The update code could not load, so nothing can be installed: ${error.message}`);
    return;
  }
  const FM1Web = globalThis.FM1Web;
  const t = text => (globalThis.FM1I18n ? globalThis.FM1I18n.t(String(text)) : String(text));

  const packages = { clatter: null, open: null };
  try {
    packages.clatter = await FM1Web.prepare(packageMeta(release));
  } catch (error) {
    stopped(`The package failed its checks, so nothing can be installed: ${t(error.message)}`);
    return;
  }
  try {
    packages.open = await FM1Web.prepare(openPackageMeta(release));
    $('package-state').textContent = `Both packages verified: SHA-256, format, update identity and loader. Official Open ${release.openFirmware.version} is M-VAVE's, unchanged.`;
  } catch (error) {
    stopped(`CLATTER Pocket verified. Official Open failed its checks, so a stock FM-1 cannot be prepared here: ${t(error.message)}`);
  }

  let session = null;
  const shown = { open: false };
  const buttons = () => {
    const state = session?.state ?? 'idle';
    const ready = state === 'ready', preparing = state === 'prepare' || state === 'recover';
    const busy = state === 'connecting' || state === 'installing' || state === 'installing-open';
    if (preparing) shown.open = true;
    $('connect').disabled = busy || !support.ok;
    $('stage-open').hidden = !shown.open;
    $('confirm-open').disabled = busy || !preparing;
    if (!preparing) $('confirm-open').checked = false;
    $('install-open').disabled = busy || !preparing || !$('confirm-open').checked;
    $('confirm').disabled = busy || !ready;
    if (!ready) $('confirm').checked = false;
    $('install').disabled = busy || !ready || !$('confirm').checked;
  };
  const ui = {
    log(text) { $('log').hidden = false; $('log').textContent += `${t(text)}\n`; },
    phase(text, percent) {
      const [progress, phase, bar] = session?.state === 'installing-open' ? ['progress-open', 'phase-open', 'bar-open'] : ['progress', 'phase', 'bar'];
      $(progress).hidden = false;
      $(phase).textContent = percent === undefined ? text : `${text} ${percent} %`;
      if (percent !== undefined) $(bar).style.width = `${percent}%`;
    },
    state(name, text) {
      $('device-state').textContent = text;
      $('device-state').dataset.state = name;
      if (name === 'recover') $('open-detected').textContent = 'Update loader detected';
      if (name === 'prepare') $('open-detected').textContent = 'Stock firmware detected';
      const openRunning = session?.firmware === FIRMWARE.OPEN;
      $('after-open').hidden = !(shown.open && openRunning && name === 'ready');
      $('after-failure').hidden = name !== 'failed';
      if (name === 'failed') $('after-failure-text').textContent = RECOVERY[session?.failedStage ?? 'connect'];
      $('after-done').hidden = name !== 'done';
      buttons();
    },
  };

  addEventListener('beforeunload', event => {
    if (session?.state === 'installing' || session?.state === 'installing-open') { event.preventDefault(); event.returnValue = ''; }
  });
  $('confirm').addEventListener('change', buttons);
  $('confirm-open').addEventListener('change', buttons);

  $('connect').addEventListener('click', async () => {
    try {
      if (!navigator.requestMIDIAccess) throw new Error('This browser has no Web MIDI: use Chrome or Edge on a computer.');
      const access = await navigator.requestMIDIAccess({ sysex: true });
      if (!access.sysexEnabled) throw new Error('Allow MIDI SysEx for this page.');
      await session?.updater.disconnect().catch(() => {});
      session = new Session(FM1Web, access, packages, ui, t);
      await session.connect();
    } catch (error) {
      session = null;
      ui.state('failed', `Connect failed: ${t(error.message)}`);
    }
    buttons();
  });

  $('install-open').addEventListener('click', async () => {
    if ((session?.state !== 'prepare' && session?.state !== 'recover') || !$('confirm-open').checked) return;
    await session.installOpen(true);
    buttons();
  });

  $('install').addEventListener('click', async () => {
    if (session?.state !== 'ready' || !$('confirm').checked) return;
    await session.install(true);
    buttons();
  });
  buttons();
}
