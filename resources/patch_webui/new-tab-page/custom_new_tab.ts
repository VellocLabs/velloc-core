import {loadTimeData} from '//resources/js/load_time_data.js';

interface LogoElement extends HTMLElement {
  shadowRoot: ShadowRoot|null;
  requestUpdate?: () => void;
}

interface LogoElementPrototype extends LogoElement {
  computeShowDoodle_?: () => boolean;
  connectedCallback?: () => void;
  updated?: (changedProperties: unknown) => void;
  __customBrowserSearchLogoPatched?: boolean;
}

type LogoElementConstructor = {
  prototype: LogoElementPrototype;
};

const ENGINE_ICON_FILES: Record<string, string> = {
  ask: 'ask_logo.svg',
  baidu: 'baidu_logo.svg',
  bing: 'bing_logo.svg',
  duckduckgo: 'duckduckgo_logo.svg',
  ecosia: 'ecosia_logo.svg',
  yahoo: 'yahoo_logo.svg',
  yandex: 'yandex_logo.svg',
};

const NAME_TOKENS: Array<[string, string]> = [
  ['google', 'google'],
  ['duckduckgo', 'duckduckgo'],
  ['ddg', 'duckduckgo'],
  ['bing', 'bing'],
  ['baidu', 'baidu'],
  ['yahoo', 'yahoo'],
  ['yandex', 'yandex'],
  ['ecosia', 'ecosia'],
  ['ask', 'ask'],
];

const ENGINE_DISPLAY_NAMES: Record<string, string> = {
  ask: 'Ask',
  baidu: 'Baidu',
  bing: 'Bing',
  duckduckgo: 'DuckDuckGo',
  ecosia: 'Ecosia',
  google: 'Google',
  yahoo: 'Yahoo',
  yandex: 'Yandex',
};

const SEARCH_BRAND_TOKENS = [
  'Google',
  'Bing',
  'DuckDuckGo',
  'Baidu',
  'Yahoo',
  'Yandex',
  'Ecosia',
  'Ask',
];

const CUSTOM_ENGINE_NAME_KEY = 'customSearchEngineName';
const CUSTOM_ENGINE_URL_KEY = 'customSearchEngineUrl';
const SHOW_SEARCH_KEY = 'customNtpShowSearch';
const SHOW_LOGO_KEY = 'customNtpShowLogo';
const SEARCHBOX_PLACEHOLDER_KEY = 'searchBoxPlaceholder';
const SETUP_LIST_BODY_KEY = 'customNtpSetupListGetStartedBody';
const SETUP_LIST_ACTION_KEY = 'customNtpSetupListGetStartedAction';
const SETUP_LIST_PROMO_ID = 'customBrowserSetupListGetStarted';
const SETUP_LIST_ITEM_ID = 'customBrowserSetupListGetStartedItem';
const SETUP_LIST_WRAPPER_ID = 'customBrowserSetupListWrapper';
const SETUP_LIST_ICON_NAME = 'palette';
const SETUP_LIST_GET_STARTED_URL = 'chrome://settings/getStarted';
const PRODUCT_LOGO_URL = 'chrome://theme/IDR_PRODUCT_LOGO';

type SearchEngineInfo = {
  name: string;
  url: string;
};

type NtpAppElement = HTMLElement & {
  shadowRoot: ShadowRoot|null;
  oneGoogleBarEnabled_?: boolean;
  requestUpdate?: () => void;
};

type AppState = {
  shouldShowSearch: boolean;
  shouldShowLogo: boolean;
};
const APP_STYLE_ID = 'customNtpAdditionsStyle';

type SetupListElement = HTMLElement & {
  shadowRoot: ShadowRoot|null;
  onSetPromos?: (eligible: unknown, completed: unknown) => void;
  updated?: (changedProperties: unknown) => void;
  __customBrowserSetupListPatched?: boolean;
};

type SetupListElementConstructor = {
  prototype: SetupListElement;
};

