import {loadTimeData} from 'chrome://resources/js/load_time_data.js';
import './get_started_page.js';

const GET_STARTED_PATH = '/getStarted';
const MENU_ITEM_ID = 'customBrowserGetStartedMenuItem';
const VIEW_ID = 'getStarted';
const MENU_LABEL_KEY = 'customBrowserGetStartedMenuLabel';
const GET_STARTED_PAGE_ID = 'customBrowserGetStartedPage';
const HIDDEN_DATA_ATTR = 'customBrowserPrevDisplay';
const PEOPLE_LINK_ID = 'customBrowserGetStartedPeopleLink';
const LOGO_ALT_KEY = 'customBrowserGetStartedLogoAlt';

function getString(key: string): string {
  try {
    return loadTimeData.valueExists(key) ? loadTimeData.getString(key) : '';
  } catch {
    return '';
  }
}

const initialPath =
    (window as Window & {customBrowserSettingsInitialPath?: string})
        .customBrowserSettingsInitialPath ||
    window.location.pathname;

type RouterInstance = {
  getCurrentRoute?: () => {path: string};
  getQueryParameters?: () => URLSearchParams;
  getRoutes?: () => any;
  navigateTo?: (route: any, params?: URLSearchParams, removeSearch?: boolean) =>
      void;
  addObserver?: (observer: {currentRouteChanged: (route: any) => void}) =>
      void;
  removeObserver?: (observer: {currentRouteChanged: (route: any) => void}) =>
      void;
};

let initialized = false;
let scanObserver: MutationObserver|null = null;
let scanScheduled = false;
let routerPromise: Promise<RouterInstance|null>|null = null;
let routerInstance: RouterInstance|null = null;
let getStartedRoute: any = null;
let redirecting = false;
let routeObserver: {currentRouteChanged: (route: any) => void}|null = null;
const observedShadows = new WeakSet<ShadowRoot>();
let pendingInitialPath: string|null|undefined;
let getStartedPage: HTMLElement|null = null;

function getPendingInitialPath(): string|null {
  if (pendingInitialPath !== undefined) {
    return pendingInitialPath;
  }
  pendingInitialPath = null;
  const entry = performance.getEntriesByType('navigation')[0] as
      PerformanceNavigationTiming|undefined;
  const fallbackPath = initialPath || null;
  if (!entry?.name) {
    if (fallbackPath === GET_STARTED_PATH) {
      pendingInitialPath = GET_STARTED_PATH;
    }
    return pendingInitialPath;
  }
  try {
    const url = new URL(entry.name);
    const path = url.pathname || fallbackPath;
    pendingInitialPath = path === GET_STARTED_PATH ? GET_STARTED_PATH : null;
  } catch {
    pendingInitialPath = fallbackPath === GET_STARTED_PATH ? GET_STARTED_PATH :
                                                          null;
  }
  return pendingInitialPath;
}

function clearPendingInitialPath() {
  pendingInitialPath = null;
}

function shouldShowGetStartedImmediately(): boolean {
  if (window.location.pathname === GET_STARTED_PATH) {
    return true;
  }
  return getPendingInitialPath() === GET_STARTED_PATH;
}

async function loadRouter(): Promise<RouterInstance|null> {
  if (routerInstance) {
    return routerInstance;
  }
  if (!routerPromise) {
    routerPromise = import('chrome://settings/settings.js')
                        .then(module => {
                          const instance =
                              module?.Router?.getInstance?.() ?? null;
                          routerInstance = instance;
                          return instance as RouterInstance|null;
                        })
                        .catch(() => null);
  }
  return routerPromise;
}

async function ensureGetStartedRoute(): Promise<any|null> {
  if (getStartedRoute) {
    return getStartedRoute;
  }
  const router = await loadRouter();
  if (!router?.getRoutes) {
    return null;
  }
  const routes = router.getRoutes();
  if (routes?.GET_STARTED) {
    getStartedRoute = routes.GET_STARTED;
    return getStartedRoute;
  }
  if (!routes?.BASIC?.createSection) {
    return null;
  }
  const route = routes.BASIC.createSection(
      GET_STARTED_PATH, VIEW_ID, getString(MENU_LABEL_KEY));
  routes.GET_STARTED = route;
  getStartedRoute = route;
  return getStartedRoute;
}

function observeShadowRoot(root: ShadowRoot) {
  if (observedShadows.has(root)) {
    return;
  }
  observedShadows.add(root);
  const observer = new MutationObserver(() => scheduleScan());
  observer.observe(root, {childList: true, subtree: true});
}

