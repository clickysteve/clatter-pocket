// The installer page. It reads site/release.json and asks the gate; the update driver is only
// loaded if the gate is open, and only exists in a site built with the gate open
// (tools/build-site.mjs). Any failure here leaves the buttons as the page ships them: disabled.
// Copyright (C) 2026 The CLATTER contributors. SPDX-License-Identifier: GPL-3.0-only

import { browserSupport, installAllowed, refusals, statusLabel } from './gate.mjs';

const $ = id => document.getElementById(id);

const support = browserSupport({
  isSecureContext: globalThis.isSecureContext,
  hasWebMidi: 'requestMIDIAccess' in navigator,
  userAgent: navigator.userAgent,
});
$('fact-browser').textContent = support.message;

function showRefusals(reasons) {
  $('why').hidden = false;
  $('why-list').replaceChildren(...reasons.map(text => {
    const li = document.createElement('li');
    li.textContent = text;
    return li;
  }));
}

let release = null;
try {
  const response = await fetch('../release.json', { cache: 'no-store' });
  if (!response.ok) throw new Error(`release.json: ${response.status}`);
  release = await response.json();
} catch (error) {
  showRefusals([`The release configuration could not be read: ${error.message}`]);
}

if (release) {
  $('fact-version').textContent = release.version ?? 'unknown';
  $('fact-status').textContent = statusLabel(release);
}

if (installAllowed(release)) {
  const { start } = await import('./driver.js');
  await start({ release, support, $ });
} else if (release) {
  showRefusals(refusals(release));
  $('package-state').textContent = 'Stopped: this release may not be installed.';
}
