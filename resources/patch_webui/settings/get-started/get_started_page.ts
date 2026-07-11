import 'chrome://resources/cr_elements/cr_shared_vars.css.js';

import '/strings.m.js';
import {addWebUiListener, removeWebUiListener} from
    'chrome://resources/js/cr.js';
import type {WebUiListener} from 'chrome://resources/js/cr.js';
import {loadTimeData} from 'chrome://resources/js/load_time_data.js';
import {CrLitElement, css, html, type CSSResultGroup} from 'chrome://resources/lit/v3_0/lit.rollup.js';
import type {CrToggleElement} from
    'chrome://resources/cr_elements/cr_toggle/cr_toggle.js';
import {SearchEnginesBrowserProxyImpl} from
    'chrome://settings/settings.js';
import type {SearchEngine, SearchEnginesInfo} from
    'chrome://settings/settings.js';

const DEBUG_GET_STARTED =
    new URLSearchParams(window.location.search).has('getStartedDebug');

function debugLog(...args: unknown[]) {
  if (DEBUG_GET_STARTED) {
    console.info('[get-started]', ...args);
  }
}

let layoutControlsReady: Promise<void>|null = null;

function ensureLayoutControlsReady(): Promise<void> {
  if (layoutControlsReady) {
    return layoutControlsReady;
  }
  layoutControlsReady = (async () => {
    const imports: Array<Promise<unknown>> = [];
    const rippleDefined = !!customElements.get('cr-ripple');
    debugLog('layout controls init', {
      rippleDefined,
      cardDefined: !!customElements.get('cr-card-radio-button'),
      groupDefined: !!customElements.get('cr-radio-group'),
    });
    if (!customElements.get('cr-card-radio-button') && !rippleDefined) {
      imports.push(
          import('chrome://resources/cr_elements/cr_radio_button/cr_card_radio_button.js'));
    } else if (!customElements.get('cr-card-radio-button') && rippleDefined) {
      debugLog('skip card import: cr-ripple already defined');
    }
    if (!customElements.get('cr-radio-group')) {
      imports.push(
          import('chrome://resources/cr_elements/cr_radio_group/cr_radio_group.js'));
    }
    if (imports.length) {
      try {
        await Promise.all(imports);
        debugLog('layout controls imported');
      } catch (error) {
        debugLog('layout controls import failed', error);
      }
    }
    debugLog('layout controls ready', {
      cardDefined: !!customElements.get('cr-card-radio-button'),
      groupDefined: !!customElements.get('cr-radio-group'),
    });
  })();
  return layoutControlsReady;
}

function getPrefPath(key: string, fallback: string): string {
  try {
    if (!loadTimeData.isInitialized()) {
      return fallback;
    }
    const value = loadTimeData.getString(key);
    return value || fallback;
  } catch {
    return fallback;
  }
}

const LAYOUT_PREF_PATH =
    getPrefPath('customBrowserUIModePrefPath',
                'custom_browser.pref.kUIMode');
const SHOW_SEARCH_PREF_PATH =
    getPrefPath('customNewTabShowSearchPrefPath',
                'custom_browser.pref.new_tab.show_search');
const SHOW_LOGO_PREF_PATH =
    getPrefPath('customNewTabShowLogoPrefPath',
                'custom_browser.pref.new_tab.show_logo');

const LayoutOptionName = {
  SIDEBAR: 'sidebar',
  COMPACT_SIDEBAR: 'compactSidebar',
  TOP_TABS: 'topTabs',
  COMPACT_TOP_TABS: 'compactTopTabs',
} as const;

type LayoutOptionNameType =
    (typeof LayoutOptionName)[keyof typeof LayoutOptionName];

const LayoutRadioToPrefValue = new Map<LayoutOptionNameType, number>([
  [LayoutOptionName.SIDEBAR, 1],
  [LayoutOptionName.COMPACT_SIDEBAR, 2],
  [LayoutOptionName.TOP_TABS, 3],
  [LayoutOptionName.COMPACT_TOP_TABS, 4],
]);

const PrefValueToLayoutRadio =
    new Map<number, LayoutOptionNameType>(
        Array.from(LayoutRadioToPrefValue.entries())
            .map(([name, value]) => [value, name]));

interface LayoutOption {
  name: LayoutOptionNameType;
  title: string;
  tag: string;
  description: string;
  disabled?: boolean;
}

interface GetStartedStrings {
  logoAlt: string;
  pageTitle: string;
  subtitle: string;
  bringTitle: string;
  bringDescription: string;
  importButtonLabel: string;
  importSkipLabel: string;
  importMicrocopy: string;
  layoutTitle: string;
  layoutDescription: string;
  layoutHint: string;
  layoutSidebarTitle: string;
  layoutSidebarDescription: string;
  layoutCompactSidebarTitle: string;
  layoutCompactSidebarDescription: string;
  layoutTopTabsTitle: string;
  layoutTopTabsDescription: string;
  layoutCompactTopTabsTitle: string;
  layoutCompactTopTabsDescription: string;
  newTabTitle: string;
  newTabDescription: string;
  newTabShowSearchLabel: string;
  newTabShowLogoLabel: string;
  newTabSearchEngineLabel: string;
  newTabSearchEngineChangeLabel: string;
  loadingLabel: string;
  searchBrandingTitle: string;
  searchBrandingDescription: string;
  toolsTitle: string;
  toolsDescription: string;
  toolUblockTitle: string;
  toolUblockStatus: string;
  toolUblockDescription: string;
  toolTamperTitle: string;
  toolTamperStatus: string;
  toolTamperDescription: string;
  toolsManageButton: string;
  toolsFootnote: string;
  finishTitle: string;
  finishDescription: string;
  finishPrimary: string;
  finishSecondary: string;
  finishTertiary: string;
  badgeRecommended: string;
  badgeMinimal: string;
  badgeComingSoon: string;
  badgeAutomatic: string;
  badgeAdvanced: string;
  badgeOptional: string;
}

function getString(key: string, fallback: string): string {
  try {
    if (!loadTimeData.isInitialized()) {
      return fallback;
    }
    const value = loadTimeData.getString(key);
    return value || fallback;
  } catch {
    return fallback;
  }
}