function collectElementsDeep(
    root: ParentNode, selector: string, out: Set<Element>) {
  if (!('querySelectorAll' in root)) {
    return;
  }
  for (const element of root.querySelectorAll(selector)) {
    out.add(element);
  }
  for (const element of root.querySelectorAll('*')) {
    const shadow = (element as HTMLElement).shadowRoot;
    if (!shadow) {
      continue;
    }
    observeShadowRoot(shadow);
    collectElementsDeep(shadow, selector, out);
  }
}

function ensureMenuPatch(menu: HTMLElement) {
  const shadow = menu.shadowRoot;
  if (!shadow) {
    return;
  }
  shadow.querySelectorAll<HTMLElement>(`#${MENU_ITEM_ID}`)
      .forEach(item => item.remove());
}

function createGetStartedView(): HTMLElement {
  const view = document.createElement('div');
  view.id = VIEW_ID;
  view.setAttribute('slot', 'view');
  return view;
}

function ensureGetStartedView(main: HTMLElement): boolean {
  const shadow = main.shadowRoot;
  if (!shadow) {
    return false;
  }
  const switcher = shadow.getElementById('switcher');
  if (!switcher) {
    return false;
  }
  if (!switcher.querySelector(`#${VIEW_ID}`)) {
    switcher.appendChild(createGetStartedView());
  }
  return true;
}

function ensureGetStartedViewReady(): boolean {
  const mains = new Set<Element>();
  collectElementsDeep(document, 'settings-main', mains);
  let ready = false;
  for (const main of mains) {
    if (main instanceof HTMLElement && ensureGetStartedView(main)) {
      ready = true;
    }
  }
  return ready;
}

function ensureGetStartedPage(): HTMLElement|null {
  if (!getStartedPage) {
    getStartedPage = document.createElement('get-started-page');
    getStartedPage.id = GET_STARTED_PAGE_ID;
    getStartedPage.setAttribute('aria-hidden', 'true');
  }

  const settingsUi = document.querySelector('settings-ui');
  const shadow = settingsUi?.shadowRoot;
  if (shadow) {
    if (getStartedPage.parentNode !== shadow) {
      shadow.appendChild(getStartedPage);
    }
    return getStartedPage;
  }

  const host = document.body || document.documentElement;
  if (!host) {
    return null;
  }
  if (getStartedPage.parentNode !== host) {
    host.appendChild(getStartedPage);
  }
  return getStartedPage;
}

function setElementHidden(element: HTMLElement|null, hidden: boolean) {
  if (!element) {
    return;
  }
  if (hidden) {
    if (!(HIDDEN_DATA_ATTR in element.dataset)) {
      element.dataset[HIDDEN_DATA_ATTR] = element.style.display;
    }
    // Move focus away before hiding to avoid aria-hidden warning
    if (element.contains(document.activeElement) && document.activeElement) {
      // Move focus to the body or another element outside this container
      const focusTarget = document.body;
      if (focusTarget && typeof focusTarget.focus === 'function') {
        focusTarget.focus();
      } else {
        (document.activeElement as HTMLElement).blur();
      }
    }
    // Set inert first to prevent focus from returning
    element.toggleAttribute('inert', true);
    element.style.display = 'none';
    element.setAttribute('aria-hidden', 'true');
    return;
  }
  // Remove inert before making visible
  element.toggleAttribute('inert', false);
  if (HIDDEN_DATA_ATTR in element.dataset) {
    element.style.display = element.dataset[HIDDEN_DATA_ATTR] || '';
    delete element.dataset[HIDDEN_DATA_ATTR];
  } else {
    element.style.display = '';
  }
  element.removeAttribute('aria-hidden');
}

function updateGetStartedVisibility(isActive: boolean) {
  const page = ensureGetStartedPage();
  if (page) {
    page.toggleAttribute('active', isActive);
    page.setAttribute('aria-hidden', isActive ? 'false' : 'true');
  }

  const settingsUi = document.querySelector('settings-ui');
  const shadow = settingsUi?.shadowRoot;
  if (!shadow) {
    return;
  }
  setElementHidden(shadow.getElementById('toolbar'), isActive);
  setElementHidden(shadow.getElementById('container'), isActive);
  setElementHidden(shadow.getElementById('drawer'), isActive);
}

async function navigateToGetStarted() {
  const router = await loadRouter();
  if (!router) {
    return;
  }
  const route = await ensureGetStartedRoute();
  if (!route) {
    return;
  }
  router.navigateTo?.(route, undefined, true);
}

