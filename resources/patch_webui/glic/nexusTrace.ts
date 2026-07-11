// Copyright 2024 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {loadTimeData} from 'chrome://resources/js/load_time_data.js';

/**
 * Build-time debug flag. Set to `false` for release builds.
 * When false, all nexusTrace/nexusTraceLine calls become dead code
 * and can be eliminated by minifiers (Terser, Rollup, etc.).
 */
export const NEXUS_DEBUG = true;

export const isNexusMode = loadTimeData.getBoolean('isNexusMode');

export function nexusTrace(message: string, details?: unknown): void {
  if (!NEXUS_DEBUG || !isNexusMode) return;
  if (details === undefined) {
    console.info(`[NexusTrace][Host] ${message}`);
    return;
  }
  console.info(`[NexusTrace][Host] ${message}`, details);
}

export function nexusTraceLine(message: string): void {
  if (!NEXUS_DEBUG || !isNexusMode) return;
  console.info(`[NexusTrace][Host] ${message}`);
}