type PlaceholderTextCycler = {
  placeholderTexts_?: string[];
  start?: () => void;
  stop?: () => void;
};

type SearchboxElement = HTMLElement & {
  shadowRoot: ShadowRoot|null;
  placeholderText?: string;
  cyclingPlaceholders?: boolean;
  requestUpdate?: () => void;
  placeholderCycler_?: PlaceholderTextCycler|null;
  connectedCallback?: () => void|Promise<void>;
  updated?: (changedProperties: unknown) => void;
  __customBrowserSearchboxPatched?: boolean;
};

type SearchboxElementConstructor = {
  prototype: SearchboxElement;
};

let activeIconUrl: string|null = null;
const appStateMap = new WeakMap<NtpAppElement, AppState>();
let setupListRefreshScheduled = false;

type PrefState = {
  showSearch: boolean;
  showLogo: boolean;
};

const prefState: PrefState = {
  showSearch: loadTimeData.valueExists(SHOW_SEARCH_KEY) ?
      loadTimeData.getBoolean(SHOW_SEARCH_KEY) :
      true,
  showLogo: loadTimeData.valueExists(SHOW_LOGO_KEY) ?
      loadTimeData.getBoolean(SHOW_LOGO_KEY) :
      true,
};

function getString(key: string): string {
  return loadTimeData.valueExists(key) ? loadTimeData.getString(key) : '';
}

function normalizeName(name: string): string {
  return name.toLowerCase().replace(/[^a-z0-9]/g, '');
}

function detectEngineKeyFromName(name: string): string|null {
  const normalized = normalizeName(name);
  if (!normalized) {
    return null;
  }

  for (const [token, key] of NAME_TOKENS) {
    if (normalized.includes(token)) {
      return key;
    }
  }

  return null;
}


function getSearchEngineInfo(): SearchEngineInfo {
  let name = '';
  let url = '';

  if (loadTimeData.valueExists(CUSTOM_ENGINE_NAME_KEY)) {
    name = loadTimeData.getString(CUSTOM_ENGINE_NAME_KEY);
  }
  if (loadTimeData.valueExists(CUSTOM_ENGINE_URL_KEY)) {
    url = loadTimeData.getString(CUSTOM_ENGINE_URL_KEY);
  }
  return {name, url};
}

function getSearchEngineKey(): string|null {
  const info = getSearchEngineInfo();
  return detectEngineKeyFromName(info.name);
}

function hasCustomSearchEngineInfo(): boolean {
  return loadTimeData.valueExists(CUSTOM_ENGINE_NAME_KEY) ||
      loadTimeData.valueExists(CUSTOM_ENGINE_URL_KEY);
}

function detectEngineKeyFromUrl(url: string): string|null {
  if (!url) {
    return null;
  }

  try {
    const parsed = new URL(url);
    return detectEngineKeyFromName(parsed.hostname);
  } catch (error) {
    return detectEngineKeyFromName(url);
  }
}

function getSearchEngineDisplayName(): string|null {
  const info = getSearchEngineInfo();
  const name = info.name.trim();
  const keyFromName = detectEngineKeyFromName(name);
  const keyFromUrl = detectEngineKeyFromUrl(info.url);
  const engineKey = keyFromName || keyFromUrl;

  if (engineKey === 'google') {
    return null;
  }

  if (name) {
    return name;
  }

  if (engineKey && ENGINE_DISPLAY_NAMES[engineKey]) {
    return ENGINE_DISPLAY_NAMES[engineKey];
  }

  if (engineKey) {
    return engineKey.charAt(0).toUpperCase() + engineKey.slice(1);
  }

  return null;
}

function replaceSearchBrand(text: string, brandName: string): string {
  if (!text || !brandName) {
    return text;
  }

  if (text.toLowerCase().includes(brandName.toLowerCase())) {
    return text;
  }

  for (const token of SEARCH_BRAND_TOKENS) {
    const regex = new RegExp(token, 'gi');
    if (regex.test(text)) {
      return text.replace(regex, brandName);
    }
  }

  return text;
}