function getStrings(): GetStartedStrings {
  return {
    logoAlt: getString(
        'customBrowserGetStartedLogoAlt', 'Chromium logo'),
    pageTitle: getString(
        'customBrowserGetStartedPageTitle', 'Get started with Chromium'),
    subtitle: getString(
        'customBrowserGetStartedSubtitle',
        'Set up Chromium in a minute: import your data, pick a layout, and ' +
            'tune your New Tab.'),
    bringTitle: getString(
        'customBrowserGetStartedBringTitle', 'Step 1. Bring your stuff'),
    bringDescription: getString(
        'customBrowserGetStartedBringDescription',
        'Import bookmarks, passwords, and settings from another browser—no ' +
            'account required.'),
    importButtonLabel: getString(
        'customBrowserGetStartedImportButton', 'Import'),
    importSkipLabel: getString(
        'customBrowserGetStartedImportSkip', 'Skip for now'),
    importMicrocopy: getString(
        'customBrowserGetStartedImportMicrocopy',
        'Import happens locally on your device.'),
    layoutTitle: getString('customBrowserGetStartedLayoutTitle',
                           'Step 2. Choose your layout'),
    layoutDescription: getString(
        'customBrowserGetStartedLayoutDescription',
        'Pick the tab style that fits how you work. You can switch anytime.'),
    layoutHint: getString(
        'customBrowserGetStartedLayoutHint',
        'Layout changes apply instantly.'),
    layoutSidebarTitle: getString(
        'customBrowserGetStartedLayoutSidebarTitle', 'Sidebar Tabs'),
    layoutSidebarDescription: getString(
        'customBrowserGetStartedLayoutSidebarDescription',
        'Your tabs, address bar, and tools in one vertical sidebar.'),
    layoutCompactSidebarTitle: getString(
        'customBrowserGetStartedLayoutCompactSidebarTitle', 'Compact Sidebar'),
    layoutCompactSidebarDescription: getString(
        'customBrowserGetStartedLayoutCompactSidebarDescription',
        'Icon-first sidebar that expands when you hover—great for smaller ' +
            'screens.'),
    layoutTopTabsTitle: getString(
        'customBrowserGetStartedLayoutTopTabsTitle', 'Top Tabs'),
    layoutTopTabsDescription: getString(
        'customBrowserGetStartedLayoutTopTabsDescription',
        'Classic tabs across the top for a familiar Chrome-style layout.'),
    layoutCompactTopTabsTitle: getString(
        'customBrowserGetStartedLayoutCompactTopTabsTitle',
        'Compact Top Tabs'),
    layoutCompactTopTabsDescription: getString(
        'customBrowserGetStartedLayoutCompactTopTabsDescription',
        'A slimmer top layout that leaves more room for what matters: the ' +
            'page.'),
    newTabTitle: getString(
        'customBrowserGetStartedNewTabTitle', 'Step 3. Customize your New Tab'),
    newTabDescription: getString(
        'customBrowserGetStartedNewTabDescription',
        'Keep it clean, or keep search up front.'),
    newTabShowSearchLabel: getString(
        'customizeChromeNtpShowSearchLabel',
        getString('customBrowserGetStartedNewTabToggleLabel',
                  'Show search box')),
    newTabShowLogoLabel: getString(
        'customizeChromeNtpShowLogoLabel', 'Show search provider logo'),
    newTabSearchEngineLabel: getString(
        'customizeChromeNtpSearchEngineLabel', 'Search engine'),
    newTabSearchEngineChangeLabel: getString(
        'searchEnginesChange', 'Change'),
    loadingLabel: getString('customBrowserLoadingLabel', 'Loading...'),
    searchBrandingTitle: getString(
        'customBrowserGetStartedSearchBrandingTitle', 'Your search, your look'),
    searchBrandingDescription: getString(
        'customBrowserGetStartedSearchBrandingDescription',
        'If you choose a non-Google default search, Chromium updates the New ' +
            'Tab branding automatically.'),
    toolsTitle: getString(
        'customBrowserGetStartedToolsTitle',
        'Step 4. Built-in power tools'),
    toolsDescription: getString(
        'customBrowserGetStartedToolsDescription',
        'Chromium includes optional built-in extensions. You stay in control.'),
    toolUblockTitle: getString(
        'customBrowserGetStartedToolUblockTitle', 'uBlock Origin'),
    toolUblockStatus: getString(
        'customBrowserGetStartedToolUblockStatus', 'Enabled by default'),
    toolUblockDescription: getString(
        'customBrowserGetStartedToolUblockDescription',
        'Efficient content blocking for a faster, cleaner web.'),
    toolTamperTitle: getString(
        'customBrowserGetStartedToolTamperTitle', 'Tampermonkey (Legacy)'),
    toolTamperStatus: getString(
        'customBrowserGetStartedToolTamperStatus', 'Off by default'),
    toolTamperDescription: getString(
        'customBrowserGetStartedToolTamperDescription',
        'Advanced userscript manager for deep site customization.'),
    toolsManageButton: getString(
        'customBrowserGetStartedToolsManageButton', 'Manage extensions'),
    toolsFootnote: getString(
        'customBrowserGetStartedToolsFootnote',
        'Some built-in tools may differ from Chrome Web Store availability.'),
    finishTitle: getString(
        'customBrowserGetStartedFinishTitle', 'Step 5. You’re ready'),
    finishDescription: getString(
        'customBrowserGetStartedFinishDescription',
        'Well done. Want to go further? Explore more features next.'),
    finishPrimary: getString(
        'customBrowserGetStartedFinishPrimary', 'Open New Tab'),
    finishSecondary: getString(
        'customBrowserGetStartedFinishSecondary', 'Go to Settings'),
    finishTertiary: getString(
        'customBrowserGetStartedFinishTertiary', 'Extensions →'),
    badgeRecommended: getString(
        'customBrowserBadgeRecommended', 'Recommended'),
    badgeMinimal: getString('customBrowserBadgeMinimal', 'Minimal'),
    badgeComingSoon: getString('customBrowserBadgeComingSoon', 'Coming soon'),
    badgeAutomatic: getString('customBrowserBadgeAutomatic', 'Automatic'),
    badgeAdvanced: getString('customBrowserBadgeAdvanced', 'Advanced'),
    badgeOptional: getString('customBrowserBadgeOptional', 'Optional'),
  };
}

