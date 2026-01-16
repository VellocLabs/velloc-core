import {html, render} from '//resources/lit/v3_0/lit.rollup.js';

import './layout_section.js';
import './new_tab_page_section.js';

/*
This file is injected into chrome\browser\resources\side_panel\customize_chrome\customize_chrome.html
to add Custom Browser sections to the Customize Chrome side panel.
*/

type AppElement = HTMLElement&{
  shadowRoot: ShadowRoot|null;
  updateComplete?: Promise<unknown>;
};

type LayoutSelectionEvent =
    CustomEvent<{selection?: string}>;

interface PatchState {
  layoutContainer: HTMLElement|null;
  newTabContainer: HTMLElement|null;
  toolbarButton: HTMLElement|null;
  styleElement: HTMLStyleElement|null;
  onLayoutSelection: ((event: LayoutSelectionEvent) => void)|null;
}

const stateMap = new WeakMap<AppElement, PatchState>();
const CUSTOM_STYLE_ID = 'customBrowserCustomizeChromeStyles';
const CUSTOM_SECTION_STYLES = `
custom-browser-layout-section,
custom-browser-new-tab-page-section {
  display: block;
  max-width: 360px;
  width: 100%;
}

custom-browser-layout-section cr-radio-group {
  --cr-radio-group-item-padding: 0px;
  width: 100%;
}

custom-browser-layout-section .layout-options {
  display: flex;
  flex-direction: column;
  gap: 8px;
}

custom-browser-layout-section .option {
  display: flex;
  align-items: flex-start;
  gap: 16px;
  padding: var(--sp-card-padding);
}

custom-browser-layout-section .option:hover {
  background-color: var(--cr-hover-background-color);
  cursor: pointer;
}

custom-browser-layout-section .option:focus {
  background-color: var(--cr-menu-background-focus-color);
  outline: none;
}

custom-browser-layout-section .button-aria-describedby {
  clip: rect(0, 0, 0, 0);
  display: block;
  position: fixed;
}

custom-browser-layout-section customize-chrome-button-label {
  flex: 1;
}

custom-browser-layout-section cr-radio-button {
  gap: 0;
}

custom-browser-new-tab-page-section .sp-card-content {
  display: flex;
  flex-direction: column;
  gap: 12px;
  padding-bottom: 16px;
}

custom-browser-new-tab-page-section .toggle-row {
  align-items: center;
  display: flex;
  justify-content: space-between;
  gap: 16px;
  padding: var(--sp-card-padding);
}

custom-browser-new-tab-page-section .toggle-row:hover {
  background-color: var(--cr-hover-background-color);
  cursor: pointer;
}

custom-browser-new-tab-page-section .toggle-row:focus {
  background-color: var(--cr-menu-background-focus-color);
  outline: none;
}

custom-browser-new-tab-page-section .toggle-label {
  flex: 1;
  min-width: 0;
}

custom-browser-new-tab-page-section cr-toggle {
  flex: 0 0 auto;
}

custom-browser-new-tab-page-section .action-row {
  padding: 0 16px;
}

custom-browser-new-tab-page-section cr-chip {
  cursor: pointer;
}

custom-browser-new-tab-page-section .chip-inner {
  display: inline-flex;
  align-items: center;
  gap: 8px;
  white-space: nowrap;
  user-select: none;
}

custom-browser-new-tab-page-section .chip-label {
  color: var(--cr-primary-text-color);
  font-size: 13px;
  line-height: 18px;
}

custom-browser-new-tab-page-section cr-icon {
  width: 18px;
  height: 18px;
}
`;

function setToolbarButtonVisibility(
    toolbarButton: HTMLElement, visible: boolean) {
  if (visible) {
    toolbarButton.setAttribute('aria-hidden', 'false');
    toolbarButton.removeAttribute('tabindex');
    toolbarButton.style.display = '';
    return;
  }
  toolbarButton.setAttribute('aria-hidden', 'true');
  toolbarButton.setAttribute('tabindex', '-1');
  toolbarButton.style.display = 'none';
}

function getOrCreateState(app: AppElement): PatchState {
  let state = stateMap.get(app);
  if (state) {
    return state;
  }

  state = {
    layoutContainer: null,
    newTabContainer: null,
    toolbarButton: null,
    styleElement: null,
    onLayoutSelection: null,
  };
  stateMap.set(app, state);
  return state;
}

function ensureStyles(app: AppElement, state: PatchState) {
  const shadow = app.shadowRoot;
  if (!shadow) {
    return;
  }

  let style = state.styleElement;
  if (style && style.isConnected) {
    return;
  }

  style = shadow.getElementById(CUSTOM_STYLE_ID) as HTMLStyleElement | null;
  if (!style) {
    style = document.createElement('style');
    style.id = CUSTOM_STYLE_ID;
    style.textContent = CUSTOM_SECTION_STYLES;
    shadow.appendChild(style);
  }

  state.styleElement = style;
}

function ensureToolbarButton(app: AppElement, state: PatchState):
    HTMLElement|null {
  if (state.toolbarButton && state.toolbarButton.isConnected) {
    return state.toolbarButton;
  }

  const toolbarButton = app.shadowRoot?.getElementById('toolbarButton') ??
      null;
  if (!toolbarButton) {
    return null;
  }
  setToolbarButtonVisibility(toolbarButton, false);
  state.toolbarButton = toolbarButton;
  return toolbarButton;
}

