// Copyright 2025 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TODO(nexus-tools): wire user-agent + cookie toggle to a backend pref store.
// State is local-only this pass.

import 'chrome://resources/cr_elements/cr_input/cr_input.js';
import 'chrome://resources/cr_elements/cr_toggle/cr_toggle.js';

import {
  CrLitElement,
  css,
  html,
  type CSSResultGroup,
} from 'chrome://resources/lit/v3_0/lit.rollup.js';

import {getString, paneStyles} from './nexus_shared.js';
import './nexus_subpage_layout.js';

const DEFAULT_USER_AGENT =
    'Mozilla/5.0 (Macintosh; Intel) Nexus/0.4.2 Chromium/128';

export class NexusSettingsToolsElement extends CrLitElement {
  static get is() {
    return 'nexus-settings-tools';
  }

  static override styles: CSSResultGroup = [
    paneStyles,
    css`
      :host {
        width: 100%;
      }

      .nexus-input {
        --cr-input-width: 100%;

        display: block;
        width: 100%;
      }
    `,
  ];

  private userAgent_ = DEFAULT_USER_AGENT;
  private blockThirdPartyCookies_ = true;

  private onUserAgentInput_(e: Event) {
    this.userAgent_ = (e.target as HTMLInputElement).value;
    this.requestUpdate();
  }

  private onBlockCookiesChange_(e: CustomEvent<boolean>) {
    this.blockThirdPartyCookies_ = e.detail;
    this.requestUpdate();
  }

  protected override render() {
    return html`
      <nexus-subpage-layout>
        <section class="panel-shell">
          <section class="settings-page-section">
            <div class="settings-page-section-header">
              <h2 class="settings-page-section-title">${getString(
                  'customBrowserNexusToolsPageTitle',
                  'Tools · Browser')}</h2>
            </div>
            <div class="section-card">
              <div class="card-row block">
                <div class="field-label with-description">
                  ${getString(
                      'customBrowserNexusToolsUaTitle',
                      'User-agent string')}
                </div>
                <p class="card-row-description field-description">
                  ${getString(
                      'customBrowserNexusToolsUaDesc',
                      'Sent with every navigation. Leave blank to use Chromium default.')}
                </p>
                <cr-input class="nexus-input"
                    .value=${this.userAgent_}
                    placeholder="Mozilla/5.0 ..."
                    @input=${this.onUserAgentInput_}>
                </cr-input>
              </div>
              <div class="card-row">
                <div class="card-row-content">
                  <div class="card-row-title">${getString(
                      'customBrowserNexusToolsCookiesTitle',
                      'Block third-party cookies')}</div>
                  <div class="card-row-description">${getString(
                      'customBrowserNexusToolsCookiesDesc',
                      'Recommended for agent runs to reduce cross-site tracking.')}</div>
                </div>
                <cr-toggle class="card-row-action"
                    ?checked=${this.blockThirdPartyCookies_}
                    @change=${this.onBlockCookiesChange_}>
                </cr-toggle>
              </div>
            </div>
          </section>
        </section>
      </nexus-subpage-layout>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'nexus-settings-tools': NexusSettingsToolsElement;
  }
}

customElements.define(
    NexusSettingsToolsElement.is, NexusSettingsToolsElement);
