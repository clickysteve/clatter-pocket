#!/usr/bin/env node
// Serve build/site as GitHub Pages will: under /clatter-pocket/, so that a path written from
// the site's root fails here as it would there. Anything outside that prefix is a 404.
//
//   node tools/serve-site.mjs [port] [dir]
//
// Copyright (C) 2026 The CLATTER contributors. SPDX-License-Identifier: GPL-3.0-only

import { createServer } from 'node:http';
import { createReadStream, existsSync, statSync } from 'node:fs';
import { dirname, extname, join, normalize } from 'node:path';
import { fileURLToPath } from 'node:url';

const PREFIX = '/clatter-pocket/';
const TYPES = {
  '.html': 'text/html; charset=utf-8', '.css': 'text/css', '.js': 'text/javascript', '.mjs': 'text/javascript',
  '.json': 'application/json', '.wasm': 'application/wasm', '.png': 'image/png', '.svg': 'image/svg+xml',
  '.woff2': 'font/woff2', '.txt': 'text/plain; charset=utf-8',
};

export function serve(dir, port) {
  const server = createServer((req, res) => {
    const path = decodeURIComponent(new URL(req.url, 'http://x').pathname);
    if (path === '/clatter-pocket') { res.writeHead(301, { Location: PREFIX }); res.end(); return; }
    let file = path.startsWith(PREFIX) ? join(dir, normalize(path.slice(PREFIX.length))) : null;
    if (file && !file.startsWith(dir)) file = null;
    if (file && existsSync(file) && statSync(file).isDirectory()) file = join(file, 'index.html');
    if (!file || !existsSync(file)) { res.writeHead(404); res.end('not found'); return; }
    res.writeHead(200, { 'Content-Type': TYPES[extname(file)] ?? 'application/octet-stream' });
    createReadStream(file).pipe(res);
  });
  return new Promise(resolve => server.listen(port, '127.0.0.1', () => resolve(server)));
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const port = Number(process.argv[2] ?? 8770);
  const dir = process.argv[3] ?? join(dirname(fileURLToPath(import.meta.url)), '../build/site');
  await serve(dir, port);
  console.log(`http://127.0.0.1:${port}${PREFIX}`);
}
