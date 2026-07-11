import 'chrome://resources/cr_elements/cr_button/cr_button.js';
import 'chrome://resources/cr_elements/cr_input/cr_input.js';

import {
  CrLitElement,
  css,
  html,
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

export class NexusSettingsWebappElement extends CrLitElement {
  static get is() {
    return 'nexus-settings-webapp';
  }

  static override styles: CSSResultGroup = [
    paneStyles,
    css`
      :host {
        width: 100%;
      }

      .loading-text {
        color: var(--cr-secondary-text-color, var(--text-secondary));
        line-height: 20px;
        margin: 0;
      }

      .nexus-input {
        --cr-input-width: 100%;

        display: block;
        width: 100%;
      }

      .nexus-input::part(row-container) {
        justify-content: normal;
      }
    `,
  ];

  private loading_ = true;
  private validationMessage_ = '';
  private webappUrl_ = '';

  override firstUpdated() {
    void this.loadState_();
  }

  private async loadState_() {
    this.loading_ = true;
    this.requestUpdate();

    this.webappUrl_ = await this.getPrefString_(WEBAPP_URL_PREF, '');
    this.validationMessage_ = getWebappUrlValidationMessage(this.webappUrl_);
    this.loading_ = false;
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

  private async setPref_(prefName: string, value: unknown) {
    if (!chrome.settingsPrivate) {
      return;
    }

    try {
      await chrome.settingsPrivate.setPref(prefName, value);
    } catch (error) {
      console.error(`Failed to set pref ${prefName}:`, error);
    }
  }

  private onInput_(event: Event) {
    const input = event.currentTarget as CrInputLike;
    this.webappUrl_ = input.value || '';
    this.validationMessage_ = getWebappUrlValidationMessage(this.webappUrl_);
    this.requestUpdate();
  }

  private async onChange_(event: Event) {
    const input = event.currentTarget as CrInputLike;
    const url = (input.value || '').trim();

    this.webappUrl_ = url;
    this.validationMessage_ = getWebappUrlValidationMessage(url);
    if (!isValidWebappUrl(url)) {
      this.requestUpdate();
      return;
    }

    await this.setPref_(WEBAPP_URL_PREF, url);
    this.requestUpdate();
  }

  private async onReset_() {
    this.webappUrl_ = '';
    this.validationMessage_ = '';
    await this.setPref_(WEBAPP_URL_PREF, '');
    this.requestUpdate();
  }

  protected override render() {
    const sectionTitle = getString(
        'customBrowserNexusWebappSectionTitle', 'OpenClaw web app');
    const sectionDescription = getString(
        'customBrowserNexusWebappSectionDescription',
        'Override the OpenClaw web app URL that Nexus opens. Leave this ' +
            'blank to keep the built-in web app.');
    const defaultBehaviorTitle = getString(
        'customBrowserNexusWebappDefaultTitle', 'Use the built-in web app');
    const defaultBehaviorDescription = getString(
        'customBrowserNexusWebappDefaultDescription',
        'Clear the custom URL and use the default OpenClaw web app that ' +
            'ships with Nexus.');

    if (this.loading_) {
      return html`
        <nexus-subpage-layout>
          <section class="panel-shell">
            <div class="panel-card">
              <p class="loading-text">
                ${getString('customBrowserLoadingLabel', 'Loading…')}
              </p>
            </div>
          </section>
        </nexus-subpage-layout>
      `;
    }

    return html`
      <nexus-subpage-layout>
        <section class="panel-shell">
          <section class="settings-page-section">
            <div class="settings-page-section-header">
              <h2 class="settings-page-section-title">${sectionTitle}</h2>
            </div>
            <div class="section-card">
              <div class="card-row block">
                <div class="field-label with-description" id="webappUrlLabel">
                  ${getString(
                      'customBrowserNexusWebappUrlLabel',
                      'Custom OpenClaw web app URL')}
                </div>
                <p class="card-row-description field-description">
                  ${sectionDescription}
                </p>
                <cr-input id="webappUrlInput"
                    class="nexus-input"
                    aria-labelledby="webappUrlLabel"
                    type="url"
                    .placeholder=${getString(
                        'customBrowserNexusWebappUrlPlaceholder',
                        'https://your-openclaw-app.example.com')}
                    .value=${this.webappUrl_}
                    .errorMessage=${this.validationMessage_}
                    .invalid=${!!this.validationMessage_}
                    @input=${this.onInput_}
                    @change=${this.onChange_}>
                </cr-input>
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
                    @click=${this.onReset_}>
                  ${getString(
                      'customBrowserNexusWebappUrlReset',
                      'Reset to default')}
                </cr-button>
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
    'nexus-settings-webapp': NexusSettingsWebappElement;
  }
}

customElements.define(
    NexusSettingsWebappElement.is, NexusSettingsWebappElement);
