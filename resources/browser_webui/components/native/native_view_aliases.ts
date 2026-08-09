import {VIEWS_CONTAINER_VIEW_NAME, LOCATION_BAR_AUTO_SIZE_VIEW_NAME, NEW_TAB_TOOLBAR_BUTTON_VIEW_NAME, MAIN_CONTENTS_VIEW_NAME} from '../../browser_constants.mojom-webui.js';
import {LayoutAlignment} from '../../ui_view_controller.mojom-webui.js';
import {NativeViewHolder} from './native_view_holder.js';
type LengthValue = string|number;
type Defaults = Partial<Pick<NativeViewHolder,
  'viewClass'|'viewID'|'alignX'|'alignY'|'round'|'backgroundColor'|'extraStyle'|'cornerRadius'|'hashId'
>> & {
  width?: LengthValue;
  height?: LengthValue;
  hAlign?: LayoutAlignment;
  vAlign?: LayoutAlignment;
  childViews?: any[];
  gap?: number;
  visibility?: CSSStyleDeclaration['visibility'];
};

// BrowserCaptionButtonContainer reserves space for three Windows caption
// buttons: 3 * 45 DIP plus 2 * 1 DIP visual spacing.
const WINDOWS_CAPTION_BUTTON_AREA_WIDTH = 144;

// TabStripActionContainer defaults to the startup GLIC button plus separator.
// On Windows this is ~100 DIP in the default "Gemini" labeled state:
// ~47 DIP text + 40 DIP button chrome + 12 DIP separator and margins.
const TAB_STRIP_ACTION_CONTAINER_WIDTH = 100;

function toCssLength(value: LengthValue): string {
  return typeof value === 'number' ? `${value}px` : value;
}

function quickHash(str: string): number {
  let hash = 5381;
  for (let i = 0; i < str.length; i++) {
    hash = (hash * 33) ^ str.charCodeAt(i);
  }
  return hash >>> 0; // convert to unsigned 32-bit
}

function applyDefaults(el: NativeViewHolder, defaults: Defaults, tagName: string) {
  el.hashId = quickHash(tagName);

  if (defaults.viewClass && !el.hasAttribute('view-class')) {
    el.setAttribute('view-class', defaults.viewClass);
    el.viewClass = defaults.viewClass;
  }
  if (defaults.viewID && !el.hasAttribute('view-id')) {
    el.setAttribute('view-id', String(defaults.viewID));
    el.viewID = String(defaults.viewID);
  }
  if (defaults.alignX && !el.hasAttribute('align-x')) {
    el.setAttribute('align-x', defaults.alignX);
    el.alignX = defaults.alignX;
  }
  if (defaults.alignY && !el.hasAttribute('align-y')) {
    el.setAttribute('align-y', defaults.alignY);
    el.alignY = defaults.alignY;
  }
  if (defaults.round && !el.hasAttribute('round')) {
    el.setAttribute('round', defaults.round);
    el.round = defaults.round as any;
  }
  if (defaults.backgroundColor && !el.hasAttribute('background-color')) {
    el.setAttribute('background-color', String(defaults.backgroundColor));
    el.backgroundColor = String(defaults.backgroundColor);
  }
  if (defaults.extraStyle && !el.hasAttribute('extra-style')) {
    el.setAttribute('extra-style', defaults.extraStyle);
    el.extraStyle = defaults.extraStyle;
  }
  if (defaults.hAlign !== undefined && !el.hasAttribute('h-align')) {
    el.setAttribute('h-align', String(defaults.hAlign));
    el.hAlign = defaults.hAlign;
  }
  if (defaults.vAlign !== undefined && !el.hasAttribute('v-align')) {
    el.setAttribute('v-align', String(defaults.vAlign));
    el.vAlign = defaults.vAlign;
  }
  if (defaults.childViews !== undefined) {
    el.childViews = defaults.childViews;
  }
  if (defaults.gap !== undefined) {
    el.gap = defaults.gap;
  }

  if (defaults.width != null && !el.style.width) {
    el.style.width = toCssLength(defaults.width);
  }

  if (defaults.height != null && !el.style.height) {
    el.style.height = toCssLength(defaults.height);
  }

  if(defaults.visibility != null && !el.style.visibility) {
    el.style.visibility = defaults.visibility;
  }

  if (defaults.cornerRadius != null && !el.hasAttribute('corner-radius')) {
    el.setAttribute('corner-radius', String(defaults.cornerRadius));
    el.cornerRadius = defaults.cornerRadius;
  }
}

