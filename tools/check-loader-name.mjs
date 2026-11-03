#!/usr/bin/env node
// The offline installer's handover, against a simulated FM-1 whose loader has the name a real one came
// back with: refused without the page's change, installed with it, and resumed when found at Connect.
// Copyright (C) 2026 The CLATTER contributors. SPDX-License-Identifier: GPL-3.0-only
import { readFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { loadFM1Web } from './fm1-web.mjs';
import { SimulatedFM1, simulatedAccess } from './fm1-update-sim.mjs';
const FM1Web = loadFM1Web();
const html = readFileSync('build/test-0.9.1/clatter-pocket-installer-UNVERIFIED.html', 'utf8');
const meta = JSON.parse(/const FM1_PACKAGE = (\{.*\});/.exec(html)[1]);
const pkg = await FM1Web.prepare(meta);
const options = { imageBytes: pkg.image.length, newBuild: pkg.build_id, build: 'ab'.repeat(32), identity: 'FM-1_8000014', loaderName: 'ota-FM1C_0000000000000000' };
const timing = { reconnect: 2000, settle: 20, idle: 4000, writeIdle: 4000, pace: 0 };
const updater = access => { const u = new FM1Web.Updater(access, pkg, () => {}, timing); u.timing = { ...u.timing, ...timing }; return u; };
const accept = u => { const h = u.waitFor.bind(u); u.waitFor = (id, any) => h(id, any || id === pkg.loaderIdentity); return u; };
let failed = 0;
const say = (ok, what) => { failed += !ok; console.log((ok ? 'ok   ' : 'FAIL ') + what); };
{ const a = simulatedAccess(); new SimulatedFM1(FM1Web, a, options); const u = updater(a); await u.connect({ force: false });
  const r = await u.install({ force: false }).then(() => 'done', e => e.message); say(r !== 'done', `as published it times out: ${r}`); }
{ const a = simulatedAccess(); new SimulatedFM1(FM1Web, a, options); const u = accept(updater(a)); await u.connect({ force: false });
  const r = await u.install({ force: false }).then(() => 'done', e => e.message); say(r === 'done', `with the change it installs: ${r}`); }
{ const a = simulatedAccess(); new SimulatedFM1(FM1Web, a, { ...options, mode: 'loader' }); const u = accept(updater(a));
  let c = await u.connect({ force: false }); const seen = c.migration; await u.disconnect(); c = await u.connect({ force: true });
  const r = await u.install({ force: true }).then(() => 'done', e => e.message); say(seen && c.resume && r === 'done', `found in that loader at Connect it resumes: ${r}`); }
process.exit(failed ? 1 : 0);
