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
  bytes: release.firmware.bytes, loader_sha256: release.firmware.loaderSha256, hardware_validated: true,
  url: `firmware/${release.firmware.filename}`,
});

/** One device conversation. `ui` is { log(text), phase(text, percent), ready(ok) }. */
export class Session {
  constructor(FM1Web, access, pkg, ui, translate = String) {
    this.t = translate;
    this.ui = ui;
    this.updater = new FM1Web.Updater(access, pkg, (phase, percent) => ui.phase(this.t(phase), percent));
    this.ok = false;
  }

  async connect() {
    this.ok = false;
    const result = await this.updater.connect({ force: false });
    this.ui.log(`Connected: ${result.identity}`);
    if (result.migration || result.legacy) {
      this.ui.log("Not installing: this FM-1 does not run fm1_open firmware. Install M-VAVE's official Open firmware with the official workbench first.");
    } else if (result.same) {
      this.ui.log('This build is already installed.');
    } else if (result.resume) {
      this.ui.log('The FM-1 is in its update loader: Install resumes this package.');
      this.ok = true;
    } else {
      this.ui.log(`Ready to install over ${result.identity} (build ${String(result.current).slice(0, 16)}).`);
      this.ok = true;
    }
    this.ui.ready(this.ok);
    return this.ok;
  }

  async install() {
    if (!this.ok) throw new Error('No device is ready.');
    await this.updater.install({ force: false });
    this.ok = false;
    this.ui.ready(false);
  }
}

export async function start({ release, support, $ }) {
  await script('I18n.js');
  await script('WebUpdate.js');
  const t = text => (globalThis.FM1I18n ? globalThis.FM1I18n.t(String(text)) : String(text));
  const logLine = text => { $('log').hidden = false; $('log').textContent += `${t(text)}\n`; };

  const fail = text => { $('notice-head').textContent = 'INSTALLATION STOPPED'; $('notice-body').textContent = text; };
  let pkg;
  try {
    pkg = await globalThis.FM1Web.prepare(packageMeta(release));
  } catch (error) {
    fail(`The package failed its checks, so nothing can be installed: ${t(error.message)}`);
    return;
  }

  let session = null, busy = false, writing = false;
  const buttons = () => {
    $('connect').disabled = busy || !support.ok;
    $('backup').disabled = busy || !session?.ok;
    $('install').disabled = busy || !session?.ok || !$('backup').checked;
  };
  const ui = {
    log: logLine,
    phase(text, percent) {
      $('progress').hidden = false;
      $('phase').textContent = percent === undefined ? text : `${text} ${percent} %`;
      if (percent !== undefined) $('bar').style.width = `${percent}%`;
    },
    ready(ok) { $('device-state').textContent = ok ? 'Ready.' : 'Not ready: see the log below.'; buttons(); },
  };

  $('notice-head').textContent = 'READY TO CONNECT';
  $('notice-body').textContent = `Package verified: ${release.firmware.filename}.`;
  $('notice-detail').textContent = 'Read each step before you start. Keep the FM-1 connected and powered until this page says it has finished.';
  $('strip-status').nextElementSibling.textContent = 'Hardware-verified release.';
  $('fact-package').textContent = `${release.firmware.filename}, ${release.firmware.bytes} bytes`;
  $('fact-sha').firstElementChild.textContent = release.firmware.sha256;
  $('why').hidden = true;
  addEventListener('beforeunload', event => { if (writing) { event.preventDefault(); event.returnValue = ''; } });
  $('backup').addEventListener('change', buttons);

  $('connect').addEventListener('click', async () => {
    busy = true; buttons();
    try {
      if (!navigator.requestMIDIAccess) throw new Error('This browser has no Web MIDI: use Chrome or Edge on a computer.');
      const access = await navigator.requestMIDIAccess({ sysex: true });
      if (!access.sysexEnabled) throw new Error('Allow MIDI SysEx for this page.');
      if (session) await session.updater.disconnect();
      session = new Session(globalThis.FM1Web, access, pkg, ui, t);
      await session.connect();
    } catch (error) {
      logLine(`Connect failed: ${t(error.message)}`);
    } finally { busy = false; buttons(); }
  });

  $('install').addEventListener('click', async () => {
    if (!confirm(`Install CLATTER Pocket ${release.version}? Keep the FM-1 connected and powered until the page says it has finished.`)) return;
    busy = true; writing = true; buttons();
    try {
      await session.install();
      logLine('Installed and verified: the FM-1 reports this build.');
    } catch (error) {
      logLine(`Install did not finish: ${t(error.message)}`);
      logLine('Keep this page open; reconnect the same FM-1 and use Connect, then Install, to resume.');
    } finally { busy = false; writing = false; buttons(); }
  });
  buttons();
}
