import 'chrome://resources/cr_elements/cr_button/cr_button.js';

import {
  CrLitElement,
  css,
  html,
  type CSSResultGroup,
} from 'chrome://resources/lit/v3_0/lit.rollup.js';

type NexusTemplate = ReturnType<typeof html>;

import {getString, paneStyles} from './nexus_shared.js';
import './nexus_subpage_layout.js';

export class NexusWarningElement extends CrLitElement {
  static get is() {
    return 'nexus-warning';
  }

  static override styles: CSSResultGroup = [
    paneStyles,
    css`
      :host {
        background-color: var(--cr-page-background-color, var(--app-background, #f8f9fa));
      }

      .panel-shell {
        padding-top: 24px;
      }

      .warning-list {
        color: var(--cr-primary-text-color, var(--text-primary));
        line-height: 20px;
        margin: 16px 0 0 20px;
        padding: 0;
      }

      .warning-list li + li {
        margin-top: 10px;
      }

      .acknowledge-row {
        align-items: flex-start;
        display: flex;
        gap: 12px;
        margin-top: 24px;
      }

      .acknowledge-checkbox {
        accent-color: var(--google-blue-600);
        block-size: 18px;
        inline-size: 18px;
        margin: 2px 0 0;
      }

      .acknowledge-label {
        color: var(--cr-primary-text-color, var(--text-primary));
        line-height: 20px;
      }

      .actions {
        display: flex;
        justify-content: flex-end;
        margin-top: 24px;
      }
    `,
  ];

  private acknowledged_ = false;

  private onAcknowledgeChange_(event: Event) {
    this.acknowledged_ = (event.target as HTMLInputElement).checked;
    this.requestUpdate();
  }

  private onContinueClick_() {
    if (!this.acknowledged_) {
      return;
    }

    this.dispatchEvent(new CustomEvent(
        'nexus-warning-accepted', {bubbles: true, composed: true}));
  }

  private renderFormattedText_(text: string) {
    if (!text) return html``;
    
    const result: NexusTemplate[] = [];
    const lines = text.split('\n');
    let currentParagraph: string[] = [];
    let currentList: string[] = [];

    const flushParagraph = () => {
      if (currentParagraph.length > 0) {
        result.push(html`
          <p class="panel-copy">
            ${currentParagraph.map((line, i) => html`${line}${i < currentParagraph.length - 1 ? html`<br>` : ''}`)}
          </p>
        `);
        currentParagraph = [];
      }
    };

    const flushList = () => {
      if (currentList.length > 0) {
        result.push(html`
          <ul class="warning-list">
            ${currentList.map(item => html`<li>${item}</li>`)}
          </ul>
        `);
        currentList = [];
      }
    };

    for (const line of lines) {
      const trimmed = line.trim();
      if (!trimmed) {
        flushParagraph();
        flushList();
        continue;
      }

      if (trimmed.startsWith('- ') || trimmed.startsWith('* ')) {
        flushParagraph();
        currentList.push(trimmed.substring(2).trim());
      } else {
        flushList();
        currentParagraph.push(trimmed);
      }
    }
    flushParagraph();
    flushList();

    return result;
  }

  protected override render() {
    return html`
      <nexus-subpage-layout>
      <section class="panel-shell">
        <div class="panel-card">
          <p class="panel-description">
            ${getString(
                'customBrowserNexusWarningIntro',
                'Please read before enabling Nexus.')}
          </p>
          ${this.renderFormattedText_(getString(
              'customBrowserNexusWarningText',
              'Nexus is an experimental feature that provides advanced autonomous browser automation capabilities.\n\nBy enabling this feature, you understand and agree to the following:\n- You are solely responsible for actions performed by Nexus on your behalf\n- Some websites may restrict or block automated access\n- Your session data may be used to improve feature accuracy\n- This tool should not be used on shared or public devices without explicit consent\n- Sensitive operations should be monitored manually\n\nPlease ensure you are running this in a controlled environment.'))}
          <label class="acknowledge-row">
            <input class="acknowledge-checkbox" type="checkbox"
                @change=${this.onAcknowledgeChange_}>
            <span class="acknowledge-label">
              ${getString('customBrowserNexusWarningAcknowledgment')}
            </span>
          </label>
          <div class="actions">
            <cr-button class="action-button"
                ?disabled=${!this.acknowledged_}
                @click=${this.onContinueClick_}>
              ${getString('customBrowserNexusWarningContinue', 'Continue')}
            </cr-button>
          </div>
        </div>
      </section>
      </nexus-subpage-layout>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'nexus-warning': NexusWarningElement;
  }
}

customElements.define(NexusWarningElement.is, NexusWarningElement);
