// Copyright 2025 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// About page. Two parts:
//   1. Version metadata (Nexus / Chromium / License). Read-only.
//   2. Web App URL section — porting the persisted nexus.webapp_url save
//      flow (250 ms debounce, blur flush, disconnect flush, restart-tip
//      banner) verbatim from the deleted nexus_settings_webapp.ts. Logic
//      and behavior preserved; only its location moved (per design).

import 'chrome://resources/cr_elements/cr_button/cr_button.js';
import 'chrome://resources/cr_elements/cr_input/cr_input.js';

import {loadTimeData} from 'chrome://resources/js/load_time_data.js';
import {
  CrLitElement,
  css,
  html,
  nothing,
  type CSSResultGroup,
} from 'chrome://resources/lit/v3_0/lit.rollup.js';

import {
  getString,
  getWebappUrlValidationMessage,
  isValidWebappUrl,
  paneStyles,
  WEBAPP_URL_PREF,
} from './nexus_shared.js';
import './nexus_subpage_layout.js';

type CrInputLike = HTMLElement & {
  errorMessage: string;
  invalid: boolean;
  label: string;
  placeholder: string|null;
  type: string;
  value: string;
};

const SAVE_DEBOUNCE_MS = 250;
const NEXUS_VERSION_FALLBACK = '0.4.2';
const CHROMIUM_VERSION_FALLBACK = '128.0.6613.119';

