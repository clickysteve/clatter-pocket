// The update driver: only in a site whose release passed the gate (tools/build-site.mjs leaves
// this file out otherwise, with M-VAVE's update code and the firmware).
//
// It is tools/make-installer-page.py's page logic, the route FIRST-HARDWARE-TEST.md follows. The
// protocol is M-VAVE's sdk/WebUpdate.js, unchanged: this only drives FM1Web.prepare,
// Updater.connect and Updater.install, as fm1_open's workbench does. It never uses the updater's
// force mode, so it installs only over firmware reporting an fm1_open identity and update family,
// or resumes an interrupted install of this same package.
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

export const NOT_OPEN =
  "This FM-1 is not running official fm1_open firmware, so nothing was installed. Install official Open with M-VAVE's workbench first, check that it works, then come back here.";

/**
 * One device conversation, from Connect to the end of an install.
 * States: idle, connecting, ready, blocked, installing, done, failed.
 * `ui` is { state(name, text), log(text), phase(text, percent) }.
 */
export class Session {
  constructor(FM1Web, access, pkg, ui, translate = String, timing = {}) {
    this.t = translate;
    this.ui = ui;
    this.updater = new FM1Web.Updater(access, pkg, (phase, percent) => ui.phase(this.t(phase), percent), timing);
    this.state = 'idle';
  }

  go(state, text) {
    this.state = state;
    this.ui.log(text);
    this.ui.state(state, text);
  }

  async connect() {
    this.go('connecting', 'Connecting…');
    let result;
    try {
      result = await this.updater.connect({ force: false });
    } catch (error) {
      this.go('failed', `Connect failed: ${this.t(error.message)}`);
      return false;
    }
    this.ui.log(`Connected: ${result.identity}`);
    if (result.migration || result.legacy) this.go('blocked', `${NOT_OPEN} It reports ${result.identity}.`);
    else if (result.same) this.go('blocked', 'This build is already installed.');
    else if (result.resume) this.go('ready', 'The FM-1 is in its update loader: Install resumes this package.');
    else this.go('ready', `Ready to install over ${result.identity} (build ${String(result.current).slice(0, 16)}).`);
    if (this.state === 'blocked') await this.updater.disconnect().catch(() => {});
    return this.state === 'ready';
  }

  async install() {
    if (this.state !== 'ready') throw new Error('No device is ready.');
    this.go('installing', 'Installing. Do not unplug the FM-1.');
    try {
      await this.updater.install({ force: false });
    } catch (error) {
      this.go('failed', `Install did not finish: ${this.t(error.message)}`);
      return false;
    }
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

  let pkg;
  try {
    pkg = await FM1Web.prepare(packageMeta(release));
  } catch (error) {
    stopped(`The package failed its checks, so nothing can be installed: ${t(error.message)}`);
    return;
  }
  $('package-state').textContent = 'Package verified: SHA-256, format, update identity and loader.';

  let session = null, busy = false;
  const buttons = () => {
    const state = session?.state ?? 'idle';
    const ready = state === 'ready';
    busy = state === 'connecting' || state === 'installing';
    $('connect').disabled = busy || !support.ok;
    $('confirm').disabled = busy || !ready;
    if (!ready) $('confirm').checked = false;
    $('install').disabled = busy || !ready || !$('confirm').checked;
  };
  const ui = {
    log(text) { $('log').hidden = false; $('log').textContent += `${t(text)}\n`; },
    phase(text, percent) {
      $('progress').hidden = false;
      $('phase').textContent = percent === undefined ? text : `${text} ${percent} %`;
      if (percent !== undefined) $('bar').style.width = `${percent}%`;
    },
    state(name, text) {
      $('device-state').textContent = text;
      $('device-state').dataset.state = name;
      $('after-failure').hidden = name !== 'failed';
      $('after-done').hidden = name !== 'done';
      buttons();
    },
  };

  addEventListener('beforeunload', event => {
    if (session?.state === 'installing') { event.preventDefault(); event.returnValue = ''; }
  });
  $('confirm').addEventListener('change', buttons);

  $('connect').addEventListener('click', async () => {
    try {
      if (!navigator.requestMIDIAccess) throw new Error('This browser has no Web MIDI: use Chrome or Edge on a computer.');
      const access = await navigator.requestMIDIAccess({ sysex: true });
      if (!access.sysexEnabled) throw new Error('Allow MIDI SysEx for this page.');
      await session?.updater.disconnect().catch(() => {});
      session = new Session(FM1Web, access, pkg, ui, t);
      await session.connect();
    } catch (error) {
      session = null;
      ui.state('failed', `Connect failed: ${t(error.message)}`);
    }
    buttons();
  });

  $('install').addEventListener('click', async () => {
    if (session?.state !== 'ready' || !$('confirm').checked) return;
    await session.install();
    buttons();
  });
  buttons();
}
