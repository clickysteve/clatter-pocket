// The demo page: fit the emulator's frame to what the emulator lays out.
// The frame is same-origin, so its height can be read. Without this script it keeps the
// height in styles.css, and the emulator scrolls inside it.
// Copyright (C) 2026 The CLATTER contributors. SPDX-License-Identifier: GPL-3.0-only

const frame = document.getElementById('emu');

function fit() {
  const doc = frame.contentDocument;
  if (!doc?.body) return;
  const border = frame.offsetHeight - frame.clientHeight;
  frame.style.height = `${doc.documentElement.scrollHeight + border}px`;
}

frame.addEventListener('load', () => {
  try {
    new ResizeObserver(fit).observe(frame.contentDocument.body);
    fit();
  } catch { /* the frame's own scrolling still works */ }
});