function buildIconUrl(fileName: string): string {
  return `${window.location.protocol}//${window.location.host}/${fileName}`;
}

function getCustomSearchLogoUrl(engineKey: string|null): string|null {
  if (!engineKey || engineKey === 'google') {
    return null;
  }
  const iconFile = ENGINE_ICON_FILES[engineKey];
  return iconFile ? buildIconUrl(iconFile) : null;
}

function getEffectiveLogoUrl(showSearch: boolean): string|null {
  if (!showSearch) {
    return PRODUCT_LOGO_URL;
  }

  if (!hasCustomSearchEngineInfo()) {
    return null;
  }

  const engineKey = getSearchEngineKey();
  if (engineKey === 'google') {
    return null;
  }

  const customLogoUrl = getCustomSearchLogoUrl(engineKey);
  if (customLogoUrl) {
    return customLogoUrl;
  }
  return PRODUCT_LOGO_URL;
}

function resetLogo(element: LogoElement) {
  const shadow = element.shadowRoot;
  if (!shadow) {
    return;
  }

  const logo = shadow.getElementById('logo');
  if (!logo) {
    return;
  }

  if (!logo.dataset['customSearchLogo']) {
    return;
  }

  delete logo.dataset['customSearchLogo'];
  logo.style.removeProperty('background-image');
  logo.style.removeProperty('background-repeat');
  logo.style.removeProperty('background-position');
  logo.style.removeProperty('background-size');
  logo.style.removeProperty('background-color');
  logo.style.removeProperty('webkit-mask-image');
  logo.style.removeProperty('mask-image');
}

function applyCustomLogo(element: LogoElement, iconUrl: string) {
  const shadow = element.shadowRoot;
  if (!shadow) {
    return;
  }

  const logo = shadow.getElementById('logo');
  if (!logo) {
    return;
  }

  if (logo.dataset['customSearchLogo'] === iconUrl) {
    return;
  }

  logo.dataset['customSearchLogo'] = iconUrl;
  logo.style.backgroundImage = `url("${iconUrl}")`;
  logo.style.backgroundRepeat = 'no-repeat';
  logo.style.backgroundPosition = 'center';
  logo.style.backgroundSize = 'contain';
  logo.style.backgroundColor = 'transparent';
  logo.style.webkitMaskImage = 'none';
  logo.style.maskImage = 'none';
}

function applyLogo(element: LogoElement) {
  if (!activeIconUrl) {
    resetLogo(element);
    return;
  }

  applyCustomLogo(element, activeIconUrl);
}

function applyLogoToAll() {
  const logos = collectElementsDeep(document, 'ntp-logo');
  for (const element of logos) {
    if (!(element instanceof HTMLElement)) {
      continue;
    }
    const logoElement = element as LogoElement;
    applyLogo(logoElement);
    logoElement.requestUpdate?.();
  }
}

function installLogoPatch() {
  customElements.whenDefined('ntp-logo').then(() => {
    const ctor = customElements.get('ntp-logo') as
        LogoElementConstructor|undefined;
    if (!ctor) {
      return;
    }

    const proto = ctor.prototype;
    if (proto.__customBrowserSearchLogoPatched) {
      return;
    }
    proto.__customBrowserSearchLogoPatched = true;

    const originalComputeShowDoodle = proto.computeShowDoodle_;
    proto.computeShowDoodle_ = function() {
      if (activeIconUrl) {
        return false;
      }
      return originalComputeShowDoodle ?
          originalComputeShowDoodle.call(this) :
          true;
    };

    const originalConnected = proto.connectedCallback;
    proto.connectedCallback = function() {
      originalConnected?.call(this);
      applyLogo(this as LogoElement);
    };

    const originalUpdated = proto.updated;
    proto.updated = function(changedProperties: unknown) {
      originalUpdated?.call(this, changedProperties);
      applyLogo(this as LogoElement);
    };

    applyLogoToAll();
  });
}

