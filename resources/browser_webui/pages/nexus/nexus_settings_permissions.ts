// Copyright 2025 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TODO(nexus-permissions): wire to a backend permissions store. State is
// local-only this pass; toggles do not persist across reload.

import 'chrome://resources/cr_elements/cr_toggle/cr_toggle.js';

import {
  CrLitElement,
  css,
  html,
  type CSSResultGroup,
} from 'chrome://resources/lit/v3_0/lit.rollup.js';

import {getString, paneStyles} from './nexus_shared.js';
import './nexus_subpage_layout.js';

interface PermissionRow {
  desc: string;
  key: keyof PermissionsState;
  title: string;
}

interface PermissionsState {
  clipboard: boolean;
  files: boolean;
  network: boolean;
  notifications: boolean;
  screen: boolean;
}

export class NexusSettingsPermissionsElement extends CrLitElement {
  static get is() {
    return 'nexus-settings-permissions';
  }

  static override styles: CSSResultGroup = [
    paneStyles,
    css`
      :host {
        width: 100%;
      }
    `,
  ];

  private state_: PermissionsState = {
    clipboard: true,
    files: true,
    network: true,
    notifications: true,
    screen: true,
  };

  private rows_(): PermissionRow[] {
    return [
      {
        key: 'files',
        title: getString(
            'customBrowserNexusPermFilesTitle', 'Local file access'),
        desc: getString(
            'customBrowserNexusPermFilesDesc',
            'Read and write files in user-approved folders'),
      },
      {
        key: 'screen',
        title: getString(
            'customBrowserNexusPermScreenTitle', 'Screen recording'),
        desc: getString(
            'customBrowserNexusPermScreenDesc',
            'Capture screenshots for tool calls'),
      },
      {
        key: 'clipboard',
        title: getString(
            'customBrowserNexusPermClipboardTitle', 'Clipboard'),
        desc: getString(
            'customBrowserNexusPermClipboardDesc',
            'Read and write clipboard contents'),
      },
      {
        key: 'notifications',
        title: getString(
            'customBrowserNexusPermNotificationsTitle',
            'System notifications'),
        desc: getString(
            'customBrowserNexusPermNotificationsDesc',
            'Send desktop notifications when tasks complete'),
      },
      {
        key: 'network',
        title: getString(
            'customBrowserNexusPermNetworkTitle', 'Unrestricted network'),
        desc: getString(
            'customBrowserNexusPermNetworkDesc',
            'Allow agent to fetch arbitrary URLs'),
      },
    ];
  }

  private onToggleChange_(
      key: keyof PermissionsState, e: CustomEvent<boolean>) {
    this.state_ = {...this.state_, [key]: e.detail};
    this.requestUpdate();
  }

  protected override render() {
    return html`
      <nexus-subpage-layout>
        <section class="panel-shell">
          <section class="settings-page-section">
            <div class="settings-page-section-header">
              <h2 class="settings-page-section-title">${getString(
                  'customBrowserNexusPermissionsPageTitle',
                  'Permissions')}</h2>
            </div>
            <div class="section-card">
              ${this.rows_().map(r => html`
                <div class="card-row">
                  <div class="card-row-content">
                    <div class="card-row-title">${r.title}</div>
                    <div class="card-row-description">${r.desc}</div>
                  </div>
                  <cr-toggle class="card-row-action"
                      ?checked=${this.state_[r.key]}
                      @change=${(e: CustomEvent<boolean>) =>
                          this.onToggleChange_(r.key, e)}>
                  </cr-toggle>
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
    'nexus-settings-permissions': NexusSettingsPermissionsElement;
  }
}

customElements.define(
    NexusSettingsPermissionsElement.is, NexusSettingsPermissionsElement);
