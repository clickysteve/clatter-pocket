#!/usr/bin/env python3
"""Make a local installer page for a CLATTER Pocket FM-1 package.

    python3 tools/make-installer-page.py [build/fm1-replace]

Writes <dir>/clatter-pocket-installer-UNVERIFIED.html: one self-contained page that installs
the package in <dir> on an FM-1 over USB-MIDI (Chrome or Edge, Web MIDI with SysEx).

The update protocol is M-VAVE's, unchanged: the page embeds fm1_open's sdk/WebUpdate.js and
sdk/I18n.js byte for byte (their SHA-256 are printed in the page) and only drives
FM1Web.prepare, Updater.connect and Updater.install, as fm1_open's own workbench page does,
with the package description that sdk/Build.ps1 -Package builds (same fields; no stock V15
image: restore with M-VAVE's official workbench). It never uses the updater's force mode:
it installs only over firmware that reports an fm1_open identity and update family (stock
Open, or a CLATTER Pocket build), or resumes an interrupted install of this same package.

The page is NOT VERIFIED on hardware. FIRST-HARDWARE-TEST.md describes when to use it.
Nothing here talks to a device; only the generated page does, when a person uses it.

Copyright (C) 2026 The CLATTER contributors. SPDX-License-Identifier: GPL-3.0-only
"""
import base64
import glob
import hashlib
import html
import json
import os
import pathlib
import re
import sys

root = pathlib.Path(__file__).resolve().parents[1]
build = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else root / 'build/fm1-replace')
sdk = root / 'third_party/fm1_open/sdk'


def sha(b):
    return hashlib.sha256(b).hexdigest()


report = json.loads((build / 'firmware-report.json').read_text())
layout = json.loads((build / 'tree/build/dx7/package-report.json').read_text())
identity = json.loads((build / 'tree/build/dx7/build-identity.json').read_text(encoding='utf-8-sig'))
if not report.get('passed') or not layout.get('fits'):
    raise SystemExit('the firmware checks or the packager did not pass: no installer page')
packages = sorted(glob.glob(str(build / '*-UNVERIFIED.fwsc')))
if len(packages) != 1:
    raise SystemExit(f'expected one *-UNVERIFIED.fwsc in {build}')
package = pathlib.Path(packages[0])
data = package.read_bytes()
version_h = (build / 'tree/app/version.h').read_text()
product = re.search(r'FM1_UPDATE_PRODUCT "([^"]+)"', version_h).group(1)
release = re.search(r'FM1_RELEASE_VERSION "([^"]+)"', version_h).group(1)
webupdate = (sdk / 'WebUpdate.js').read_bytes()
i18n = (sdk / 'I18n.js').read_bytes()
loader = (sdk / 'tools/pack/ota.bin').read_bytes()

meta = {
    'schema': 1, 'product': product, 'profile': identity['profile'], 'family': 7,
    'release_version': release, 'build_id': identity['build_id'], 'sha256': sha(data),
    'bytes': len(data), 'loader_sha256': sha(loader), 'hardware_validated': False,
    'data': base64.b64encode(data).decode('ascii'),
}
facts = [
    ('Package', package.name), ('Package SHA-256', meta['sha256']), ('Bytes', str(len(data))),
    ('Update identity', product), ('Build identity', meta['build_id']),
    ('Firmware image', f"{report['app_image_bytes']} bytes; flash layout as stock fm1_open V14 "
                       f"(VM {layout['VM_START_ADDR']}, {layout['VM_REAL_SIZE']} bytes; user partition {layout['USER_RESERVED_START']})"),
    ('Embedded sdk/WebUpdate.js SHA-256', sha(webupdate)), ('Embedded sdk/I18n.js SHA-256', sha(i18n)),
    ('Loader (sdk/tools/pack/ota.bin) SHA-256', meta['loader_sha256']),
]
rows = '\n'.join(f'<tr><th>{html.escape(k)}</th><td><code>{html.escape(v)}</code></td></tr>' for k, v in facts)