function setSearchLogo(iconUrl: string|null) {
  activeIconUrl = iconUrl;
  installLogoPatch();

  applyLogoToAll();
}

const engineKey = getSearchEngineKey();
setSearchLogo(prefState.showLogo ? getEffectiveLogoUrl(prefState.showSearch) :
    null);

function shouldShowGoogleLinks(engineKey: string|null,
                               hasCustomEngineData: boolean): boolean {
  if (engineKey === 'google') {
    return true;
  }
  return !hasCustomEngineData;
}

function getAppState(app: NtpAppElement): AppState {
  let state = appStateMap.get(app);
  if (!state) {
    state = {
      shouldShowSearch: prefState.showSearch,
      shouldShowLogo: prefState.showLogo,
    };
    appStateMap.set(app, state);
  }
  state.shouldShowSearch = prefState.showSearch;
  state.shouldShowLogo = prefState.showLogo;
  return state;
}

function updatePrefState(showSearch: boolean, showLogo: boolean): boolean {
  let changed = false;
  const nextShowSearch = !!showSearch;
  const nextShowLogo = !!showLogo;

  if (prefState.showSearch !== nextShowSearch) {
    prefState.showSearch = nextShowSearch;
    changed = true;
  }
  if (prefState.showLogo !== nextShowLogo) {
    prefState.showLogo = nextShowLogo;
    changed = true;
  }

  return changed;
}

function applyPrefStateToAllApps(showGoogleLinks: boolean) {
  for (const element of document.querySelectorAll('ntp-app')) {
    applyCustomNtpChanges(element as NtpAppElement, showGoogleLinks);
  }
}

function applyPrefUpdate(showGoogleLinks: boolean, showSearch: boolean,
                         showLogo: boolean) {
  if (updatePrefState(showSearch, showLogo)) {
    applyPrefStateToAllApps(showGoogleLinks);
  }
}

function attachAppStyles(root: ShadowRoot) {
  if (root.querySelector(`#${APP_STYLE_ID}`)) {
    return;
  }

  const style = document.createElement('style');
  style.id = APP_STYLE_ID;
  style.textContent = `
:host(.custom-hide-search) #searchboxContainer,
:host(.custom-hide-search) ntp-action-chips {
  visibility: hidden;
  pointer-events: none;
}

:host(.custom-hide-search) #scrim {
  display: none;
}
`;
  root.appendChild(style);
}

function applyGoogleBarVisibility(app: NtpAppElement,
                                  showGoogleLinks: boolean,
                                  showSearch: boolean) {
  const shadow = app.shadowRoot;
  if (!shadow) {
    return;
  }

  const shouldShowGoogleLinks = showGoogleLinks && showSearch;
  if (shouldShowGoogleLinks) {
    shadow.querySelectorAll<HTMLElement>(
               '#oneGoogleBar, #oneGoogleBarScrim, #oneGoogleBarStackingContext')
        .forEach(el => {
          el.style.display = '';
        });
    return;
  }

  if (!showGoogleLinks && app.oneGoogleBarEnabled_) {
    app.oneGoogleBarEnabled_ = false;
    app.requestUpdate?.();
  }

  shadow.querySelectorAll<HTMLElement>(
             '#oneGoogleBar, #oneGoogleBarScrim, #oneGoogleBarStackingContext')
      .forEach(el => {
        el.style.display = 'none';
      });
}