export class GetStartedPage extends CrLitElement {
  static get is() {
    return 'get-started-page';
  }

  static override styles: CSSResultGroup = css`
    :host {
      position: fixed;
      inset: 0;
      display: none;
      overflow: auto;
      background: var(--md-background-color, #f8f9fa);
      color: var(--cr-primary-text-color, #202124);
      z-index: 10000;
    }

    :host([active]) {
      display: block;
    }

    *,
    *::before,
    *::after {
      box-sizing: border-box;
    }

    main {
      min-height: 100%;
      padding: 32px 24px 48px;
    }

    .container {
      max-width: 1040px;
      margin: 0 auto;
      display: flex;
      flex-direction: column;
      gap: 16px;
    }

    .hero h1 {
      margin: 0;
      font-size: 28px;
      font-weight: 500;
      line-height: 36px;
    }

    .hero p {
      margin: 0;
      font-size: 16px;
      line-height: 24px;
      color: var(--cr-secondary-text-color);
    }

    .hero-title {
      display: flex;
      align-items: center;
      gap: 12px;
      margin-bottom: 8px;
    }

    .product-logo {
      width: 56px;
      height: 56px;
    }

    .section-card {
      background: var(--cr-card-background-color);
      border-radius: 12px;
      box-shadow: var(--cr-elevation-1);
      padding: 20px 24px;
    }

    .section-card h2 {
      margin: 0;
      font-size: 18px;
      font-weight: 500;
      line-height: 24px;
    }

    .section-header {
      display: flex;
      align-items: center;
      gap: 8px;
      margin-bottom: 6px;
    }

    .section-icon {
      width: 20px;
      height: 20px;
      color: var(--cr-secondary-text-color);
    }

    .section-desc {
      margin: 0;
      color: var(--cr-secondary-text-color);
      font-size: 14px;
      line-height: 20px;
    }

    .section-actions {
      margin-top: 16px;
      display: flex;
      align-items: center;
      gap: 12px;
      flex-wrap: wrap;
    }

    .layout-grid {
      margin-top: 16px;
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(300px, 1fr));
      gap: 16px;
      --cr-radio-group-item-padding: 0;
    }

    cr-card-radio-button.layout-option {
      width: 100%;
      --cr-card-radio-button-margin: 0;
      --cr-card-radio-button-width: 100%;
      --cr-card-radio-button-checkmark-right: 8px;
      --cr-card-radio-button-checkmark-top: 8px;
      --cr-card-radio-button-padding: 0;
      border-radius: 12px;
      transition: box-shadow 0.15s ease, outline-color 0.15s ease;
      outline: 2px solid transparent;
      outline-offset: -2px;
    }

    cr-card-radio-button.layout-option .layoutContainer {
      width: 100%;
      padding: 16px;
    }

    cr-card-radio-button.layout-option[disabled] {
      opacity: 0.6;
    }

    cr-card-radio-button.layout-option:not([disabled]):not([checked]):hover {
      outline-color: var(--google-blue-400);
      box-shadow: 0 4px 12px rgba(0, 0, 0, 0.12);
    }

    .layout-option.native {
      position: relative;
      display: block;
      width: 100%;
      padding: 16px;
      border-radius: 12px;
      border: 2px solid var(--cr-separator-color);
      background: var(--cr-card-background-color);
      box-shadow: var(--cr-elevation-1);
      cursor: pointer;
      transition: box-shadow 0.15s ease, border-color 0.15s ease,
                  background 0.15s ease;
    }

    .layout-option.native input {
      position: absolute;
      inset: 0;
      margin: 0;
      opacity: 0;
      cursor: pointer;
    }

    .layout-option.native:focus-within {
      box-shadow: 0 0 0 2px var(--cr-focus-outline-color);
    }

    .layout-option.native .native-check {
      position: absolute;
      top: 10px;
      right: 10px;
      width: 20px;
      height: 20px;
      --cr-icon-color: var(--google-green-700);
      --iron-icon-fill-color: var(--google-green-700);
      opacity: 0;
      border-radius: 999px;
      transform: scale(0.8);
      transition: opacity 0.15s ease, transform 0.15s ease;
      pointer-events: none;
    }


    .layout-option.native.checked .native-check {
      opacity: 1;
      transform: scale(1);
    }

    .layout-option.native:hover:not(.disabled) {
      border-color: var(--google-green-300);
      box-shadow: 0 4px 12px rgba(0, 0, 0, 0.12);
    }

    .layout-option.native.disabled {
      cursor: default;
      opacity: 0.6;
    }

    .layout-option.native.disabled input {
      cursor: default;
    }

    .layout-option.native .layoutContainer {
      padding: 0;
    }

    .option-header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 8px;
      margin-bottom: 8px;
      flex-wrap: wrap;
    }

    .option-title {
      font-size: 15px;
      font-weight: 500;
      line-height: 20px;
      white-space: nowrap;
      flex: 1 1 auto;
      min-width: 0;
    }

    .option-desc {
      margin: 0;
      color: var(--cr-secondary-text-color);
      font-size: 13px;
      line-height: 18px;
    }

    .badge {
      display: inline-flex;
      align-items: center;
      padding: 2px 8px;
      border-radius: 999px;
      border: 1px solid var(--cr-separator-color);
      font-size: 11px;
      font-weight: 500;
      color: var(--cr-secondary-text-color);
      background: var(--cr-card-background-color);
      white-space: nowrap;
      margin-left: auto;
    }

    .layout-option.native.checked .badge {
      background: var(--google-green-50);
      color: var(--google-green-700);
    }

    .hint {
      margin: 12px 0 0;
      font-size: 12px;
      color: var(--cr-secondary-text-color);
    }

    .microcopy {
      margin: 12px 0 0;
      font-size: 12px;
      color: var(--cr-secondary-text-color);
    }

    .badge-optional {
      background: var(--google-grey-100);
      border-color: var(--google-grey-300);
    }

    @media (prefers-color-scheme: dark) {
      .badge-optional {
        background: var(--google-grey-800);
        border-color: var(--google-grey-700);
      }
    }

    .toggle-row {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 12px;
      margin-top: 12px;
      padding: 10px 0;
      cursor: pointer;
    }

    .toggle-row[disabled] {
      cursor: default;
      opacity: 0.7;
    }

    .toggle-label {
      font-size: 14px;
      font-weight: 500;
      line-height: 20px;
    }

    .toggle-status {
      margin-top: 6px;
      font-size: 12px;
      color: var(--cr-secondary-text-color);
    }

    .settings-rows {
      margin-top: 12px;
      border: 1px solid var(--cr-separator-color);
      border-radius: 8px;
      overflow: hidden;
    }

    .settings-row {
      display: flex;
      align-items: center;
      gap: 12px;
      padding: 12px 16px;
      cursor: pointer;
      transition: background-color 0.15s ease;
      border-bottom: 1px solid var(--cr-separator-color);
    }

    .settings-row:last-child {
      border-bottom: none;
    }

    .settings-row:hover {
      background-color: var(--cr-hover-background-color, rgba(0, 0, 0, 0.04));
    }

    .settings-row:active {
      background-color: var(--cr-active-background-color, rgba(0, 0, 0, 0.08));
    }

    .settings-row[disabled] {
      cursor: default;
      opacity: 0.7;
    }

    .settings-row[disabled]:hover {
      background-color: transparent;
    }

    .row-icon {
      --site-favicon-border-radius: 4px;
      --site-favicon-height: 20px;
      --site-favicon-width: 20px;
      flex-shrink: 0;
    }

    .row-content {
      flex: 1;
      min-width: 0;
    }

    .row-label {
      font-size: 14px;
      font-weight: 500;
      line-height: 20px;
    }

    .row-sublabel {
      font-size: 13px;
      color: var(--cr-secondary-text-color);
      line-height: 18px;
      margin-top: 2px;
    }

    .row-arrow {
      width: 20px;
      height: 20px;
      color: var(--cr-secondary-text-color);
      flex-shrink: 0;
    }

    .search-engine-row {
      margin-top: 12px;
      display: flex;
      align-items: center;
      gap: 12px;
      padding: 10px 0;
    }

    .search-engine-name {
      flex: 1;
      min-width: 0;
      font-size: 14px;
      line-height: 20px;
    }

    .search-engine-row site-favicon {
      --site-favicon-border-radius: 4px;
      --site-favicon-height: 16px;
      --site-favicon-width: 16px;
    }

    .title-row {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 12px;
      margin-bottom: 6px;
    }

    .title-group {
      display: flex;
      align-items: center;
      gap: 8px;
    }

    .tools-grid {
      margin-top: 16px;
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(280px, 1fr));
      gap: 16px;
    }

    .tool-card {
      border: 1px solid var(--cr-separator-color);
      border-radius: 12px;
      padding: 16px;
      display: flex;
      flex-direction: column;
      gap: 8px;
      background: var(--cr-card-background-color);
    }

    .tool-card-featured {
      flex-direction: row;
      align-items: flex-start;
      gap: 16px;
      padding: 20px;
      border: 2px solid var(--cr-separator-color);
      box-shadow: 0 2px 8px rgba(0, 0, 0, 0.08);
    }

    .tool-icon-wrapper {
      width: 48px;
      height: 48px;
      border-radius: 12px;
      background: var(--google-blue-50);
      display: flex;
      align-items: center;
      justify-content: center;
      flex-shrink: 0;
    }

    .tool-icon-wrapper.tool-icon-muted {
      background: var(--google-grey-100);
    }

    @media (prefers-color-scheme: dark) {
      .tool-icon-wrapper {
        background: var(--google-blue-900);
      }

      .tool-icon-wrapper.tool-icon-muted {
        background: var(--google-grey-800);
      }
    }

    .tool-icon {
      width: 24px;
      height: 24px;
      color: var(--google-blue-600);
    }

    .tool-icon-muted .tool-icon {
      color: var(--cr-secondary-text-color);
    }

    .tool-content {
      flex: 1;
      min-width: 0;
      display: flex;
      flex-direction: column;
      gap: 4px;
    }

    .tool-title-row {
      display: flex;
      align-items: center;
      justify-content: flex-start;
      gap: 8px;
      flex-wrap: wrap;
    }

    .tool-title {
      font-size: 15px;
      font-weight: 600;
      line-height: 20px;
    }

    .tool-status {
      font-size: 12px;
      color: var(--cr-secondary-text-color);
    }

    .tool-desc {
      margin: 0;
      font-size: 13px;
      line-height: 18px;
      color: var(--cr-secondary-text-color);
    }

    .badge-enabled {
      background: var(--google-green-50);
      border-color: var(--google-green-300);
      color: var(--google-green-700);
    }

    @media (prefers-color-scheme: dark) {
      .badge-enabled {
        background: var(--google-green-900);
        border-color: var(--google-green-700);
        color: var(--google-green-300);
      }
    }

    .tool-card cr-button {
      align-self: flex-start;
      margin-top: 4px;
    }

    .footnote {
      margin: 12px 0 0;
      font-size: 12px;
      color: var(--cr-secondary-text-color);
    }

    .section-separator {
      border: none;
      border-top: 1px solid var(--cr-separator-color);
      margin: 12px 0;
    }

    .footer-actions {
      margin-top: 16px;
      display: flex;
      align-items: center;
      flex-wrap: wrap;
      gap: 12px;
    }

    .footer-link {
      color: var(--cr-link-color, var(--google-blue-600));
      font-weight: 500;
      text-decoration: none;
    }

    .footer-link:hover {
      text-decoration: underline;
    }

  `;

