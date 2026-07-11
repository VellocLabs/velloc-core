import 'chrome://resources/cr_elements/cr_icon_button/cr_icon_button.js';
import 'chrome://resources/cr_elements/cr_ripple/cr_ripple.js';
import 'chrome://resources/cr_elements/icons.html.js';

import {
  CrLitElement,
  css,
  html,
  type CSSResultGroup,
} from 'chrome://resources/lit/v3_0/lit.rollup.js';

import {
  browserModeIcon,
  cpuIcon,
  cubeIcon,
  infoIcon,
  keyboardIcon,
  shieldIcon,
} from './nexus_icons.js';
import {
  getString,
  type NexusSettingsSection,
} from './nexus_shared.js';
import './nexus_settings_about.js';
import './nexus_settings_mcp.js';
import './nexus_settings_models.js';
import './nexus_settings_permissions.js';
import './nexus_settings_shortcuts.js';
import './nexus_settings_tools.js';

type SettingsNavItem = {
  icon: ReturnType<typeof html>;
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
      flex-direction: column;
      background: var(--md-background-color);
      min-height: 0;
      overflow: hidden;
      width: 100%;
      -webkit-app-region: no-drag;
    }

    /* ===== Settings header (back button + title) ===== */
    #settings-header {
      align-items: center;
      background: var(--md-background-color);
      border-bottom: 1px solid var(--cr-separator-color);
      box-sizing: border-box;
      display: flex;
      flex-shrink: 0;
      gap: 12px;
      height: 56px;
      padding: 0 16px;
    }

    #back-button {
      --cr-icon-button-fill-color: var(--cr-primary-text-color);
      --cr-icon-button-icon-size: 20px;
      --cr-icon-button-size: 36px;
      flex-shrink: 0;
    }

    #settings-title {
      color: var(--cr-primary-text-color, var(--text-primary));
      font-size: 16px;
      font-weight: 500;
      letter-spacing: -0.005em;
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

    #main,
    #right {
      flex: 1 1 0;
      min-height: 0;
      min-width: 0;
    }

    #left {
      flex: 0 0 var(--settings-menu-width);
      min-height: 0;
      min-width: 0;
      overflow-x: hidden;
      overflow-y: auto;
      padding-top: 16px;
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

    .cr-nav-menu-item > svg {
      color: var(--google-grey-700) !important;
      display: block !important;
      flex-shrink: 0 !important;
      height: 20px !important;
      margin-inline-end: 20px !important;
      pointer-events: none;
      width: 20px !important;
    }

    .cr-nav-menu-item[selected] > svg {
      color: var(--google-blue-600) !important;
    }

    .cr-nav-menu-item span {
      flex: 1;
      min-width: 0;
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
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
        color: white;
      }

      .cr-nav-menu-item > svg {
        color: var(--google-grey-500) !important;
      }

      .cr-nav-menu-item:hover {
        background: var(--google-grey-800);
      }

      .cr-nav-menu-item:hover > svg {
        color: white !important;
      }

      .cr-nav-menu-item[selected] {
        background: var(--google-blue-300);
        color: var(--google-grey-900);
      }

      .cr-nav-menu-item[selected] > svg {
        color: black !important;
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
        icon: cubeIcon,
        title: getString(
            'customBrowserNexusProviderPageTitle', 'Model Providers'),
      },
      {
        id: 'mcp',
        icon: cpuIcon,
        title: getString('customBrowserNexusMcpPageTitle', 'Agent · MCP'),
      },
      {
        id: 'permissions',
        icon: shieldIcon,
        title: getString(
            'customBrowserNexusPermissionsPageTitle', 'Permissions'),
      },
      {
        id: 'tools',
        icon: browserModeIcon,
        title: getString(
            'customBrowserNexusToolsPageTitle', 'Tools · Browser'),
      },
      {
        id: 'shortcuts',
        icon: keyboardIcon,
        title: getString(
            'customBrowserNexusShortcutsPageTitle', 'Keyboard Shortcuts'),
      },
      {
        id: 'about',
        icon: infoIcon,
        title: getString('customBrowserNexusAboutPageTitle', 'About'),
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

  private onBackClick_() {
    this.dispatchEvent(new CustomEvent(
        'nexus-settings-close', {bubbles: true, composed: true}));
  }

  protected override render() {
    const navItems = this.getNavItems_();

    return html`
      <div id="settings-header">
        <cr-icon-button id="back-button"
            iron-icon="cr:arrow-back"
            aria-label=${getString(
                'customBrowserNexusCloseSettings', 'Close settings')}
            title=${getString(
                'customBrowserNexusCloseSettings', 'Close settings')}
            @click=${this.onBackClick_}>
        </cr-icon-button>
        <div id="settings-title">${getString(
            'customBrowserNexusSettingsTitle', 'Settings')}</div>
      </div>

      <div id="container">
        <div id="left">
          <nav aria-label=${getString(
              'customBrowserNexusSectionTitle', 'Nexus settings')}>
            ${navItems.map(item => html`
              <button class="cr-nav-menu-item"
                  type="button"
                  ?selected=${item.id === this.selectedSection}
                  aria-current=${item.id === this.selectedSection ? 'page' :
                                                                  'false'}
                  @click=${() => this.onNavItemClick_(item.id)}>
                ${item.icon}
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
          <nexus-settings-mcp
              ?hidden=${this.selectedSection !== 'mcp'}>
          </nexus-settings-mcp>
          <nexus-settings-permissions
              ?hidden=${this.selectedSection !== 'permissions'}>
          </nexus-settings-permissions>
          <nexus-settings-tools
              ?hidden=${this.selectedSection !== 'tools'}>
          </nexus-settings-tools>
          <nexus-settings-shortcuts
              ?hidden=${this.selectedSection !== 'shortcuts'}>
          </nexus-settings-shortcuts>
          <nexus-settings-about
              ?hidden=${this.selectedSection !== 'about'}>
          </nexus-settings-about>
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