function applySearchVisibility(app: NtpAppElement,
                               root: ShadowRoot,
                               state: AppState) {
  app.classList.toggle('custom-hide-search', !state.shouldShowSearch);

  setSearchLogo(state.shouldShowLogo ?
      getEffectiveLogoUrl(state.shouldShowSearch) :
      null);

  const searchContainer =
      root.querySelector<HTMLElement>('#searchboxContainer');
  if (searchContainer) {
    searchContainer.style.display = state.shouldShowSearch ? '' : 'none';
    searchContainer.toggleAttribute('inert', !state.shouldShowSearch);
    searchContainer.setAttribute(
        'aria-hidden', state.shouldShowSearch ? 'false' : 'true');
  }

  const actionChips = root.querySelector<HTMLElement>('ntp-action-chips');
  if (actionChips) {
    actionChips.style.display = state.shouldShowSearch ? '' : 'none';
    actionChips.toggleAttribute('inert', !state.shouldShowSearch);
    actionChips.setAttribute(
        'aria-hidden', state.shouldShowSearch ? 'false' : 'true');
  }

  if (!state.shouldShowSearch) {
    const composeDialog =
        root.querySelector<HTMLDialogElement>('#composeboxDialog');
    composeDialog?.close?.();
  }

  const logo = root.querySelector<HTMLElement>('#logo');
  if (logo) {
    const showLogo = state.shouldShowLogo;
    logo.style.display = showLogo ? '' : 'none';
    logo.toggleAttribute('hidden', !showLogo);
    logo.setAttribute('aria-hidden', showLogo ? 'false' : 'true');
  }
}

function applyCustomNtpChanges(app: NtpAppElement,
                               showGoogleLinks: boolean) {
  const shadow = app.shadowRoot;
  if (!shadow) {
    return;
  }

  const state = getAppState(app);
  attachAppStyles(shadow);
  applyGoogleBarVisibility(app, showGoogleLinks, state.shouldShowSearch);
  applySearchVisibility(app, shadow, state);
  ensureSetupListModuleWrapper(shadow);
  scheduleSetupListRefresh();
}

function getSearchboxPlaceholderBase(searchbox: SearchboxElement): string {
  if (searchbox.placeholderText) {
    return searchbox.placeholderText;
  }

  const attrPlaceholder = searchbox.getAttribute('placeholder-text');
  if (attrPlaceholder) {
    return attrPlaceholder;
  }

  const input =
      searchbox.shadowRoot?.querySelector<HTMLInputElement>('#input');
  if (input?.placeholder) {
    return input.placeholder;
  }

  return getString(SEARCHBOX_PLACEHOLDER_KEY);
}

function updateSearchboxPlaceholder(
    searchbox: SearchboxElement, brandName: string) {
  const base = getSearchboxPlaceholderBase(searchbox);
  if (!base) {
    return;
  }

  const updated = replaceSearchBrand(base, brandName);
  if (!updated || updated === base) {
    return;
  }

  searchbox.placeholderText = updated;
  searchbox.setAttribute('placeholder-text', updated);
  searchbox.requestUpdate?.();

  const input =
      searchbox.shadowRoot?.querySelector<HTMLInputElement>('#input');
  if (input) {
    input.placeholder = updated;
  }
}

function updateSearchboxPlaceholderCycler(
    searchbox: SearchboxElement, brandName: string) {
  const cycler = searchbox.placeholderCycler_;
  const placeholderTexts = cycler?.placeholderTexts_;
  if (!cycler || !placeholderTexts || placeholderTexts.length === 0) {
    return;
  }

  const updatedTexts =
      placeholderTexts.map(text => replaceSearchBrand(text, brandName));

  let changed = updatedTexts.length !== placeholderTexts.length;
  if (!changed) {
    for (let i = 0; i < updatedTexts.length; i += 1) {
      if (updatedTexts[i] !== placeholderTexts[i]) {
        changed = true;
        break;
      }
    }
  }

  if (!changed) {
    return;
  }

  cycler.placeholderTexts_ = updatedTexts;
  cycler.stop?.();
  cycler.start?.();
}

function applySearchboxBranding(searchbox: SearchboxElement) {
  const brandName = getSearchEngineDisplayName();
  if (!brandName) {
    return;
  }

  updateSearchboxPlaceholder(searchbox, brandName);
  updateSearchboxPlaceholderCycler(searchbox, brandName);
}