  static override get properties() {
    return {
      layoutSelection_: {type: String},
      layoutDisabled_: {type: Boolean},
      useNativeLayoutControls_: {type: Boolean},
      showSearch_: {type: Boolean},
      searchDisabled_: {type: Boolean},
      showLogo_: {type: Boolean},
      logoDisabled_: {type: Boolean},
      searchEngines_: {type: Array},
      defaultSearchEngine_: {type: Object},
    };
  }

  private strings_: GetStartedStrings = getStrings();
  accessor layoutSelection_: LayoutOptionNameType = LayoutOptionName.SIDEBAR;
  accessor layoutDisabled_: boolean = false;
  accessor useNativeLayoutControls_: boolean =
      !customElements.get('cr-card-radio-button');
  accessor showSearch_: boolean = true;
  accessor searchDisabled_: boolean = false;
  accessor showLogo_: boolean = true;
  accessor logoDisabled_: boolean = false;
  accessor searchEngines_: SearchEngine[] = [];
  accessor defaultSearchEngine_: SearchEngine|null = null;
  // Non-reactive private members
  private prefListener_: ((prefs: chrome.settingsPrivate.PrefObject[]) => void)|
      null = null;
  private applyingLayoutPref_: boolean = false;
  private importDialog_: HTMLElement|null = null;
  private searchEnginesListener_: WebUiListener|null = null;
  private searchEngineDialogElement_: HTMLElement|null = null;
  private browserProxy_ = SearchEnginesBrowserProxyImpl.getInstance();