function ensurePeoplePagePatch(page: HTMLElement) {
  const shadow = page.shadowRoot;
  if (!shadow) {
    return;
  }
  const googleServices = shadow.getElementById('google-services');
  if (googleServices) {
    googleServices.hidden = true;
    googleServices.setAttribute('aria-hidden', 'true');
  }

  const importRow = shadow.getElementById('importDataDialogTrigger');
  if (!importRow || shadow.getElementById(PEOPLE_LINK_ID)) {
    return;
  }

  const row = document.createElement('cr-link-row');
  row.id = PEOPLE_LINK_ID;
  (row as {usingSlottedLabel?: boolean}).usingSlottedLabel = true;
  const roleDescription = importRow.getAttribute('role-description');
  if (roleDescription) {
    row.setAttribute('role-description', roleDescription);
  }
  const label = document.createElement('span');
  label.setAttribute('slot', 'label');
  label.style.display = 'flex';
  label.style.alignItems = 'center';
  label.style.gap = '8px';
  const logo = document.createElement('img');
  logo.id = 'product-logo';
  logo.setAttribute(
      'srcset',
      'chrome://theme/current-channel-logo@1x 1x, ' +
          'chrome://theme/current-channel-logo@2x 2x');
  logo.setAttribute('alt', getString(LOGO_ALT_KEY));
  logo.setAttribute('role', 'presentation');
  label.append(logo, ' ', getString(MENU_LABEL_KEY));
  row.appendChild(label);
  row.addEventListener('click', () => {
    void navigateToGetStarted();
  });

  importRow.parentElement?.insertBefore(row, importRow.nextSibling);
}

function scanAndPatch() {
  const menus = new Set<Element>();
  collectElementsDeep(document, 'settings-menu', menus);
  for (const menu of menus) {
    if (menu instanceof HTMLElement) {
      ensureMenuPatch(menu);
    }
  }

  ensureGetStartedViewReady();
  ensureGetStartedPage();
  const peoplePages = new Set<Element>();
  collectElementsDeep(document, 'settings-people-page', peoplePages);
  for (const page of peoplePages) {
    if (page instanceof HTMLElement) {
      ensurePeoplePagePatch(page);
    }
  }
  void handleRouteChange();
}

function scheduleScan() {
  if (scanScheduled) {
    return;
  }
  scanScheduled = true;
  requestAnimationFrame(() => {
    scanScheduled = false;
    scanAndPatch();
  });
}

function startObserver() {
  if (scanObserver) {
    return;
  }
  const root = document.body || document.documentElement;
  if (!root) {
    requestAnimationFrame(startObserver);
    return;
  }
  scanObserver = new MutationObserver(() => scheduleScan());
  scanObserver.observe(root, {childList: true, subtree: true});
  scheduleScan();
}

async function handleRouteChange(route?: {path?: string}) {
  const router = await loadRouter();
  if (!router) {
    return;
  }
  const currentRoute = route || router.getCurrentRoute?.();
  if (!currentRoute) {
    return;
  }
  await ensureGetStartedRoute();
  const viewReady = ensureGetStartedViewReady();
  if (!viewReady) {
    scheduleScan();
  }
  const pendingPath = getPendingInitialPath();
  const currentPath = window.location.pathname;
  const shouldUsePendingPath =
      pendingPath === GET_STARTED_PATH && currentPath === '/' &&
      currentRoute.path === '/';
  const isGetStartedPath =
      currentPath === GET_STARTED_PATH || shouldUsePendingPath;
  if (!redirecting && getStartedRoute && viewReady &&
      isGetStartedPath && currentRoute.path !== GET_STARTED_PATH) {
    redirecting = true;
    router.navigateTo?.(getStartedRoute, undefined, true);
    redirecting = false;
    if (shouldUsePendingPath) {
      clearPendingInitialPath();
    }
    return;
  }
  if (!shouldUsePendingPath && currentPath !== '/') {
    clearPendingInitialPath();
  }
  updateGetStartedVisibility(
      isGetStartedPath || currentRoute.path === GET_STARTED_PATH);
}

async function ensureRouteObserver() {
  if (routeObserver) {
    return;
  }
  const router = await loadRouter();
  if (!router?.addObserver) {
    return;
  }
  routeObserver = {
    currentRouteChanged: (route: {path?: string}) => {
      void handleRouteChange(route);
    },
  };
  router.addObserver(routeObserver);
}

async function initialize() {
  if (shouldShowGetStartedImmediately()) {
    updateGetStartedVisibility(true);
  } else {
    ensureGetStartedPage();
  }
  await customElements.whenDefined('settings-ui');
  await ensureGetStartedRoute();
  startObserver();
  await ensureRouteObserver();
  await handleRouteChange();
}

export function attach() {
  if (initialized) {
    return;
  }
  initialized = true;
  void initialize();
}

export function detach() {
  if (!initialized) {
    return;
  }
  initialized = false;
  scanObserver?.disconnect();
  scanObserver = null;
  scanScheduled = false;
  if (routeObserver && routerInstance?.removeObserver) {
    routerInstance.removeObserver(routeObserver);
  }
  routeObserver = null;
}
