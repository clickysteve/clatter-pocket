// M-VAVE's update code in Node: sdk/WebUpdate.js and sdk/I18n.js, unchanged, as a browser loads
// them. For checking a package with the same code that will install it.
// Copyright (C) 2026 The CLATTER contributors. SPDX-License-Identifier: GPL-3.0-only

import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { runInThisContext } from 'node:vm';

const sdk = join(dirname(fileURLToPath(import.meta.url)), '../third_party/fm1_open/sdk');

export function loadFM1Web() {
  if (!globalThis.FM1Web) {
    runInThisContext(readFileSync(join(sdk, 'I18n.js'), 'utf8'));
    runInThisContext(readFileSync(join(sdk, 'WebUpdate.js'), 'utf8'));
  }
  return globalThis.FM1Web;
}

/** The description FM1Web.prepare takes, for a package held in memory. */
export const metaFor = (firmware, version, bytes) => ({
  schema: 1, product: firmware.product, profile: 'dx7', family: firmware.family, release_version: version,
  build_id: firmware.buildId, sha256: firmware.sha256, bytes: bytes.length, loader_sha256: firmware.loaderSha256,
  data: Buffer.from(bytes).toString('base64'),
});