  override connectedCallback() {
    super.connectedCallback();
    debugLog('connected');
    const layoutReady = ensureLayoutControlsReady();
    void layoutReady;
    void layoutReady.then(() => this.updateLayoutControlMode_());
    void this.updateComplete.then(() => {
      const radios =
          this.shadowRoot?.querySelectorAll('cr-card-radio-button') ?? [];
      const nativeInputs = this.shadowRoot?.querySelectorAll(
          '.layout-option.native input[type="radio"]') ?? [];
      debugLog('layout radios', {
        count: radios.length,
        items: Array.from(radios).map(radio => ({
          name: (radio as any).name,
          checked: (radio as any).checked,
          disabled: (radio as any).disabled,
        })),
        nativeCount: nativeInputs.length,
        nativeChecked: Array.from(nativeInputs)
                           .filter(input => (input as HTMLInputElement).checked)
                           .map(input => (input as HTMLInputElement).value),
      });
    });
    this.startPrefSync_();
    // Fetch search engines list - same pattern as settings-search-page.
    // Guarded: getSearchEnginesList() crashes the browser when the
    // SearchSettingsUpdate feature is enabled (see useLegacySearchEnginesList_).
    if (this.useLegacySearchEnginesList_()) {
      this.browserProxy_.getSearchEnginesList().then(
          this.handleSearchEnginesResponse_.bind(this));
      // Listen for changes
      if (!this.searchEnginesListener_) {
        this.searchEnginesListener_ = addWebUiListener(
            'search-engines-changed',
            this.handleSearchEnginesResponse_.bind(this));
      }
    }
  }

  // Whether the legacy getSearchEnginesList() WebUI message is safe to call.
  // That handler is gated by `CHECK(!IsEnabled(kSearchSettingsUpdate))` in
  // chrome/browser/ui/webui/settings/search_engines_handler.cc, so invoking it
  // while the SearchSettingsUpdate feature is enabled (e.g. via the field-trial
  // testing config in dev builds) crashes the browser process. The replacement
  // getCategorizedTemplateUrls() API returns @-shortcuts rather than the default
  // engine this page renders, so when the feature is on we skip the inline list;
  // the engine picker falls back to chrome://settings/searchEngines.
  // TODO(crbug.com/494551138): adopt the new API once SearchSettingsUpdate
  // launches and the categorized data carries the default engine.
  private useLegacySearchEnginesList_(): boolean {
    return !(loadTimeData.valueExists('searchSettingsUpdate') &&
             loadTimeData.getBoolean('searchSettingsUpdate'));
  }

  private handleSearchEnginesResponse_(info: SearchEnginesInfo) {
    this.updateSearchEngines_(info);
  }

  override disconnectedCallback() {
    this.stopPrefSync_();
    if (this.searchEnginesListener_) {
      removeWebUiListener(this.searchEnginesListener_);
      this.searchEnginesListener_ = null;
    }
    if (this.searchEngineDialogElement_) {
      this.searchEngineDialogElement_.remove();
      this.searchEngineDialogElement_ = null;
    }
    super.disconnectedCallback();
  }