function applySearchboxBrandingToAll() {
  const elements = collectElementsDeep(document, 'cr-searchbox');
  for (const element of elements) {
    if (!(element instanceof HTMLElement)) {
      continue;
    }
    applySearchboxBranding(element);
  }
}

function installSearchboxPatch() {
  customElements.whenDefined('cr-searchbox').then(() => {
    const ctor = customElements.get('cr-searchbox') as
        SearchboxElementConstructor|undefined;
    if (!ctor) {
      return;
    }

    const proto = ctor.prototype;
    if (proto.__customBrowserSearchboxPatched) {
      return;
    }
    proto.__customBrowserSearchboxPatched = true;

    const originalConnected = proto.connectedCallback;
    proto.connectedCallback = async function() {
      await originalConnected?.call(this);
      applySearchboxBranding(this);
    };

    const originalUpdated = proto.updated;
    proto.updated = function(changedProperties: unknown) {
      originalUpdated?.call(this, changedProperties);
      applySearchboxBranding(this);
    };

    applySearchboxBrandingToAll();
  });
}

function collectElementsDeep(root: ParentNode, selector: string): Element[] {
  const results = new Set<Element>();
  const visit = (node: ParentNode) => {
    if (node instanceof Element && node.matches(selector)) {
      results.add(node);
    }
    const children = (node instanceof Document ||
                      node instanceof ShadowRoot ||
                      node instanceof Element) ?
        Array.from(node.children) :
        [];
    for (const child of children) {
      visit(child);
      if (child instanceof HTMLElement && child.shadowRoot) {
        visit(child.shadowRoot);
      }
    }
  };
  visit(root);
  return Array.from(results);
}

function ensureSetupListModuleWrapper(root: ShadowRoot): HTMLElement|null {
  const customWrapper =
      root.querySelector<HTMLElement>(`#${SETUP_LIST_WRAPPER_ID}`);
  const wrappers = Array.from(
      root.querySelectorAll<HTMLElement>('setup-list-module-wrapper'));
  const nativeWrapper =
      wrappers.find(wrapper =>
          wrapper !== customWrapper && wrapper.id !== SETUP_LIST_WRAPPER_ID);
  if (nativeWrapper) {
    // Prefer the native wrapper when it exists to avoid duplicate modules.
    if (customWrapper) {
      customWrapper.remove();
    }
    return nativeWrapper;
  }

  if (customWrapper) {
    return customWrapper;
  }

  const content = root.querySelector<HTMLElement>('#content');
  if (!content) {
    return null;
  }

  const wrapper = document.createElement('setup-list-module-wrapper');
  wrapper.id = SETUP_LIST_WRAPPER_ID;
  wrapper.setAttribute('data-custom-browser', 'true');
  wrapper.setAttribute('maxPromos', '3');
  wrapper.setAttribute('maxCompletedPromos', '1');

  const insertBefore = content.querySelector<HTMLElement>('#contentBottomSpacer') ||
      content.querySelector<HTMLElement>('#modules') ||
      null;
  if (insertBefore) {
    content.insertBefore(wrapper, insertBefore);
  } else {
    content.appendChild(wrapper);
  }
  return wrapper;
}

function scheduleSetupListRefresh() {
  if (setupListRefreshScheduled) {
    return;
  }
  setupListRefreshScheduled = true;
  setTimeout(() => {
    setupListRefreshScheduled = false;
    const setupLists = collectElementsDeep(document, 'setup-list');
    for (const element of setupLists) {
      if (element instanceof HTMLElement) {
        ensureSetupListGetStartedItem(element);
      }
    }
  }, 0);
}

