// Copyright 2025 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Inline SVG icons for the Nexus shell (header + settings). Each icon is a
// Lit `html` template using `currentColor` so it tints from the surrounding
// button color. Sourced from the Nexus design handoff (icons.jsx).

import {html} from 'chrome://resources/lit/v3_0/lit.rollup.js';

// --- Header icons ----------------------------------------------------

export const plusIcon = html`
  <svg class="icon" viewBox="0 0 24 24" fill="none" stroke="currentColor"
      stroke-width="2" stroke-linecap="round">
    <path d="M12 5v14M5 12h14"></path>
  </svg>`;

export const globeIcon = html`
  <svg class="icon" viewBox="0 0 24 24" fill="none" stroke="currentColor"
      stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">
    <circle cx="12" cy="12" r="9"></circle>
    <path d="M3 12h18M12 3a14 14 0 0 1 0 18M12 3a14 14 0 0 0 0 18"></path>
  </svg>`;

export const globeOffIcon = html`
  <svg class="icon" viewBox="0 0 24 24" fill="none" stroke="currentColor"
      stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">
    <circle cx="12" cy="12" r="9"></circle>
    <path d="M3 12h18M12 3a14 14 0 0 1 0 18M12 3a14 14 0 0 0 0 18"></path>
    <path d="M4 4l16 16" stroke-width="2"></path>
  </svg>`;

export const shieldIcon = html`
  <svg class="icon" viewBox="0 0 24 24" fill="none" stroke="currentColor"
      stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">
    <path d="M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z"></path>
  </svg>`;

export const browserModeIcon = html`
  <svg class="icon" viewBox="0 0 24 24" fill="none" stroke="currentColor"
      stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">
    <rect x="3" y="4" width="18" height="16" rx="2"></rect>
    <path d="M3 9h18"></path>
    <circle cx="6.5" cy="6.5" r="0.5" fill="currentColor"></circle>
    <circle cx="9" cy="6.5" r="0.5" fill="currentColor"></circle>
    <circle cx="11.5" cy="6.5" r="0.5" fill="currentColor"></circle>
  </svg>`;

// --- Settings rail icons ---------------------------------------------

export const cubeIcon = html`
  <svg class="icon" viewBox="0 0 24 24" fill="none" stroke="currentColor"
      stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">
    <path d="M21 16V8l-9-5-9 5v8l9 5 9-5z"></path>
    <path d="M3.3 7.3L12 12l8.7-4.7M12 22V12"></path>
  </svg>`;

export const cpuIcon = html`
  <svg class="icon" viewBox="0 0 24 24" fill="none" stroke="currentColor"
      stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">
    <rect x="6" y="6" width="12" height="12" rx="2"></rect>
    <rect x="9" y="9" width="6" height="6"></rect>
    <path d="M9 2v2M15 2v2M9 20v2M15 20v2M2 9h2M2 15h2M20 9h2M20 15h2"></path>
  </svg>`;

export const keyboardIcon = html`
  <svg class="icon" viewBox="0 0 24 24" fill="none" stroke="currentColor"
      stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">
    <rect x="2" y="6" width="20" height="12" rx="2"></rect>
    <path d="M6 10h.01M10 10h.01M14 10h.01M18 10h.01M6 14h12"></path>
  </svg>`;

export const infoIcon = html`
  <svg class="icon" viewBox="0 0 24 24" fill="none" stroke="currentColor"
      stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">
    <circle cx="12" cy="12" r="9"></circle>
    <path d="M12 11v5M12 8v.01"></path>
  </svg>`;

export const wrenchIcon = html`
  <svg class="icon" viewBox="0 0 24 24" fill="none" stroke="currentColor"
      stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">
    <path d="M14.7 6.3a1 1 0 0 0 0 1.4l1.6 1.6a1 1 0 0 0 1.4 0l3.77-3.77a6 6 0 0 1-7.94 7.94l-6.91 6.91a2.12 2.12 0 0 1-3-3l6.91-6.91a6 6 0 0 1 7.94-7.94l-3.76 3.76z"></path>
  </svg>`;

// --- Misc small icons used inside settings panes ---------------------

export const checkIcon = html`
  <svg class="icon-sm" viewBox="0 0 24 24" fill="none" stroke="currentColor"
      stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
    <path d="M5 12l5 5L20 7"></path>
  </svg>`;

export const lockIcon = html`
  <svg class="icon-sm" viewBox="0 0 24 24" fill="none" stroke="currentColor"
      stroke-width="2" stroke-linecap="round">
    <rect x="4" y="11" width="16" height="10" rx="2"></rect>
    <path d="M8 11V7a4 4 0 0 1 8 0v4"></path>
  </svg>`;

export const errorIcon = html`
  <svg class="icon-sm" viewBox="0 0 24 24" fill="none" stroke="currentColor"
      stroke-width="2" stroke-linecap="round">
    <path d="M12 8v5M12 16v.01"></path>
    <circle cx="12" cy="12" r="9"></circle>
  </svg>`;

export const disconnectedIcon = html`
  <svg class="icon-sm" viewBox="0 0 24 24" fill="none" stroke="currentColor"
      stroke-width="2" stroke-linecap="round">
    <circle cx="12" cy="12" r="3.5"></circle>
  </svg>`;