  override render() {
    const strings = this.strings_;
    const layoutOptions = this.getLayoutOptions_();
    const defaultEngine = this.defaultSearchEngine_;
    const defaultEngineName = defaultEngine?.name ||
        defaultEngine?.displayName || strings.loadingLabel;
    const defaultEngineUrl = defaultEngine?.url || '';
    const defaultEngineIconUrl = defaultEngine?.iconURL || '';
    const defaultEngineIconPath = defaultEngine?.iconPath || '';
    const layoutControls = this.useNativeLayoutControls_ ? html`
      <div class="layout-grid native-layout-grid"
          role="radiogroup"
          aria-label=${strings.layoutTitle}
          aria-disabled=${this.layoutDisabled_ ? 'true' : 'false'}>
        ${layoutOptions.map(option => {
          const isSelected = this.layoutSelection_ === option.name;
          const isDisabled = this.layoutDisabled_ || !!option.disabled;
          return html`
            <label class="layout-option native ${isSelected ? 'checked' : ''} ${isDisabled ? 'disabled' : ''}">
              <input type="radio"
                  name="layoutOption"
                  .value=${option.name}
                  .checked=${isSelected}
                  ?disabled=${isDisabled}
                  @change=${this.onNativeLayoutSelectionChanged_}>
              <cr-icon class="native-check" icon="cr:check-circle"></cr-icon>
              <div class="layoutContainer">
                <div class="option-header">
                  <div class="option-title">${option.title}</div>
                  <span class="badge">${option.tag}</span>
                </div>
                <p class="option-desc">${option.description}</p>
              </div>
            </label>
          `;
        })}
      </div>
    ` : html`
      <cr-radio-group class="layout-grid"
          aria-label=${strings.layoutTitle}
          .selected=${this.layoutSelection_}
          ?disabled=${this.layoutDisabled_}
          @selected-changed=${this.onLayoutSelectionChanged_}>
        ${layoutOptions.map(option => html`
          <cr-card-radio-button class="layout-option"
              .name=${option.name}
              ?disabled=${option.disabled}>
            <div class="layoutContainer">
              <div class="option-header">
                <div class="option-title">${option.title}</div>
                <span class="badge">${option.tag}</span>
              </div>
              <p class="option-desc">${option.description}</p>
            </div>
          </cr-card-radio-button>
        `)}
      </cr-radio-group>
    `;

    return html`
      <main role="main" aria-label=${strings.pageTitle}>
        <div class="container">
          <header class="hero">
            <div class="hero-title">
              <img class="product-logo"
                  src="images/product-logo.svg"
                  alt=${strings.logoAlt}>
              <h1>${strings.pageTitle}</h1>
            </div>
            <p>${strings.subtitle}</p>
          </header>

          <!-- Step 1: Import -->
          <section class="section-card" aria-labelledby="bringTitle">
            <div class="section-header">
              <cr-icon class="section-icon" icon="cr:file-download"></cr-icon>
              <h2 id="bringTitle">${strings.bringTitle}</h2>
            </div>
            <p class="section-desc">${strings.bringDescription}</p>
            <div class="section-actions">
              <cr-button class="action-button"
                  @click=${this.onImportDataClick_}>
                ${strings.importButtonLabel}
              </cr-button>
              <cr-button @click=${this.onSkipImportClick_}>
                ${strings.importSkipLabel}
              </cr-button>
            </div>
            <p class="microcopy">${strings.importMicrocopy}</p>
          </section>

          <!-- Step 2: Layout -->
          <section class="section-card" aria-labelledby="layoutTitle">
            <div class="section-header">
              <cr-icon class="section-icon" icon="cr:domain"></cr-icon>
              <h2 id="layoutTitle">${strings.layoutTitle}</h2>
            </div>
            <p class="section-desc">${strings.layoutDescription}</p>
            ${layoutControls}
            <p class="hint">${strings.layoutHint}</p>
          </section>

          <!-- Step 3: New Tab -->
          <section class="section-card" aria-labelledby="newTabTitle">
            <div class="section-header">
              <cr-icon class="section-icon" icon="cr:open-in-new"></cr-icon>
              <h2 id="newTabTitle">${strings.newTabTitle}</h2>
            </div>
            <p class="section-desc">${strings.newTabDescription}</p>
            <div class="settings-rows">
              <div class="settings-row"
                  ?disabled=${this.searchDisabled_}
                  @click=${this.onSearchToggleRowClick_}>
                <div class="row-content">
                  <div class="row-label">${strings.newTabShowSearchLabel}</div>
                </div>
                <cr-toggle .checked=${this.showSearch_}
                    ?disabled=${this.searchDisabled_}
                    @change=${this.onSearchToggle_}>
                </cr-toggle>
              </div>
              <div class="settings-row"
                  ?disabled=${this.logoDisabled_}
                  @click=${this.onLogoToggleRowClick_}>
                <div class="row-content">
                  <div class="row-label">${strings.newTabShowLogoLabel}</div>
                </div>
                <cr-toggle .checked=${this.showLogo_}
                    ?disabled=${this.logoDisabled_}
                    @change=${this.onLogoToggle_}>
                </cr-toggle>
              </div>
              <div class="settings-row"
                  @click=${this.onOpenSearchEngineDialog_}>
                <site-favicon class="row-icon" .faviconUrl=${defaultEngineIconUrl}
                    .url=${defaultEngineUrl}
                    .iconPath=${defaultEngineIconPath}>
                </site-favicon>
                <div class="row-content">
                  <div class="row-label">${strings.newTabSearchEngineLabel}</div>
                  <div class="row-sublabel">${defaultEngineName}</div>
                </div>
                <cr-icon class="row-arrow" icon="cr:chevron-right"></cr-icon>
              </div>
            </div>
          </section>

          <!-- Step 4: Built-in power tools -->
          <section class="section-card" aria-labelledby="toolsTitle">
            <div class="section-header">
              <cr-icon class="section-icon" icon="cr:extension"></cr-icon>
              <h2 id="toolsTitle">${strings.toolsTitle}</h2>
              <span class="badge badge-optional">${strings.badgeOptional}</span>
            </div>
            <p class="section-desc">${strings.toolsDescription}</p>
            <div class="tools-grid">
              <article class="tool-card tool-card-featured">
                <div class="tool-icon-wrapper">
                  <cr-icon class="tool-icon" icon="cr:extension"></cr-icon>
                </div>
                <div class="tool-content">
                  <div class="tool-title-row">
                    <div class="tool-title">${strings.toolUblockTitle}</div>
                    <span class="badge badge-enabled">${strings.toolUblockStatus}</span>
                  </div>
                  <p class="tool-desc">${strings.toolUblockDescription}</p>
                </div>
              </article>

              <article class="tool-card tool-card-featured">
                <div class="tool-icon-wrapper tool-icon-muted">
                  <cr-icon class="tool-icon" icon="cr:extension"></cr-icon>
                </div>
                <div class="tool-content">
                  <div class="tool-title-row">
                    <div class="tool-title">${strings.toolTamperTitle}</div>
                    <span class="badge">${strings.badgeAdvanced}</span>
                  </div>
                  <div class="tool-status">${strings.toolTamperStatus}</div>
                  <p class="tool-desc">${strings.toolTamperDescription}</p>
                </div>
              </article>
            </div>
            <p class="footnote">${strings.toolsFootnote}</p>
            <div class="section-actions">
              <cr-button @click=${this.onManageExtensionsClick_}>
                ${strings.toolsManageButton}
              </cr-button>
            </div>
          </section>

          <!-- Step 5: Ready -->
          <section class="section-card" aria-labelledby="finishTitle">
            <div class="section-header">
              <cr-icon class="section-icon" icon="cr:check-circle"></cr-icon>
              <h2 id="finishTitle">${strings.finishTitle}</h2>
            </div>
            <p class="section-desc">${strings.finishDescription}</p>
            <div class="footer-actions">
              <cr-button class="action-button"
                  @click=${this.onOpenNewTabClick_}>
                ${strings.finishPrimary}
              </cr-button>
              <cr-button @click=${this.onGoToSettingsClick_}>
                ${strings.finishSecondary}
              </cr-button>
            </div>
          </section>
        </div>
      </main>
    `;
  }

  private getLayoutOptions_(): LayoutOption[] {
    const strings = this.strings_;
    return [
      {
        name: LayoutOptionName.SIDEBAR,
        title: strings.layoutSidebarTitle,
        tag: strings.badgeRecommended,
        description: strings.layoutSidebarDescription,
      },
      {
        name: LayoutOptionName.COMPACT_SIDEBAR,
        title: strings.layoutCompactSidebarTitle,
        tag: strings.badgeMinimal,
        description: strings.layoutCompactSidebarDescription,
      },
      {
        name: LayoutOptionName.TOP_TABS,
        title: strings.layoutTopTabsTitle,
        tag: "Classic",
        description: strings.layoutTopTabsDescription,
      },
      // {
      //   name: LayoutOptionName.COMPACT_TOP_TABS,
      //   title: strings.layoutCompactTopTabsTitle,
      //   tag: strings.badgeComingSoon,
      //   description: strings.layoutCompactTopTabsDescription,
      //   disabled: true,
      // },
    ];
  }