function ensureSetupListGetStartedItem(setupList: HTMLElement) {
  const shadow = setupList.shadowRoot;
  if (!shadow) {
    return;
  }

  const promos = shadow.querySelector<HTMLElement>('#promos');
  if (!promos) {
    return;
  }

  let item = shadow.getElementById(SETUP_LIST_ITEM_ID);
  if (!item) {
    item = document.createElement('setup-list-item') as unknown as HTMLElement;
    item.id = SETUP_LIST_ITEM_ID;
    item.setAttribute('body-icon-name', SETUP_LIST_ICON_NAME);
    item.setAttribute(
        'body-text',
        getString(SETUP_LIST_BODY_KEY));
    item.setAttribute(
        'action-button-text',
        getString(SETUP_LIST_ACTION_KEY));
    item.setAttribute('promo-id', SETUP_LIST_PROMO_ID);
    item.addEventListener('ntp-promo-click', event => {
      const promoEvent = event as CustomEvent<unknown>;
      if (promoEvent.detail !== SETUP_LIST_PROMO_ID) {
        return;
      }
      promoEvent.stopImmediatePropagation();
      promoEvent.stopPropagation();
      window.location.href = SETUP_LIST_GET_STARTED_URL;
    });
    promos.prepend(item);
  }

  setupList.dispatchEvent(new CustomEvent('module-ready', {
    bubbles: true,
    composed: true,
    detail: true,
  }));
}

function installSetupListPatch() {
  customElements.whenDefined('setup-list').then(() => {
    const ctor = customElements.get('setup-list') as
        SetupListElementConstructor|undefined;
    if (!ctor) {
      return;
    }

    const proto = ctor.prototype;
    if (proto.__customBrowserSetupListPatched) {
      return;
    }
    proto.__customBrowserSetupListPatched = true;

    const originalUpdated = proto.updated;
    proto.updated = function(changedProperties: unknown) {
      originalUpdated?.call(this, changedProperties);
      ensureSetupListGetStartedItem(this);
    };

    const originalOnSetPromos = proto.onSetPromos;
    proto.onSetPromos = function(eligible: unknown, completed: unknown) {
      originalOnSetPromos?.call(this, eligible, completed);
      ensureSetupListGetStartedItem(this);
      this.dispatchEvent(new CustomEvent('module-ready', {
        bubbles: true,
        composed: true,
        detail: true,
      }));
    };

    const setupLists = collectElementsDeep(document, 'setup-list');
    for (const element of setupLists) {
      if (element instanceof HTMLElement) {
        ensureSetupListGetStartedItem(element);
      }
    }
  });
}

function installNtpAppPatch(showGoogleLinks: boolean) {
  customElements.whenDefined('ntp-app').then(() => {
    const ctor = customElements.get('ntp-app') as
        |{prototype: {updated?: (props: unknown) => void; __customNtpPatched?: boolean}}
        |undefined;
    if (!ctor) {
      return;
    }

    const proto = ctor.prototype;
    if (proto.__customNtpPatched) {
      return;
    }
    proto.__customNtpPatched = true;

    const originalUpdated = proto.updated;
    proto.updated = function(changedProperties: unknown) {
      originalUpdated?.call(this, changedProperties);
      applyCustomNtpChanges(this as NtpAppElement, showGoogleLinks);
    };

    for (const element of document.querySelectorAll('ntp-app')) {
      applyCustomNtpChanges(element as NtpAppElement, showGoogleLinks);
    }
  });
}

const showGoogleLinks =
    shouldShowGoogleLinks(engineKey, hasCustomSearchEngineInfo());

declare global {
  interface Window {
    customBrowserUpdateNtpPrefs?: (showSearch: boolean,
                                   showLogo?: boolean) => void;
  }
}

window.customBrowserUpdateNtpPrefs =
    (showSearch: boolean, showLogo?: boolean) => {
      const resolvedShowLogo =
          typeof showLogo === 'boolean' ? showLogo : prefState.showLogo;
      applyPrefUpdate(showGoogleLinks, showSearch, resolvedShowLogo);
    };

installSetupListPatch();
installNtpAppPatch(showGoogleLinks);
installSearchboxPatch();
applyPrefStateToAllApps(showGoogleLinks);
