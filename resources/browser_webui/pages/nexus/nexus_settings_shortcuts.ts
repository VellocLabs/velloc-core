// Copyright 2025 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TODO(nexus-shortcuts): wire to a real shortcut registry. This pass is
// display-only — the chips show the current bindings but cannot be rebound.

import {
  CrLitElement,
  css,
  html,
  type CSSResultGroup,
} from 'chrome://resources/lit/v3_0/lit.rollup.js';

import {getString, paneStyles} from './nexus_shared.js';
import './nexus_subpage_layout.js';

interface Shortcut {
  desc: string;
  keys: string[];
}

const SHORTCUTS: Shortcut[] = [
  {keys: ['⌘', 'K'], desc: 'New thread'},
  {keys: ['⌘', '/'], desc: 'Toggle threads sidebar'},
  {keys: ['⌘', 'B'], desc: 'Toggle browser view'},
  {keys: ['⌘', ','], desc: 'Open settings'},
  {keys: ['⌘', '⇧', 'M'], desc: 'Switch to browser mode'},
];

export class NexusSettingsShortcutsElement extends CrLitElement {
  static get is() {
    return 'nexus-settings-shortcuts';
  }

  static override styles: CSSResultGroup = [
    paneStyles,
    css`
      :host {
        width: 100%;
      }

      .kbd-row {
        display: flex;
        gap: 4px;
      }

      .kbd {
        background: var(
            --md-background-color,
            var(--cr-card-background-color, var(--app-surface)));
        border: 1px solid var(--cr-separator-color);
        border-bottom-width: 2px;
        border-radius: 4px;
        color: var(--cr-secondary-text-color, var(--text-secondary));
        font-family: ui-monospace, SFMono-Regular, "SF Mono", Menlo,
                     Consolas, monospace;
        font-size: 11px;
        min-width: 18px;
        padding: 2px 7px;
        text-align: center;
      }
    `,
  ];

  protected override render() {
    return html`
      <nexus-subpage-layout>
        <section class="panel-shell">
          <section class="settings-page-section">
            <div class="settings-page-section-header">
              <h2 class="settings-page-section-title">${getString(
                  'customBrowserNexusShortcutsPageTitle',
                  'Keyboard Shortcuts')}</h2>
            </div>
            <div class="section-card">
              ${SHORTCUTS.map(s => html`
                <div class="card-row">
                  <div class="card-row-content">
                    <div class="card-row-title">${s.desc}</div>
                  </div>
                  <div class="card-row-action kbd-row">
                    ${s.keys.map(k => html`<kbd class="kbd">${k}</kbd>`)}
                  </div>
                </div>
              `)}
            </div>
          </section>
        </section>
      </nexus-subpage-layout>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'nexus-settings-shortcuts': NexusSettingsShortcutsElement;
  }
}

customElements.define(
    NexusSettingsShortcutsElement.is, NexusSettingsShortcutsElement);