  private startPrefSync_() {
    const settingsPrivate = chrome.settingsPrivate;
    if (!settingsPrivate) {
      return;
    }
    const prefPaths = [
      LAYOUT_PREF_PATH,
      SHOW_SEARCH_PREF_PATH,
      SHOW_LOGO_PREF_PATH,
    ];
    prefPaths.forEach(path => {
      settingsPrivate.getPref(path)
          .then(pref => this.applyPref_(pref))
          .catch(() => {
            // Ignore failures; listener will handle future updates.
          });
    });

    this.prefListener_ = (prefs: chrome.settingsPrivate.PrefObject[]) => {
      for (const pref of prefs) {
        this.applyPref_(pref);
      }
    };
    settingsPrivate.onPrefsChanged.addListener(this.prefListener_);
  }

  private stopPrefSync_() {
    if (this.prefListener_ && chrome.settingsPrivate) {
      chrome.settingsPrivate.onPrefsChanged.removeListener(this.prefListener_);
    }
    this.prefListener_ = null;
  }

  private updateSearchEngines_(info: SearchEnginesInfo) {
    const defaults = info.defaults ?? [];
    const allEngines = [
      ...defaults,
      ...(info.actives ?? []),
      ...(info.others ?? []),
      ...(info.extensions ?? []),
    ];
    // For the dialog, we only pass the defaults list (matching settings-search-page)
    this.searchEngines_ = defaults.length ? defaults : allEngines;
    // Find the default engine
    this.defaultSearchEngine_ =
        allEngines.find(engine => engine.default) ?? allEngines[0] ?? null;

    // Update the dialog element if it exists
    if (this.searchEngineDialogElement_) {
      (this.searchEngineDialogElement_ as any).searchEngines = this.searchEngines_;
    }
  }

  private async ensureSearchEnginesLoaded_(): Promise<boolean> {
    // If we already have multiple engines, we're ready
    if (this.searchEngines_.length > 1) {
      return true;
    }
    // Fetch fresh list from browser proxy (skipped when the legacy API is
    // unavailable; the caller then falls back to chrome://settings/searchEngines).
    if (this.useLegacySearchEnginesList_()) {
      try {
        const info = await this.browserProxy_.getSearchEnginesList();
        if (info && (info.defaults?.length || info.actives?.length)) {
          this.updateSearchEngines_(info);
          return this.searchEngines_.length > 0;
        }
      } catch {
        // Fetch failed
      }
    }
    // Return true if we have at least one engine (even from previous fetch)
    return this.searchEngines_.length > 0;
  }

  private updateLayoutControlMode_() {
    const cardDefined = !!customElements.get('cr-card-radio-button');
    const nextNative = !cardDefined;
    debugLog('layout control mode', {cardDefined, native: nextNative});
    if (this.useNativeLayoutControls_ !== nextNative) {
      this.useNativeLayoutControls_ = nextNative;
    }
  }

  private applyPref_(pref: chrome.settingsPrivate.PrefObject) {
    if (pref.key === LAYOUT_PREF_PATH) {
      const numericValue =
          typeof pref.value === 'number' ? pref.value : Number(pref.value);
      const selection =
          PrefValueToLayoutRadio.get(numericValue) ?? LayoutOptionName.SIDEBAR;
      debugLog('layout pref', {
        value: pref.value,
        numericValue,
        selection,
        enforcement: pref.enforcement,
      });
      this.applyingLayoutPref_ = true;
      this.layoutSelection_ = selection;
      this.layoutDisabled_ = pref.enforcement === 'ENFORCED';
      void this.updateComplete.then(() => {
        this.applyingLayoutPref_ = false;
      });
      return;
    }
    if (pref.key === SHOW_SEARCH_PREF_PATH) {
      this.showSearch_ = !!pref.value;
      this.searchDisabled_ = pref.enforcement === 'ENFORCED';
      return;
    }
    if (pref.key === SHOW_LOGO_PREF_PATH) {
      this.showLogo_ = !!pref.value;
      this.logoDisabled_ = pref.enforcement === 'ENFORCED';
    }
  }

  private onLayoutSelectionChanged_(event: CustomEvent<{value: string}>) {
    const newSelection = event.detail?.value as LayoutOptionNameType|undefined;
    debugLog('layout selection changed', {
      value: event.detail?.value,
      newSelection,
    });
    if (!newSelection || this.applyingLayoutPref_ ||
        newSelection === this.layoutSelection_) {
      return;
    }
    const option = this.getLayoutOptions_()
                       .find(candidate => candidate.name === newSelection);
    if (option?.disabled) {
      return;
    }
    this.layoutSelection_ = newSelection;
    this.applyLayoutSelection_(newSelection);
  }

  private onNativeLayoutSelectionChanged_(event: Event) {
    const input = event.target as HTMLInputElement | null;
    const value = input?.value as LayoutOptionNameType|undefined;
    debugLog('layout selection changed (native)', {value});
    if (!value || this.layoutDisabled_ || value === this.layoutSelection_) {
      return;
    }
    const option = this.getLayoutOptions_()
                       .find(candidate => candidate.name === value);
    if (option?.disabled) {
      return;
    }
    this.layoutSelection_ = value;
    this.applyLayoutSelection_(value);
  }

  private applyLayoutSelection_(selection: LayoutOptionNameType) {
    const settingsPrivate = chrome.settingsPrivate;
    const prefValue = LayoutRadioToPrefValue.get(selection);
    if (!settingsPrivate || prefValue === undefined || this.layoutDisabled_) {
      return;
    }
    void settingsPrivate.setPref(LAYOUT_PREF_PATH, prefValue);
  }

  private onSearchToggle_(event: Event) {
    const toggle = event.target as CrToggleElement;
    if (!chrome.settingsPrivate || this.searchDisabled_) {
      toggle.checked = this.showSearch_;
      return;
    }
    this.updateSearchPref_(toggle.checked);
  }

