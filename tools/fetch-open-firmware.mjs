#!/usr/bin/env node
// Get M-VAVE's official Open firmware for the installer, at deployment. No dependencies.
//
//   node tools/fetch-open-firmware.mjs [--cache dir]
//
// A browser cannot do this itself. GitHub's release downloads send no CORS header, and the
// firmware is not in M-VAVE's source tree: the only place it is published is inside the release's
// workbench page, as that page's own package. So this downloads the pinned release's checksum
// file and workbench, checks both, takes the package out unchanged and keeps it under build/,
// which is never committed. tools/build-site.mjs then serves it beside the page.
//
// Nothing here follows "latest". site/release.json pins one release by hash, and any difference
// from the pin stops the build, which leaves the deployed site as it was.
//
// Copyright (C) 2026 The CLATTER contributors. SPDX-License-Identifier: GPL-3.0-only

import { createHash } from 'node:crypto';
import { existsSync, mkdirSync, readFileSync, renameSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const sha = bytes => createHash('sha256').update(bytes).digest('hex');

export const defaultCache = join(root, 'build/upstream');
export const releaseFile = (open, name) => `https://github.com/${open.repository}/releases/download/${open.tag}/${name}`;

/** The JSON object assigned to FM1_PACKAGE in a workbench page. */
export function workbenchPackage(html) {
  const found = /FM1_PACKAGE\s*=\s*\{/.exec(html);
  if (!found) throw new Error('the workbench holds no firmware package');
  const start = found.index + found[0].length - 1;
  let depth = 0, quoted = false;
  for (let i = start; i < html.length; i++) {
    const c = html[i];
    if (quoted) {
      if (c === '\\') i++;
      else if (c === '"') quoted = false;
    } else if (c === '"') quoted = true;
    else if (c === '{') depth++;
    else if (c === '}' && --depth === 0) return JSON.parse(html.slice(start, i + 1));
  }
  throw new Error('the workbench\'s firmware package has no end');
}

/** The firmware inside these release files, or an error saying how they differ from the pin. */
export function verifyRelease(open, sums, workbench) {
  if (sha(sums) !== open.sumsSha256) throw new Error(`${open.sums} is not the pinned file`);
  const listed = Buffer.from(sums).toString('utf8').split('\n').some(line => line.trim().split(/\s+/).join(' ') === `${open.assetSha256} ${open.asset}`);
  if (!listed) throw new Error(`${open.sums} does not list ${open.asset}`);
  if (sha(workbench) !== open.assetSha256) throw new Error(`${open.asset} does not match M-VAVE's published SHA-256`);

  const meta = workbenchPackage(Buffer.from(workbench).toString('utf8'));
  const expected = {
    product: open.product, family: open.family, release_version: open.version, build_id: open.buildId,
    sha256: open.sha256, bytes: open.bytes, loader_sha256: open.loaderSha256,
  };
  for (const [field, value] of Object.entries(expected))
    if (meta[field] !== value) throw new Error(`the workbench's package has ${field} ${meta[field]}, not ${value}`);
  const firmware = Buffer.from(meta.data ?? '', 'base64');
  if (firmware.length !== open.bytes || sha(firmware) !== open.sha256) throw new Error('the firmware in the workbench does not match its pin');
  return firmware;
}

async function download(url, fetcher, attempts = 3) {
  for (let attempt = 1; ; attempt++) {
    try {
      const response = await fetcher(url);
      if (!response.ok) throw new Error(`${response.status}`);
      return Buffer.from(await response.arrayBuffer());
    } catch (error) {
      if (attempt >= attempts) throw new Error(`could not download ${url}: ${error.message}`);
      await new Promise(resolve => setTimeout(resolve, attempt * 1000));
    }
  }
}

/** The pinned firmware's path, downloaded and checked if the cache does not already hold it. */
export async function ensureOpenFirmware(open, { cache = defaultCache, fetcher = fetch } = {}) {
  const path = join(cache, open.filename);
  if (existsSync(path)) {
    const held = readFileSync(path);
    if (held.length === open.bytes && sha(held) === open.sha256) return path;
  }
  const sums = await download(releaseFile(open, open.sums), fetcher);
  const workbench = await download(releaseFile(open, open.asset), fetcher);
  const firmware = verifyRelease(open, sums, workbench);
  mkdirSync(cache, { recursive: true });
  writeFileSync(`${path}.part`, firmware);
  renameSync(`${path}.part`, path);
  return path;
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const { openFirmware: open } = JSON.parse(readFileSync(join(root, 'site/release.json'), 'utf8'));
  const at = process.argv.indexOf('--cache');
  try {
    const path = await ensureOpenFirmware(open, at > 0 ? { cache: process.argv[at + 1] } : {});
    console.log(`official Open ${open.version} from ${open.repository}: ${path}, SHA-256 ${open.sha256}`);
  } catch (error) {
    console.error(`fetch-open-firmware: ${error.message}`);
    process.exit(1);
  }
}
