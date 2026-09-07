// Copyright 2025 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * This panel's id.
 *
 * One browser window is one panel: the guest it hosts and this page meet on
 * `shell/<panelId>` (the runtime's topic for window chrome — layout, boot
 * progress, build info, the reveal). Two windows share one SharedWorker, so
 * without the id a second window's layout frames would move the first
 * window's native views.
 *
 * Minted per page load and handed to the guest in the `nexus-init` payload,
 * which is also this page's identity on its own runtime connection. It does
 * not outlive the page: a reload is a new panel as far as the runtime is
 * concerned, and the topic's snapshot brings the fresh panel up to date.
 */

let cached: string|undefined;

export function panelId(): string {
  if (cached === undefined) {
    const random = globalThis.crypto?.randomUUID?.() ??
        `${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 10)}`;
    cached = `panel-${random}`;
  }
  return cached;
}