  private onLogoToggle_(event: Event) {
    const toggle = event.target as CrToggleElement;
    if (!chrome.settingsPrivate || this.logoDisabled_) {
      toggle.checked = this.showLogo_;
      return;
    }
    this.updateLogoPref_(toggle.checked);
  }

  private isToggleClick_(event: Event): boolean {
    const path = event.composedPath();
    for (const target of path) {
      if (target instanceof HTMLElement && target.tagName === 'CR-TOGGLE') {
        return true;
      }
    }
    return false;
  }

  private onSearchToggleRowClick_(event: Event) {
    if (this.searchDisabled_ || !chrome.settingsPrivate ||
        this.isToggleClick_(event)) {
      return;
    }
    this.updateSearchPref_(!this.showSearch_);
  }

  private onLogoToggleRowClick_(event: Event) {
    if (this.logoDisabled_ || !chrome.settingsPrivate ||
        this.isToggleClick_(event)) {
      return;
    }
    this.updateLogoPref_(!this.showLogo_);
  }

  private updateSearchPref_(nextValue: boolean) {
    const settingsPrivate = chrome.settingsPrivate;
    if (!settingsPrivate || this.searchDisabled_) {
      return;
    }
    this.showSearch_ = nextValue;
    settingsPrivate.setPref(SHOW_SEARCH_PREF_PATH, nextValue)
        .then(success => {
          if (!success) {
            this.showSearch_ = !nextValue;
          }
        });
  }

  private updateLogoPref_(nextValue: boolean) {
    const settingsPrivate = chrome.settingsPrivate;
    if (!settingsPrivate || this.logoDisabled_) {
      return;
    }
    this.showLogo_ = nextValue;
    settingsPrivate.setPref(SHOW_LOGO_PREF_PATH, nextValue)
        .then(success => {
          if (!success) {
            this.showLogo_ = !nextValue;
          }
        });
  }

  private async onOpenSearchEngineDialog_(event: Event) {
    event.preventDefault();
    event.stopPropagation();

    // Prevent opening multiple dialogs
    if (this.searchEngineDialogElement_) {
      return;
    }

    const ready = await this.ensureSearchEnginesLoaded_();
    if (!ready) {
      // Fallback: open the search engines settings page
      window.open('chrome://settings/searchEngines', '_blank', 'noopener');
      return;
    }

    // Ensure the dialog element is available in optimized builds.
    if (!customElements.get('settings-search-engine-list-dialog')) {
      try {
        await import('chrome://settings/settings.js');
      } catch {
        window.open('chrome://settings/searchEngines', '_blank', 'noopener');
        return;
      }
    }
    if (!customElements.get('settings-search-engine-list-dialog')) {
      window.open('chrome://settings/searchEngines', '_blank', 'noopener');
      return;
    }

    // Create the Polymer dialog element imperatively
    const dialog = document.createElement('settings-search-engine-list-dialog');
    (dialog as any).searchEngines = this.searchEngines_;

    // Listen for dialog events
    dialog.addEventListener('close', () => {
      this.onSearchEngineListDialogClose_();
    });
    dialog.addEventListener('search-engine-changed', (e: Event) => {
      this.onDefaultSearchEngineChangedInDialog_(
          e as CustomEvent<{searchEngine: SearchEngine}>);
    });

    // Append to shadow root
    this.shadowRoot?.appendChild(dialog);
    this.searchEngineDialogElement_ = dialog;

    chrome.metricsPrivate?.recordUserAction('ChooseDefaultSearchEngine');
  }

  private onSearchEngineListDialogClose_() {
    if (this.searchEngineDialogElement_) {
      this.searchEngineDialogElement_.remove();
      this.searchEngineDialogElement_ = null;
    }
    // Refresh the search engines list in case it changed (legacy API only).
    if (this.useLegacySearchEnginesList_()) {
      this.browserProxy_.getSearchEnginesList().then(
          (info: SearchEnginesInfo) => {
            this.updateSearchEngines_(info);
          });
    }
  }

  private onDefaultSearchEngineChangedInDialog_(
      event: CustomEvent<{searchEngine: SearchEngine}>) {
    if (event.detail?.searchEngine) {
      this.defaultSearchEngine_ = event.detail.searchEngine;
    }
  }

  private async onImportDataClick_() {
    if (this.importDialog_) {
      return;
    }
    try {
      await import('chrome://settings/lazy_load.js');
    } catch {
      return;
    }
    if (!customElements.get('settings-import-data-dialog')) {
      return;
    }
    const dialog = document.createElement('settings-import-data-dialog') as
        HTMLElement & {prefs?: {[key: string]: any}};
    const prefs = this.getSettingsPrefs_();
    if (prefs) {
      dialog.prefs = prefs;
    }
    dialog.addEventListener('close', () => {
      dialog.remove();
      this.importDialog_ = null;
    }, {once: true});

    const root = this.getRootNode();
    if (root instanceof ShadowRoot) {
      root.appendChild(dialog);
    } else {
      document.body.appendChild(dialog);
    }
    this.importDialog_ = dialog;
  }

  private onSkipImportClick_() {
    // Skip import - just scroll to next section for visual feedback
    const layoutSection = this.shadowRoot?.getElementById('layoutTitle');
    layoutSection?.scrollIntoView({behavior: 'smooth', block: 'start'});
  }

  private getSettingsPrefs_(): {[key: string]: any}|null {
    type SettingsUiElement = HTMLElement & {
      prefs?: {[key: string]: any};
      $?: {prefs?: {prefs?: {[key: string]: any}}};
    };
    const root = this.getRootNode();
    const host = root instanceof ShadowRoot ? root.host : null;
    const settingsUi = ((host && host.tagName === 'SETTINGS-UI') ? host :
        document.querySelector('settings-ui')) as SettingsUiElement | null;
    if (!settingsUi) {
      return null;
    }
    return settingsUi.prefs ?? settingsUi.$?.prefs?.prefs ?? null;
  }

  private onManageExtensionsClick_() {
    window.open('chrome://extensions', '_blank', 'noopener');
  }

  private onOpenNewTabClick_() {
    window.open('chrome://new-tab-page/', '_blank', 'noopener');
  }

  private onGoToSettingsClick_() {
    window.location.href = 'chrome://settings/';
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'get-started-page': GetStartedPage;
  }
}

customElements.define(GetStartedPage.is, GetStartedPage);
