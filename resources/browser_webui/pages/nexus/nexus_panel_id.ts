// Copyright 2025 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

/**
 * This panel's id. One window is one panel; the guest and this page meet on
 * `shell/<panelId>`, so one window's layout frames cannot move another's native
 * views. Minted per page load and sent in the `nexus-init` payload.
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