page = f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>CLATTER Pocket installer</title>
<style>
:root{{--ink:#111;--paper:#fff;--line:#bbb;--warn:#a40000}}
body{{margin:0;padding:16px;background:var(--paper);color:var(--ink);font:15px/1.45 system-ui,sans-serif;max-width:860px}}
h1{{font-size:20px;margin:0 0 4px}} .warn{{color:var(--warn);font-weight:600}}
table{{border-collapse:collapse;width:100%;margin:12px 0;font-size:13px}} th,td{{text-align:left;padding:4px 6px;border-bottom:1px solid var(--line);vertical-align:top}}
code{{word-break:break-all}} button{{font:inherit;padding:8px 14px;margin:4px 8px 4px 0}} #log{{white-space:pre-wrap;background:#f4f4f4;padding:8px;min-height:6em;font:12px/1.4 ui-monospace,monospace}}
</style></head><body>
<h1>CLATTER Pocket installer</h1>
<p class="warn">UNVERIFIED: this firmware and this page have never run on an FM-1. Follow FIRST-HARDWARE-TEST.md.
Installing replaces the FM-1's instrument; projects, samples and voices stay in the device's user flash but cannot be
backed up while CLATTER Pocket is installed. Restore with M-VAVE's official workbench.</p>
<table>{rows}</table>
<p><label><input type="checkbox" id="backup"> I have backed up projects, samples and the DX7 library with M-VAVE's
official workbench, and I can reinstall the official firmware from it.</label></p>
<p><button id="connect">Connect FM-1</button><button id="install" disabled>Install CLATTER Pocket</button></p>
<div id="log"></div>
<script>
const FM1_PACKAGE = {json.dumps(meta)};
</script>
<script>
{i18n.decode('utf-8')}
</script>
<script>
{webupdate.decode('utf-8')}
</script>
<script>
(() => {{
  'use strict';
  const $ = id => document.getElementById(id);
  const t = text => (globalThis.FM1I18n ? FM1I18n.t(String(text)) : String(text));
  const log = line => {{ $('log').textContent += t(line) + '\\n'; }};
  let pkg, updater, device, busy = false, writing = false;
  const enable = () => {{
    $('connect').disabled = busy || !pkg;
    $('install').disabled = busy || !device || !device.ok || !$('backup').checked;
  }};
  $('backup').addEventListener('change', enable);
  window.addEventListener('beforeunload', event => {{ if (writing) {{ event.preventDefault(); event.returnValue = ''; }} }});
  (async () => {{
    try {{ pkg = await FM1Web.prepare(FM1_PACKAGE); log('Package verified (SHA-256, identity, loader).'); }}
    catch (error) {{ log('Package check failed: ' + error.message); }}
    enable();
  }})();
  $('connect').onclick = async () => {{
    busy = true; device = null; enable();
    try {{
      if (!navigator.requestMIDIAccess) throw Error('This browser has no Web MIDI: use Chrome or Edge on a computer.');
      const access = await navigator.requestMIDIAccess({{ sysex: true }});
      if (!access.sysexEnabled) throw Error('Allow MIDI SysEx for this page.');
      if (updater) await updater.disconnect();
      updater = new FM1Web.Updater(access, pkg, (phase, percent) => log(t(phase) + (percent === undefined ? '' : ' ' + percent + ' %')));
      const result = await updater.connect({{ force: false }});
      log('Connected: ' + result.identity);
      if (result.migration || result.legacy)
        log('Not installing: this FM-1 does not run fm1_open firmware. Install M-VAVE\\'s official Open firmware with the official workbench first.');
      else if (result.same) log('This build is already installed.');
      else if (result.resume) {{ log('The FM-1 is in its update loader: Install resumes this package.'); device = {{ ok: true }}; }}
      else {{ log('Ready to install over ' + result.identity + ' (build ' + String(result.current).slice(0, 16) + ').'); device = {{ ok: true }}; }}
    }} catch (error) {{ log('Connect failed: ' + t(error.message)); }}
    finally {{ busy = false; enable(); }}
  }};
  $('install').onclick = async () => {{
    if (!confirm('Install CLATTER Pocket (UNVERIFIED)? Keep the FM-1 connected and powered until the page says it has finished.')) return;
    busy = true; writing = true; enable();
    try {{ await updater.install({{ force: false }}); log('Installed and verified: the FM-1 reports this build.'); }}
    catch (error) {{ log('Install did not finish: ' + t(error.message)); log('Keep this page open; reconnect the same FM-1 and use Connect, then Install, to resume.'); }}
    finally {{ busy = false; writing = false; device = null; enable(); }}
  }};
}})();
</script>
</body></html>
"""
out = build / 'clatter-pocket-installer-UNVERIFIED.html'
out.write_text(page, encoding='utf-8')
print(f'{out.relative_to(root)}: {len(page)} bytes, SHA-256 {sha(page.encode())}')
print(f'  package {package.name} {meta["sha256"]}, WebUpdate.js {sha(webupdate)[:16]}, I18n.js {sha(i18n)[:16]}')