export function defineNativeViewAlias(tagName: string, defaults: Defaults) {
  const existing = customElements.get(tagName);
  if (existing) {
    return existing;
  }

  class Alias extends NativeViewHolder {
    static defaults: Defaults = defaults;
    override connectedCallback(): void {
      applyDefaults(this, (this.constructor as typeof Alias).defaults, tagName);
      super.connectedCallback();
    }
  }

  customElements.define(tagName, Alias);
  return Alias;
}

export const CustomBrowserAddressBoxView = defineNativeViewAlias(
  'addressbox-view',
  {viewClass: 'addressboxview', viewID: '10007', alignX: 'left', alignY: 'top'}
);

export const CustomBrowserBackButton = defineNativeViewAlias(
  'back-button',
  {viewClass: 'BackButton', viewID: '1001'}
);

export const CustomBrowserForwardButton = defineNativeViewAlias(
  'forward-button',
  {viewClass: 'ForwardButton', viewID: '1002'}
);

export const CustomBrowserReloadButton = defineNativeViewAlias(
  'reload-button',
  {viewClass: 'ReloadButton', viewID: '1003'}
);

export const CustomBrowserTabSearchContainer = defineNativeViewAlias(
  'tab-search-container',
  {viewClass: 'TabSearchContainer', width: 28, height: 41}
);

export const CustomBrowserTabSearchContainerCenter = defineNativeViewAlias(
  'tab-search-container-center',
  {viewClass: 'TabSearchContainer', hAlign: LayoutAlignment.kCenter}
);

export const CustomBrowserNavigationBarView = defineNativeViewAlias(
  'navigation-bar-view',
  { viewClass: VIEWS_CONTAINER_VIEW_NAME, height: 34, width: 136,
    childViews: [
      {viewId: '1001', width: 34, height: 34},
      {viewId: '1002', width: 34, height: 34},
      {viewId: '1003', width: 34, height: 34},
      {viewId: '1004', width: 34, height: 34}
    ]
  }
);

export const CustomBrowserLocationExtensionBarViews = defineNativeViewAlias(
  'location-extension-bar-views',
  { viewClass: VIEWS_CONTAINER_VIEW_NAME, height: 34,
    gap: 4,
    childViews: [
      {viewClass: 'LocationBarView', height: 34, flex: 1},
      {viewClass: 'ExtensionsToolbarDesktop', height: 34, flex: 0}
    ]
  }
);

export const CustomBrowserSidebarBottomBarViews = defineNativeViewAlias(
  'sidebar-bottom-bar-views',
  { viewClass: VIEWS_CONTAINER_VIEW_NAME, height: 34,
    gap: 4,
    childViews: [
      {viewClass: 'ExtensionsToolbarDesktop', height: 34, flex: 1},
      {viewClass: 'SplitTabsToolbarButton', height: 34, flex: 0}
    ]
  }
);

export const CustomBrowserLocationBarView = defineNativeViewAlias(
  'location-bar-view',
  {viewClass: 'noop-view', height: 34}
);

export const CustomBrowserLocationBarAutoSizeView = defineNativeViewAlias(
  'location-bar-auto-size-view',
  {viewClass: LOCATION_BAR_AUTO_SIZE_VIEW_NAME, height: 34}
);

