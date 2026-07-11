import 'chrome://resources/cr_elements/cr_icon/cr_icon.js';
import 'chrome://resources/cr_elements/cr_ripple/cr_ripple.js';
import 'chrome://resources/cr_elements/icons.html.js';

import {
  CrLitElement,
  css,
  html,
  type CSSResultGroup,
} from 'chrome://resources/lit/v3_0/lit.rollup.js';

import {
  getString,
  type NexusSettingsSection,
} from './nexus_shared.js';
import './nexus_settings_models.js';
import './nexus_settings_webapp.js';

type SettingsNavItem = {
  icon: string;
  id: NexusSettingsSection;
  title: string;
};

export class NexusSettingsElement extends CrLitElement {
  static get is() {
    return 'nexus-settings';
  }

  static override properties = {
    selectedSection: {type: String},
  };

  static override styles: CSSResultGroup = css`
    :host {
      --settings-menu-width: 266px;
      --nexus-centered-card-max-width:
          var(--cr-centered-card-max-width, 680px);
      --nexus-centered-card-width-percentage:
          var(--cr-centered-card-width-percentage, 0.96);
      --settings-main-basis: calc(
          var(--nexus-centered-card-max-width) /
          var(--nexus-centered-card-width-percentage));

      display: flex;
      flex: 1 1 auto;
      background: var(--md-background-color);
      min-height: 0;
      overflow: hidden;
      width: 100%;
      -webkit-app-region: no-drag;
    }

    #container {
      display: flex;
      flex: 1 1 auto;
      background: var(--md-background-color);
      min-height: 0;
      overflow: hidden;
      position: relative;
      width: 100%;
    }

    #left,
    #main,
    #right {
      flex: 1 1 0;
      min-height: 0;
      min-width: 0;
    }

    #left {
      overflow-x: hidden;
      overflow-y: auto;
    }

    #leftSpacer {
      align-items: center;
      box-sizing: border-box;
      display: flex;
      padding-bottom: 24px;
      padding-inline-start: 24px;
      padding-top: 24px;
    }

    #leftSpacer h1 {
      color: var(--cr-secondary-text-color, var(--text-secondary));
      font-size: 1.1rem;
      font-weight: 500;
      margin: 0;
      padding-inline-start: 12px;
    }

    #product-logo {
      height: 24px;
      width: 24px;
    }

    #main {
      display: flex;
      flex-basis: var(--settings-main-basis);
      min-width: 0;
      overflow: hidden;
      padding-top: 24px;
    }

    #right {
      pointer-events: none;
    }

    nav {
      color: var(--google-grey-700);
      display: flex;
      flex-direction: column;
      max-height: 100%;
      min-width: 0;
      overscroll-behavior: contain;
      width: var(--settings-menu-width);
    }

    .cr-nav-menu-item {
      --iron-icon-fill-color: var(--google-grey-700);
      --cr-icon-ripple-size: 20px;
      align-items: center;
      background: transparent;
      border: 0;
      border-end-end-radius: 100px;
      border-start-end-radius: 100px;
      box-sizing: border-box;
      color: var(--google-grey-900);
      cursor: pointer;
      display: flex;
      font: inherit;
      font-size: 14px;
      font-weight: 500;
      line-height: 14px;
      margin-inline-end: 2px;
      margin-inline-start: 1px;
      min-height: 40px;
      overflow: hidden;
      padding-block-end: 10px;
      padding-block-start: 10px;
      padding-inline-end: 16px;
      padding-inline-start: 23px;
      position: relative;
      text-align: start;
      text-decoration: none;
      width: calc(100% - 3px);
    }

    .cr-nav-menu-item:hover {
      background: var(--google-grey-200);
    }

    .cr-nav-menu-item[selected] {
      --iron-icon-fill-color: var(--google-blue-600);
      background: var(--google-blue-50);
      color: var(--google-blue-700);
    }

    .cr-nav-menu-item:focus {
      outline: auto 5px -webkit-focus-ring-color;
      z-index: 1;
    }

    .cr-nav-menu-item:focus:not([selected]):not(:hover) {
      background: transparent;
    }

    .cr-nav-menu-item cr-icon {
      flex-shrink: 0;
      margin-inline-end: 20px;
      pointer-events: none;
      vertical-align: top;
    }

    .cr-nav-menu-item span {
      min-width: 0;
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
    }

    .menu-spacer {
      min-height: 8px;
    }

    #main > * {
      display: flex;
      flex: 1 1 auto;
      min-height: 0;
      min-width: 0;
    }

    #main > [hidden] {
      display: none !important;
    }

    @media (prefers-color-scheme: dark) {
      nav {
        color: var(--cr-primary-text-color, white);
      }

      .cr-nav-menu-item {
        --iron-icon-fill-color: var(--google-grey-500);
        color: white;
      }

      .cr-nav-menu-item:hover {
        --iron-icon-fill-color: white;
        background: var(--google-grey-800);
      }

      .cr-nav-menu-item[selected] {
        --iron-icon-fill-color: black;
        background: var(--google-blue-300);
        color: var(--google-grey-900);
      }
    }

    @media (max-width: 980px) {
      #container {
        flex-direction: column;
      }

      #left {
        overflow: visible;
      }

      nav {
        width: 100%;
      }

      .cr-nav-menu-item {
        border-radius: 100px;
        margin-inline-end: 0;
        width: 100%;
      }

      #main {
        flex-basis: auto;
      }

      #right {
        display: none;
      }
    }
  `;

