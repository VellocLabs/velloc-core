// Copyright 2025 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TODO(nexus-tools): wire user-agent + cookie toggle to a backend pref store.
// State is local-only for those this pass.

import 'chrome://resources/cr_elements/cr_icon_button/cr_icon_button.js';
import 'chrome://resources/cr_elements/cr_input/cr_input.js';
import 'chrome://resources/cr_elements/cr_toggle/cr_toggle.js';
import 'chrome://resources/cr_elements/icons.html.js';

import {sendWithPromise} from 'chrome://resources/js/cr.js';
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

// Defaults mirror the browser-side GetAgentConfig() defaults. "Clear" on a
// numeric field resets to these.
const MAX_TURNS_DEFAULT = 100;
const STEP_TIMEOUT_DEFAULT_SEC = 180;

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

      /* Inline control on the trailing edge of a cr-row-style settings row
         (Chromium keyboard-shortcut / settings pattern): the label + secondary
         text fill the leading column (.card-row-content) and the cr-input sits
         compact on the right, vertically centered.

         By default cr-input reserves a hidden error row beneath the field
         (cr_input.css: #error { display: block; visibility: hidden; ... }),
         which makes the control look like a tall, detached form box rather
         than a clean settings value. These fields never raise validation
         errors, so suppress that reserved row with --cr-input-error-display,
         exactly as chrome://settings inline inputs do. */
      .row-input {
        --cr-input-error-display: none;

        flex: none;
      }

      .row-input.numeric {
        --cr-input-width: 72px;
      }

      .row-input.ua {
        --cr-input-width: 240px;

        max-width: 62%;
      }

      /* Edit / Clear suffix buttons, matching cr-shortcut-input's affordance. */
      .row-input cr-icon-button {
        --cr-icon-button-size: 28px;
        --cr-icon-button-icon-size: 16px;

        margin-inline-start: 4px;
      }
    `,
  ];

  // Agent-execution values, persisted in the unified nexus.agent_config dict
  // (getAgentConfig/setAgentConfig). maxTurns = "Max steps".
  private maxTurns_ = MAX_TURNS_DEFAULT;
  private stepTimeoutSec_ = STEP_TIMEOUT_DEFAULT_SEC;
  // Empty = use the default Chromium user agent (shown as placeholder); a
  // non-empty value is a custom override. Local-only this pass.
  private userAgent_ = '';
  private blockThirdPartyCookies_ = true;
  // Persisted (nexus.browser_tools_separate_window).
  private separateTabWindow_ = false;
  // Which fields are currently in edit mode (read-only is the default for the
  // text/number fields, matching cr-shortcut-input). Keyed by field id.
  private editingFields_ = new Set<string>();

  override firstUpdated() {
    void this.loadSeparateTabWindow_();
    void this.loadAgentConfig_();
  }

  private async loadSeparateTabWindow_() {
    try {
      const value = await sendWithPromise('getBrowserToolsSeparateWindow');
      this.separateTabWindow_ = value === true;
      this.requestUpdate();
    } catch (error) {
      console.error('Failed to read agent tab window mode:', error);
    }
  }

  private async loadAgentConfig_() {
    try {
      const config =
          await sendWithPromise('getAgentConfig') as Record<string, unknown>;
      const maxTurns = config?.['maxTurns'];
      if (typeof maxTurns === 'number' && Number.isFinite(maxTurns)) {
        this.maxTurns_ = maxTurns;
      }
      const stepTimeoutSec = config?.['stepTimeoutSec'];
      if (typeof stepTimeoutSec === 'number' &&
          Number.isFinite(stepTimeoutSec)) {
        this.stepTimeoutSec_ = stepTimeoutSec;
      }
      this.requestUpdate();
    } catch (error) {
      console.error('Failed to read agent config:', error);
    }
  }

  // --- Shared edit-mode state (mirrors cr-shortcut-input's readonly_ flow) ---

  private isEditing_(field: string): boolean {
    return this.editingFields_.has(field);
  }

  // Edit → leave read-only and focus the field for typing (mirrors
  // cr-shortcut-input's onEditClick_ → readonly_ = false; input.focus()).
  private async enterEdit_(field: string, inputId: string) {
    this.editingFields_.add(field);
    this.requestUpdate();
    await this.updateComplete;
    this.shadowRoot?.querySelector<HTMLElement>('#' + inputId)?.focus();
  }

  // Commit + return to read-only when focus leaves (cr-shortcut-input's
  // endCapture_). The value is already captured via @change / @input.
  private exitEdit_(field: string) {
    if (this.editingFields_.delete(field)) {
      this.requestUpdate();
    }
  }

  // --- Max steps (persisted) ---

  private async applyMaxTurns_(value: number) {
    const next = Number.isFinite(value)
        ? Math.min(10000, Math.max(10, Math.trunc(value)))
        : this.maxTurns_;
    this.maxTurns_ = next;
    this.requestUpdate();
    try {
      await sendWithPromise('setAgentConfig', 'maxTurns', next);
    } catch (error) {
      console.error('Failed to save max steps:', error);
    }
  }

  private onMaxTurnsChange_(e: Event) {
    void this.applyMaxTurns_(
        Number.parseInt((e.target as HTMLInputElement).value, 10));
  }

  private resetMaxTurns_() {
    this.editingFields_.delete('maxTurns');
    void this.applyMaxTurns_(MAX_TURNS_DEFAULT);
  }

  // --- Step timeout (persisted) ---

  private async applyStepTimeout_(value: number) {
    const next = Number.isFinite(value)
        ? Math.min(600, Math.max(10, Math.trunc(value)))
        : this.stepTimeoutSec_;
    this.stepTimeoutSec_ = next;
    this.requestUpdate();
    try {
      await sendWithPromise('setAgentConfig', 'stepTimeoutSec', next);
    } catch (error) {
      console.error('Failed to save step timeout:', error);
    }
  }

  private onStepTimeoutChange_(e: Event) {
    void this.applyStepTimeout_(
        Number.parseInt((e.target as HTMLInputElement).value, 10));
  }

  private resetStepTimeout_() {
    this.editingFields_.delete('stepTimeout');
    void this.applyStepTimeout_(STEP_TIMEOUT_DEFAULT_SEC);
  }

  // --- User-agent (local-only) ---

  private onUserAgentInput_(e: Event) {
    this.userAgent_ = (e.target as HTMLInputElement).value;
    this.requestUpdate();
  }

  private resetUserAgent_() {
    this.userAgent_ = '';
    this.editingFields_.delete('ua');
    this.requestUpdate();
  }

  // --- Toggles ---

  private onBlockCookiesChange_(e: CustomEvent<boolean>) {
    this.blockThirdPartyCookies_ = e.detail;
    this.requestUpdate();
  }

  private async onSeparateTabWindowChange_(e: CustomEvent<boolean>) {
    const enabled = e.detail;
    this.separateTabWindow_ = enabled;
    this.requestUpdate();
    try {
      await sendWithPromise('setBrowserToolsSeparateWindow', enabled);
    } catch (error) {
      console.error('Failed to save agent tab window mode:', error);
    }
  }

  // Edit (always) + Clear/reset (only when a value can be cleared) suffix
  // buttons — the same affordance as cr-shortcut-input.
  private renderEditClear_(config: {
    field: string,
    inputId: string,
    editAriaLabel: string,
    showClear: boolean,
    clearTitle: string,
    clearAriaLabel: string,
    onClear: () => void,
  }) {
    return html`
      <cr-icon-button slot="suffix"
          iron-icon="cr:create"
          title=${getString('customBrowserNexusEditLabel', 'Edit')}
          aria-label=${config.editAriaLabel}
          @click=${() => this.enterEdit_(config.field, config.inputId)}>
      </cr-icon-button>
      ${config.showClear ? html`
        <cr-icon-button slot="suffix"
            iron-icon="cr:clear"
            title=${config.clearTitle}
            aria-label=${config.clearAriaLabel}
            @click=${config.onClear}>
        </cr-icon-button>
      ` : ''}`;
  }

  protected override render() {
    const resetTitle =
        getString('customBrowserNexusResetDefaultLabel', 'Reset to default');
    return html`
      <nexus-subpage-layout>
        <section class="panel-shell">
          <section class="settings-page-section">
            <div class="settings-page-section-header">
              <h2 class="settings-page-section-title">${getString(
                  'customBrowserNexusToolsSectionTitle', 'Tools')}</h2>
            </div>
            <div class="section-card">
              <div class="card-row">
                <div class="card-row-content">
                  <div class="card-row-title">${getString(
                      'customBrowserNexusMaxStepsTitle', 'Max steps')}</div>
                  <div class="card-row-description">${getString(
                      'customBrowserNexusMaxStepsDesc',
                      'Maximum number of actions the agent may perform in a ' +
                          'single run.')}</div>
                </div>
                <cr-input class="row-input numeric" id="maxStepsInput"
                    type="number"
                    min="10"
                    max="10000"
                    ?readonly=${!this.isEditing_('maxTurns')}
                    .value=${String(this.maxTurns_)}
                    aria-label="Max steps"
                    @change=${this.onMaxTurnsChange_}
                    @blur=${() => this.exitEdit_('maxTurns')}>
                  ${this.renderEditClear_({
                    field: 'maxTurns',
                    inputId: 'maxStepsInput',
                    editAriaLabel: 'Edit Max steps',
                    showClear: this.maxTurns_ !== MAX_TURNS_DEFAULT,
                    clearTitle: resetTitle,
                    clearAriaLabel: 'Reset Max steps to the default',
                    onClear: () => this.resetMaxTurns_(),
                  })}
                </cr-input>
              </div>
              <div class="card-row">
                <div class="card-row-content">
                  <div class="card-row-title">${getString(
                      'customBrowserNexusStepTimeoutTitle',
                      'Step timeout (seconds)')}</div>
                  <div class="card-row-description">${getString(
                      'customBrowserNexusStepTimeoutDesc',
                      'Maximum number of seconds a single action may run ' +
                          'before it is cancelled.')}</div>
                </div>
                <cr-input class="row-input numeric" id="stepTimeoutInput"
                    type="number"
                    min="10"
                    max="600"
                    ?readonly=${!this.isEditing_('stepTimeout')}
                    .value=${String(this.stepTimeoutSec_)}
                    aria-label="Step timeout in seconds"
                    @change=${this.onStepTimeoutChange_}
                    @blur=${() => this.exitEdit_('stepTimeout')}>
                  ${this.renderEditClear_({
                    field: 'stepTimeout',
                    inputId: 'stepTimeoutInput',
                    editAriaLabel: 'Edit Step timeout',
                    showClear: this.stepTimeoutSec_ !== STEP_TIMEOUT_DEFAULT_SEC,
                    clearTitle: resetTitle,
                    clearAriaLabel: 'Reset Step timeout to the default',
                    onClear: () => this.resetStepTimeout_(),
                  })}
                </cr-input>
              </div>
            </div>
          </section>

          <section class="settings-page-section">
            <div class="settings-page-section-header">
              <h2 class="settings-page-section-title">${getString(
                  'customBrowserNexusToolsPageTitle',
                  'Tools · Browser')}</h2>
            </div>
            <div class="section-card">
              <div class="card-row">
                <div class="card-row-content">
                  <div class="card-row-title">${getString(
                      'customBrowserNexusToolsUaTitle',
                      'User-agent string')}</div>
                  <div class="card-row-description">${getString(
                      'customBrowserNexusToolsUaDesc',
                      'Custom browser identity sent with each request. Leave ' +
                          'empty to use the default Chromium user agent.')}</div>
                </div>
                <cr-input class="row-input ua" id="userAgentInput"
                    ?readonly=${!this.isEditing_('ua')}
                    .value=${this.userAgent_}
                    placeholder=${DEFAULT_USER_AGENT}
                    aria-label="User-agent string"
                    @input=${this.onUserAgentInput_}
                    @blur=${() => this.exitEdit_('ua')}>
                  ${this.renderEditClear_({
                    field: 'ua',
                    inputId: 'userAgentInput',
                    editAriaLabel: 'Edit user-agent string',
                    showClear: !!this.userAgent_,
                    clearTitle: resetTitle,
                    clearAriaLabel: 'Reset user-agent to the default',
                    onClear: () => this.resetUserAgent_(),
                  })}
                </cr-input>
              </div>
              <div class="card-row">
                <div class="card-row-content">
                  <div class="card-row-title">${getString(
                      'customBrowserNexusToolsCookiesTitle',
                      'Block third-party cookies')}</div>
                  <div class="card-row-description">${getString(
                      'customBrowserNexusToolsCookiesDesc',
                      'Helps reduce cross-site tracking during agent sessions.')}</div>
                </div>
                <cr-toggle class="card-row-action"
                    ?checked=${this.blockThirdPartyCookies_}
                    @change=${this.onBlockCookiesChange_}>
                </cr-toggle>
              </div>
              <div class="card-row">
                <div class="card-row-content">
                  <div class="card-row-title">${getString(
                      'customBrowserNexusToolsSeparateWindowTitle',
                      'Open agent tabs in a separate window')}</div>
                  <div class="card-row-description">${getString(
                      'customBrowserNexusToolsSeparateWindowDesc',
                      'When enabled, the agent uses a dedicated browser ' +
                          'window to keep automated tabs separate from your ' +
                          'current workspace.')}</div>
                </div>
                <cr-toggle class="card-row-action"
                    ?checked=${this.separateTabWindow_}
                    @change=${this.onSeparateTabWindowChange_}>
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