export class NexusSettingsAboutElement extends CrLitElement {
  static get is() {
    return 'nexus-settings-about';
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

      .nexus-input::part(row-container) {
        justify-content: normal;
      }

      .restart-tip {
        background: var(--google-yellow-50, #fef7e0);
        border-radius: 8px;
        color: var(--google-yellow-900, #ea8600);
        font-size: 0.85rem;
        line-height: 20px;
        margin: 12px 0 0;
        padding: 10px 12px;
      }

      @media (prefers-color-scheme: dark) {
        .restart-tip {
          background: rgba(253, 214, 99, 0.16);
          color: var(--google-yellow-300, #fdd663);
        }
      }
    `,
  ];

  // --- Web App URL state (ported from nexus_settings_webapp.ts) ---

  private webappLoading_ = true;
  private validationMessage_ = '';
  private webappUrl_ = '';
  private savedUrl_ = '';
  private loadedUrl_ = '';
  private saveTimer_ = 0;

  override firstUpdated() {
    void this.loadWebappState_();
  }

  override disconnectedCallback() {
    super.disconnectedCallback();
    if (this.saveTimer_) {
      clearTimeout(this.saveTimer_);
      this.saveTimer_ = 0;
      void this.commitSave_(this.webappUrl_);
    }
  }

  // --- Pref I/O (ported verbatim) -------------------------------------

  private async loadWebappState_() {
    this.webappLoading_ = true;
    this.requestUpdate();

    const value = await this.getPrefString_(WEBAPP_URL_PREF, '');
    this.webappUrl_ = value;
    this.savedUrl_ = value;
    this.loadedUrl_ = value;
    this.validationMessage_ = getWebappUrlValidationMessage(value);
    this.webappLoading_ = false;
    this.requestUpdate();
  }

  private async getPrefString_(prefName: string, fallback: string):
      Promise<string> {
    if (!chrome.settingsPrivate) {
      return fallback;
    }
    try {
      const pref = await chrome.settingsPrivate.getPref(prefName);
      return typeof pref.value === 'string' ? pref.value : fallback;
    } catch (error) {
      console.error(`Failed to read pref ${prefName}:`, error);
      return fallback;
    }
  }

  private async setPref_(prefName: string, value: unknown): Promise<boolean> {
    if (!chrome.settingsPrivate) {
      return false;
    }
    try {
      const ok = await chrome.settingsPrivate.setPref(prefName, value);
      return ok !== false;
    } catch (error) {
      console.error(`Failed to set pref ${prefName}:`, error);
      return false;
    }
  }

  private scheduleSave_() {
    if (this.saveTimer_) {
      clearTimeout(this.saveTimer_);
    }
    const pending = this.webappUrl_.trim();
    this.saveTimer_ = window.setTimeout(() => {
      this.saveTimer_ = 0;
      void this.commitSave_(pending);
    }, SAVE_DEBOUNCE_MS);
  }

  private async commitSave_(value: string): Promise<boolean> {
    const trimmed = value.trim();
    if (!isValidWebappUrl(trimmed)) {
      return false;
    }
    if (trimmed === this.savedUrl_) {
      return true;
    }
    const ok = await this.setPref_(WEBAPP_URL_PREF, trimmed);
    if (ok) {
      this.savedUrl_ = trimmed;
      this.requestUpdate();
    }
    return ok;
  }

  private onUrlInput_(event: Event) {
    const input = event.currentTarget as CrInputLike;
    this.webappUrl_ = input.value || '';
    this.validationMessage_ = getWebappUrlValidationMessage(this.webappUrl_);
    this.requestUpdate();
    this.scheduleSave_();
  }

  private async onUrlChange_(event: Event) {
    const input = event.currentTarget as CrInputLike;
    const url = (input.value || '').trim();

    if (this.saveTimer_) {
      clearTimeout(this.saveTimer_);
      this.saveTimer_ = 0;
    }

    this.webappUrl_ = url;
    this.validationMessage_ = getWebappUrlValidationMessage(url);
    this.requestUpdate();

    if (!isValidWebappUrl(url)) {
      return;
    }

    await this.commitSave_(url);
  }

  private async onResetUrl_() {
    if (this.saveTimer_) {
      clearTimeout(this.saveTimer_);
      this.saveTimer_ = 0;
    }
    this.webappUrl_ = '';
    this.validationMessage_ = '';
    const ok = await this.setPref_(WEBAPP_URL_PREF, '');
    if (ok) {
      this.savedUrl_ = '';
    }
    this.requestUpdate();
  }

  // --- Version metadata helpers --------------------------------------

  private getNexusVersion_(): string {
    if (loadTimeData.valueExists('appVersion')) {
      const v = loadTimeData.getString('appVersion');
      if (v) {
        return v;
      }
    }
    return NEXUS_VERSION_FALLBACK;
  }

  private getChromiumVersion_(): string {
    if (loadTimeData.valueExists('chromeVersion')) {
      const v = loadTimeData.getString('chromeVersion');
      if (v) {
        return v;
      }
    }
    return CHROMIUM_VERSION_FALLBACK;
  }

  // --- Render --------------------------------------------------------

  protected override render() {
    const sectionTitle = getString(
        'customBrowserNexusWebappSectionTitle', 'Nexus web app');
    const sectionDescription = getString(
        'customBrowserNexusWebappSectionDescription',
        'Override the Nexus web app URL that Nexus opens. Leave this ' +
            'blank to keep the built-in web app.');
    const defaultBehaviorTitle = getString(
        'customBrowserNexusWebappDefaultTitle', 'Use the built-in web app');
    const defaultBehaviorDescription = getString(
        'customBrowserNexusWebappDefaultDescription',
        'Clear the custom URL and use the default Nexus web app that ' +
            'ships with Nexus.');

    return html`
      <nexus-subpage-layout>
        <section class="panel-shell">

          <!-- About metadata -->
          <section class="settings-page-section">
            <div class="settings-page-section-header">
              <h2 class="settings-page-section-title">${getString(
                  'customBrowserNexusAboutPageTitle',
                  `About ${getString('customBrowserProductName', 'Nexus')}`)}</h2>
            </div>
            <div class="section-card">
              <div class="card-row">
                <div class="card-row-content">
                  <div class="card-row-title">${
                      getString('customBrowserProductName', 'Nexus')}</div>
                  <div class="card-row-description">
                    Version ${this.getNexusVersion_()} · Internal channel
                  </div>
                </div>
                <cr-button class="card-row-action">
                  Check for updates
                </cr-button>
              </div>
              <div class="card-row">
                <div class="card-row-content">
                  <div class="card-row-title">Chromium</div>
                  <div class="card-row-description">
                    ${this.getChromiumVersion_()}
                  </div>
                </div>
              </div>
              <div class="card-row">
                <div class="card-row-content">
                  <div class="card-row-title">License</div>
                  <div class="card-row-description">
                    Proprietary · &copy; ${
                        new Date().getFullYear()} Nexus Labs
                  </div>
                </div>
                <cr-button class="card-row-action">
                  View licenses
                </cr-button>
              </div>
            </div>
          </section>

          <!-- Web App URL (ported from nexus_settings_webapp.ts) -->
          <section class="settings-page-section">
            <div class="settings-page-section-header">
              <h2 class="settings-page-section-title">${sectionTitle}</h2>
            </div>
            <div class="section-card">
              ${this.webappLoading_ ? html`
                <div class="card-row block">
                  <p class="card-row-description">${getString(
                      'customBrowserLoadingLabel', 'Loading…')}</p>
                </div>
              ` : html`
                <div class="card-row block">
                  <div class="field-label with-description"
                      id="webappUrlLabel">
                    ${getString(
                        'customBrowserNexusWebappUrlLabel',
                        'Custom Nexus web app URL')}
                  </div>
                  <p class="card-row-description field-description">
                    ${sectionDescription}
                  </p>
                  <cr-input id="webappUrlInput"
                      class="nexus-input"
                      aria-labelledby="webappUrlLabel"
                      type="url"
                      .placeholder=${getString(
                          'customBrowserNexusWebappDefaultUrl',
                          getString(
                              'customBrowserNexusWebappUrlPlaceholder',
                              'https://your-openclaw-app.example.com'))}
                      .value=${this.webappUrl_}
                      .errorMessage=${this.validationMessage_}
                      .invalid=${!!this.validationMessage_}
                      @input=${this.onUrlInput_}
                      @change=${this.onUrlChange_}>
                  </cr-input>
                  ${this.webappUrl_.trim() !== this.loadedUrl_.trim() &&
                      !this.validationMessage_ ? html`
                    <p class="restart-tip" role="status">
                      ${getString(
                          'customBrowserNexusWebappRestartTip',
                          'Restart the browser to apply the new web app URL.')}
                    </p>
                  ` : nothing}
                </div>

                <div class="card-row">
                  <div class="card-row-content">
                    <div class="card-row-title">${defaultBehaviorTitle}</div>
                    <div class="card-row-description">
                      ${defaultBehaviorDescription}
                    </div>
                  </div>
                  <cr-button class="card-row-action"
                      ?disabled=${!this.webappUrl_.trim()}
                      @click=${this.onResetUrl_}>
                    ${getString(
                        'customBrowserNexusWebappUrlReset',
                        'Reset to default')}
                  </cr-button>
                </div>
              `}
            </div>
          </section>

        </section>
      </nexus-subpage-layout>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'nexus-settings-about': NexusSettingsAboutElement;
  }
}

customElements.define(
    NexusSettingsAboutElement.is, NexusSettingsAboutElement);