  declare selectedSection: NexusSettingsSection;

  constructor() {
    super();
    this.selectedSection = 'models';
  }

  private getNavItems_(): SettingsNavItem[] {
    return [
      {
        id: 'models',
        icon: 'cr:domain',
        title: getString(
            'customBrowserNexusProviderPageTitle', 'Model Providers'),
      },
      {
        id: 'webapp',
        icon: 'cr:open-in-new',
        title: getString(
            'customBrowserNexusWebappPageTitle', 'Web App URL'),
      },
    ];
  }

  private updateSelectedSection_(section: NexusSettingsSection) {
    if (section === this.selectedSection) {
      return;
    }

    this.selectedSection = section;
    this.dispatchEvent(new CustomEvent('nexus-settings-section-change', {
      bubbles: true,
      composed: true,
      detail: {section},
    }));
  }

  private onNavItemClick_(section: NexusSettingsSection) {
    this.updateSelectedSection_(section);
  }

  protected override render() {
    const navItems = this.getNavItems_();

    return html`
      <div id="container">
        <div id="left">
          <div id="leftSpacer">
            <slot name="product-logo">
              <picture>
                <source media="(prefers-color-scheme: dark)" srcset="//resources/images/chrome_logo_dark.svg">
                <img id="product-logo" srcset="chrome://theme/current-channel-logo@1x 1x,
                            chrome://theme/current-channel-logo@2x 2x" role="presentation">
              </picture>
            </slot>
            <h1>Settings</h1>
          </div>
          <nav aria-label=${getString(
              'customBrowserNexusSectionTitle', 'OpenClaw settings')}>
            <div class="menu-spacer"></div>
            ${navItems.map(item => html`
              <button class="cr-nav-menu-item"
                  type="button"
                  ?selected=${item.id === this.selectedSection}
                  aria-current=${item.id === this.selectedSection ? 'page' :
                                                                  'false'}
                  @click=${() => this.onNavItemClick_(item.id)}>
                <cr-icon icon=${item.icon}></cr-icon>
                <span>${item.title}</span>
                <cr-ripple></cr-ripple>
              </button>
            `)}
          </nav>
        </div>

        <section id="main">
          <nexus-settings-models
              ?hidden=${this.selectedSection !== 'models'}>
          </nexus-settings-models>
          <nexus-settings-webapp
              ?hidden=${this.selectedSection !== 'webapp'}>
          </nexus-settings-webapp>
        </section>

        <div id="right"></div>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'nexus-settings': NexusSettingsElement;
  }
}

customElements.define(NexusSettingsElement.is, NexusSettingsElement);