export const CustomBrowserPinnedToolbarActionsContainer = defineNativeViewAlias(
  'pinned-toolbar-actions',
  {viewClass: 'PinnedToolbarActionsContainer', hAlign: LayoutAlignment.kStart, height: 34}
);

export const CustomBrowserBookmarkBarView = defineNativeViewAlias(
  'bookmark-bar-view',
  {viewClass: 'BookmarkBarView', height: 34}
);

export const CustomBrowserPinnedToolbarActionsCenter = defineNativeViewAlias(
  'pinned-toolbar-actions-center',
  {viewClass: 'PinnedToolbarActionsContainer', hAlign: LayoutAlignment.kCenter, height: 34}
);

export const CustomBrowserAppMenuButton = defineNativeViewAlias(
  'app-menu-button',
  {viewID: '1007', width: 28, height: 34}
);

export const CustomBrowserCaptionButtonContainer = defineNativeViewAlias(
  'caption-button-container',
  {viewClass: 'BrowserCaptionButtonContainer',
   width: WINDOWS_CAPTION_BUTTON_AREA_WIDTH, height: 37}
);

export const CustomBrowserGlicButton = defineNativeViewAlias(
  'glic-nexus-button',
  {viewClass: 'TabStripActionContainer',
   width: TAB_STRIP_ACTION_CONTAINER_WIDTH, height: 37}
);

export const CustomBrowserBrowserButton = defineNativeViewAlias(
  'caption-browser-button',
  {viewClass: 'TabStripActionContainer',
   width: 46, height: 37}
);

export const CustomBrowserPinnedDownloadsButton = defineNativeViewAlias(
  'pinned-downloads-button',
  {viewClass: 'PinnedShowDownloadsButton', width: 34, height: 34}
);

export const CustomBrowserPinnedTabSearchButton = defineNativeViewAlias(
  'pinned-tab-search-button',
  {viewClass: 'PinnedTabSearchButton', width: 34, height: 34}
);

export const CustomBrowserDividerView = defineNativeViewAlias(
  'divider-view',
  {viewClass: 'DividerView', height: 34, width: 2, vAlign: LayoutAlignment.kCenter}
);

export const CustomBrowserDividerViewDark = defineNativeViewAlias(
  'divider-view-dark',
  {viewClass: 'DividerView-Dark', height: 34, width: 16, hAlign: LayoutAlignment.kCenter, vAlign: LayoutAlignment.kCenter}
);

export const CustomBrowserExtensionsToolbarContainer = defineNativeViewAlias(
  'extensions-toolbar-container',
  {viewClass: 'ExtensionsToolbarDesktop', hAlign: LayoutAlignment.kStart, height: 34}
);

export const CustomBrowserSplitTabsButton = defineNativeViewAlias(
  'split-tabs-button',
  {viewClass: 'SplitTabsToolbarButton', width: 34, height: 34}
);

export const CustomBrowserNewTabButton = defineNativeViewAlias(
  'new-tab-button',
  {viewClass: NEW_TAB_TOOLBAR_BUTTON_VIEW_NAME, width: 34, height: 34}
);

export const CustomBrowserMainContentsView = defineNativeViewAlias(
  'main-contents-view',
  {viewClass: MAIN_CONTENTS_VIEW_NAME,
   backgroundColor: "kColorWebUiTabStripBackground",
   alignX:"stretch",
   alignY:"stretch",
   cornerRadius: 5
  }
);

export const CustomBrowserMainContentsViewHidden = defineNativeViewAlias(
  'main-contents-view-hidden',
  {viewClass: MAIN_CONTENTS_VIEW_NAME,
   visibility: 'hidden',
  }
);

export const CustomBrowserNexusViewContainer = defineNativeViewAlias(
  'nexus-view-container',
  {viewClass: 'nexus-view-container'}
);

export const CustomBrowserMainContentsNoBorderView = defineNativeViewAlias(
  'main-contents-view-no-border',
  {viewClass: MAIN_CONTENTS_VIEW_NAME,
   alignX:"stretch",
   alignY:"stretch",
  }
);