function ensureLayoutContainer(app: AppElement, state: PatchState):
    HTMLElement|null {
  const shadow = app.shadowRoot;
  if (!shadow) {
    return null;
  }

  const appearanceSection = shadow.getElementById('appearance');
  const toolbarButton = ensureToolbarButton(app, state);
  const parent = appearanceSection?.parentNode ?? toolbarButton?.parentNode;
  if (!parent) {
    return null;
  }

  let container = state.layoutContainer;
  if (!container || !container.isConnected) {
    container = document.createElement('div');
    container.id = 'customBrowserLayoutMount';
    const reference =
        appearanceSection ?? toolbarButton?.nextSibling ?? null;
    parent.insertBefore(container, reference);
    state.layoutContainer = container;
  } else if (
      appearanceSection && container.nextSibling !== appearanceSection) {
    parent.insertBefore(container, appearanceSection);
  }

  return container;
}

function ensureNewTabContainer(app: AppElement, state: PatchState):
    HTMLElement|null {
  const shadow = app.shadowRoot;
  if (!shadow) {
    return null;
  }

  const shortcutsSection = shadow.getElementById('shortcuts');
  const appearanceSection = shadow.getElementById('appearance');
  const parent = shortcutsSection?.parentNode ??
      appearanceSection?.parentNode ??
      shadow;
  if (!parent) {
    return null;
  }

  let container = state.newTabContainer;
  if (!container || !container.isConnected) {
    container = document.createElement('div');
    container.id = 'customBrowserNtpMount';
    const reference = shortcutsSection ?? null;
    parent.insertBefore(container, reference);
    state.newTabContainer = container;
  } else if (shortcutsSection && container.nextSibling !== shortcutsSection) {
    parent.insertBefore(container, shortcutsSection);
  }

  return container;
}


function handleLayoutSelection(
    app: AppElement, state: PatchState, event: LayoutSelectionEvent) {
  const selection = event.detail?.selection ?? 'sidebar';
  const toolbarButton = ensureToolbarButton(app, state);
  if (!toolbarButton) {
    return;
  }

  const visible =
      selection === 'standard' || selection === 'compactStandard';
  setToolbarButtonVisibility(toolbarButton, visible);
}

function renderLayoutSection(app: AppElement, state: PatchState) {
  const container = ensureLayoutContainer(app, state);
  if (!container) {
    return;
  }

  if (!state.onLayoutSelection) {
    state.onLayoutSelection = (event: LayoutSelectionEvent) =>
        handleLayoutSelection(app, state, event);
  }

  render(html`
    <custom-browser-layout-section
        @layout-selection-changed=${state.onLayoutSelection}>
    </custom-browser-layout-section>
  `, container);
}

function renderNewTabSection(app: AppElement, state: PatchState) {
  const container = ensureNewTabContainer(app, state);
  if (!container) {
    return;
  }

  render(html`
    <custom-browser-new-tab-page-section>
    </custom-browser-new-tab-page-section>
  `, container);
}

function attach(app: AppElement): void {
  const shadow = app.shadowRoot;
  if (!shadow) {
    return;
  }

  const state = getOrCreateState(app);
  ensureToolbarButton(app, state);
  ensureStyles(app, state);
  renderLayoutSection(app, state);
  renderNewTabSection(app, state);
}

function detach(app: AppElement) {
  const state = stateMap.get(app);
  if (!state) {
    return;
  }

  if (state.layoutContainer) {
    render(html``, state.layoutContainer);
    state.layoutContainer.remove();
  }

  if (state.newTabContainer) {
    render(html``, state.newTabContainer);
    state.newTabContainer.remove();
  }

  if (state.toolbarButton) {
    setToolbarButtonVisibility(state.toolbarButton, false);
  }

  if (state.styleElement) {
    state.styleElement.remove();
    state.styleElement = null;
  }

  stateMap.delete(app);
}

function scheduleAttach(app: AppElement) {
  const run = () => attach(app);
  const promise = app.updateComplete;
  if (promise && typeof promise.then === 'function') {
    promise.then(run);
  } else {
    requestAnimationFrame(run);
  }
}

customElements.whenDefined('customize-chrome-app').then(() => {
  const ctor = customElements.get('customize-chrome-app') as
      (new () => AppElement)|undefined;
  if (!ctor) {
    return;
  }

  const proto = ctor.prototype as AppElement & {
    connectedCallback?: () => void;
    disconnectedCallback?: () => void;
    __customBrowserPatched?: boolean;
  };

  if (proto.__customBrowserPatched) {
    return;
  }
  proto.__customBrowserPatched = true;

  const originalConnected = proto.connectedCallback;
  proto.connectedCallback = function() {
    originalConnected?.call(this);
    scheduleAttach(this as AppElement);
  };

  const originalDisconnected = proto.disconnectedCallback;
  proto.disconnectedCallback = function() {
    const app = this as AppElement;
    detach(app);
    originalDisconnected?.call(this);
  };

  const protoWithUpdate = proto as typeof proto & {
    updated?: (...args: unknown[]) => void;
  };

  const originalUpdated = protoWithUpdate.updated;
  protoWithUpdate.updated = function(...args: unknown[]) {
    originalUpdated?.apply(this, args);
    const app = this as AppElement;
    const state = stateMap.get(app);
    if (!state ||
        !state.layoutContainer?.isConnected ||
        !state.newTabContainer?.isConnected) {
      scheduleAttach(app);
    }
  };
});
