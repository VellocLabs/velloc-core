import {BrowserApiProxyImpl, CloseTabAction} from "../browser_api_proxy.js";
import {BrowserCallbackRouter, Tab} from "../browser_controller.mojom-webui.js";
import {type TabId} from "../components/tabs_panel/types.js";
import {assert} from 'chrome://resources/js/assert.js';
const THUMBNAIL_UPDATE_DEBOUNCE_MS = 90;

export interface TabExtras {
  createdTime: number;
  lastActivatedTime: number | null;
  openedCount: number;
  thumbnailUrl: string | null;
}

export interface TabObserver {
  onTabDataUpdated(tab: Tab): void;
  onTabExtrasUpdated(extras: TabExtras): void;
  onTabActiveStateChanged(active: boolean): void;
  onThumbnailChanged(thumbnailUrl: string | null): void;
}

export interface TabsObserver {
  onTabsUpdated(pinnedTabs: TabId[], unpinnedTabs: TabId[]): void;
}

class TabsController {
  private readonly browserApi = BrowserApiProxyImpl.getInstance();
  private readonly router: BrowserCallbackRouter;
  private readonly tabById = new Map<TabId, Tab>();
  private readonly extrasById = new Map<TabId, TabExtras>();
  private readonly trackedPreviewIds = new Set<TabId>();
  private readonly thumbnailQueue = new Map<TabId, string>();
  private readonly tabObservers = new Map<TabId, Set<TabObserver>>();
  private readonly tabsObservers = new Set<TabsObserver>();
  private sortedId: TabId[] = [];
  private thumbnailTimer: number | null = null;
  private pinnedTabIds: TabId[] = [];
  private unpinnedTabIds: TabId[] = [];
  private activeTabId: TabId | null = null;
  private initialized = false;
  private loadingSnapshot = false;

  constructor() {
    this.router = this.browserApi.getBrowserCallbackRouter();
    this.registerListeners();
    void this.loadSnapshot();
  }

  addTabObserver(tabId: TabId, observer: TabObserver) {
    let observers = this.tabObservers.get(tabId);
    if (!observers) {
      observers = new Set();
      this.tabObservers.set(tabId, observers);
    }
    observers.add(observer);
    this.syncTabObserver(tabId, observer);
  }

  removeTabObserver(tabId: TabId, observer: TabObserver) {
    const observers = this.tabObservers.get(tabId);
    if (!observers) {
      return;
    }
    observers.delete(observer);
    if (!observers.size) {
      this.tabObservers.delete(tabId);
    }
  }

  addTabsObserver(observer: TabsObserver) {
    this.tabsObservers.add(observer);
    this.syncTabsObserver(observer);
  }

  removeTabsObserver(observer: TabsObserver) {
    this.tabsObservers.delete(observer);
  }

  getTab(tabId: TabId): Tab | null {
    const tab = this.tabById.get(tabId);
    return tab ? this.cloneTab(tab) : null;
  }

  getPinnedTabIds(): TabId[] {
    return [...this.pinnedTabIds];
  }

  getUnpinnedTabIds(): TabId[] {
    return [...this.unpinnedTabIds];
  }

  getActiveTabId(): TabId | null {
    return this.activeTabId;
  }

  activateTab(tabId: TabId) {
    this.browserApi.activateTab(tabId);
  }

  closeTab(tabId: TabId) {
    this.browserApi.closeTab(tabId, CloseTabAction.CLOSE_BUTTON);
  }

  addTrackedPreviewId(tabId: TabId) {
    if (this.trackedPreviewIds.has(tabId)) {
      return;
    }
    this.trackedPreviewIds.add(tabId);
    this.browserApi.setThumbnailTracked(tabId, true);
  }

  removeTrackedPreviewId(tabId: TabId) {
    if (!this.trackedPreviewIds.has(tabId)) {
      return;
    }
    this.trackedPreviewIds.delete(tabId);
    this.browserApi.setThumbnailTracked(tabId, false);
  }

  private syncTabObserversWithCurrentState() {
    for (const [tabId, observers] of this.tabObservers) {
      for (const observer of observers) {
        this.syncTabObserver(tabId, observer);
      }
    }
  }

  private syncTabObserver(tabId: TabId, observer: TabObserver) {
    if (!this.initialized) {
      return;
    }
    const tab = this.tabById.get(tabId);
    if (tab) {
      observer.onTabDataUpdated(this.cloneTab(tab));
      const extras = this.extrasById.get(tabId);
      if (extras) {
        observer.onTabExtrasUpdated(this.cloneExtras(extras));
      }
      observer.onTabActiveStateChanged(tabId === this.activeTabId);
    }
  }

  private syncTabsObserver(observer: TabsObserver) {
    if (!this.initialized) {
      return;
    }
    // Need to rebuild tabs to ensure unpinnedTabIds exist.
    // Bug: When start with chrome_ui, and then switch to custom_ui, the unpinnedTabIds is empty initially.
    this.rebuildTabs();
    const pinned = [...this.pinnedTabIds];
    const unpinned = [...this.unpinnedTabIds];
    observer.onTabsUpdated(pinned, unpinned);
  }

  private registerListeners() {
    this.router.tabCreated.addListener((tab: Tab) => this.onTabCreated(tab));
    this.router.tabUpdated.addListener((tab: Tab) => this.onTabUpdated(tab));
    this.router.tabRemoved.addListener((tabId: TabId) => this.onTabRemoved(tabId));
    this.router.tabMoved.addListener((tabId: TabId, toIndex: number, _inPinned: boolean) => this.onTabMoved(tabId, toIndex));
    this.router.tabReplaced.addListener((tabId: TabId, newTabId: TabId) => this.onTabReplaced(tabId, newTabId));
    this.router.tabActiveChanged.addListener((tabId: TabId) => this.onTabActiveChanged(tabId));
    this.router.tabThumbnailUpdated.addListener((tabId: TabId, dataUri: string) => this.onThumbnailUpdated(tabId, dataUri));
  }

  private async loadSnapshot() {
    if (this.loadingSnapshot) {
      return;
    }
    this.loadingSnapshot = true;
    try {
      const {tabs} = await this.browserApi.getTabs();
      this.clearTabs();
      for (const tab of tabs) {
        const clone: Tab = {...tab};
        this.addTab(clone);
        if (tab.active) {
          this.activeTabId = tab.id;
        }
      }
      this.initialized = true;
      this.notifyTabsObservers();
      this.syncTabObserversWithCurrentState();
    } catch (error) {
      console.error("Failed to load tab snapshot", error);
    } finally {
      this.loadingSnapshot = false;
    }
  }

  private clearTabs() {
    this.tabById.clear();
    this.extrasById.clear();
    this.trackedPreviewIds.clear();
    this.thumbnailQueue.clear();

    this.pinnedTabIds = [];
    this.unpinnedTabIds = [];
    this.activeTabId = null;
    this.sortedId = [];
  }

  private addTab(tab: Tab) {
    assert(!this.tabById.has(tab.id), 'Tab already exists when adding');
    const clone = this.cloneTab(tab);
    this.tabById.set(tab.id, clone);
    this.positionSortedId(tab.id, tab.index);
    return clone;
  }

  private updateTab(tab: Tab) {
    const storedTab = this.tabById.get(tab.id);
    if (!storedTab) {
      const clone = this.cloneTab(tab);
      this.tabById.set(tab.id, clone);
      this.positionSortedId(tab.id, tab.index);
      this.notifyTabsObservers();
      return;
    }
    assert(storedTab, `Attempted to update unknown tab id ${tab.id}`);
    Object.assign(storedTab, tab);
    this.positionSortedId(tab.id, tab.index);
    this.notifyTabDataUpdated(storedTab);
    this.notifyTabsObservers();
  }

  private deleteTab(tabId: TabId) {
    this.tabById.delete(tabId);
    this.extrasById.delete(tabId);
    this.tabObservers.delete(tabId);
    if (this.trackedPreviewIds.delete(tabId)) {
      this.browserApi.setThumbnailTracked(tabId, false);
    }
    this.thumbnailQueue.delete(tabId);
    if (this.activeTabId === tabId) {
      this.activeTabId = null;
    }
    this.removeSortedId(tabId);
    this.notifyTabsObservers();
  }

  private onTabCreated(tab: Tab) {
    this.updateTab(tab);
  }

  private onTabUpdated(tab: Tab) {
    this.updateTab(tab);
  }

  private onTabRemoved(tabId: TabId) {
   this.deleteTab(tabId);
  }

  private onTabMoved(tabId: TabId, toIndex: number) {
    const tab = this.tabById.get(tabId);
    if (!tab) {
      return;
    }
    tab.index = toIndex;
    this.positionSortedId(tabId, toIndex);
    this.notifyTabsObservers();
  }

  private onTabReplaced(tabId: TabId, newTabId: TabId) {
    // TODO
    console.warn('Tab replacement not implemented yet', tabId, newTabId);
  }

  private onTabActiveChanged(tabId: TabId) {
    // assert(tabId != this.activeTabId, 'Tab is already active');
    const timestamp = Date.now();
    const previousActiveId = this.activeTabId;
    const tab = this.tabById.get(tabId);
    if (!tab) {
      return;
    }
    tab.active = true;
    let previousTab: Tab | null = null;
    if (previousActiveId !== null) {
      previousTab = this.tabById.get(previousActiveId) ?? null;
      if (previousTab) {
        previousTab.active = false;
      }
    }
    this.activeTabId = tabId;
    this.notifyTabActiveStateChanged(tabId, previousActiveId);
    const extras = this.ensureExtrasForTab(tab, timestamp);
    extras.openedCount += 1;
    extras.lastActivatedTime = timestamp;
    this.notifyTabExtrasUpdated(tabId, extras);
    this.notifyTabsObservers();
  }

  private onThumbnailUpdated(tabId: TabId, dataUri: string) {
    this.thumbnailQueue.set(tabId, dataUri);
    if (this.thumbnailTimer !== null) {
      window.clearTimeout(this.thumbnailTimer);
    }
    this.thumbnailTimer = window.setTimeout(
        () => this.flushThumbnailQueue(), THUMBNAIL_UPDATE_DEBOUNCE_MS);
  }

  private ensureExtrasForTab(tab: Tab, timestamp: number): TabExtras {
    let extras = this.extrasById.get(tab.id);
    if (!extras) {
      extras = {
        createdTime: timestamp,
        lastActivatedTime: tab.active ? timestamp : null,
        openedCount: tab.active ? 1 : 0,
        thumbnailUrl: null,
      };
      this.extrasById.set(tab.id, extras);
    } else if (tab.active && extras.lastActivatedTime === null) {
      extras.lastActivatedTime = timestamp;
    }
    return extras;
  }

  private rebuildTabs(): boolean {
    assert(this.sortedId.length === this.tabById.size, 'Sorted ID list size mismatch');
    const nextPinned: TabId[] = [];
    const nextUnpinned: TabId[] = [];
    for (const tabId of this.sortedId) {
      const tab = this.tabById.get(tabId);
      assert(tab, `Tab missing during rebuild: ${tabId}`);
      if (tab.pinned) {
        nextPinned.push(tabId);
      } else {
        this.ensureExtrasForTab(tab, Date.now());
        nextUnpinned.push(tabId);
      }
    }
    const pinnedChanged = !this.areTabIdListsEqual(this.pinnedTabIds, nextPinned);
    const unpinnedChanged = !this.areTabIdListsEqual(this.unpinnedTabIds, nextUnpinned);
    this.pinnedTabIds = nextPinned;
    this.unpinnedTabIds = nextUnpinned;
    return pinnedChanged || unpinnedChanged;
  }

  private boundSortedIndex(targetIndex: number): number {
    if (targetIndex < 0) {
      return 0;
    }
    if (targetIndex > this.sortedId.length) {
      return this.sortedId.length;
    }
    return targetIndex;
  }

  private positionSortedId(tabId: TabId, targetIndex: number) {
    const index = this.boundSortedIndex(targetIndex);
    if (this.sortedId[index] === tabId) {
      return;
    }
    this.removeSortedId(tabId);
    this.sortedId.splice(index, 0, tabId);
  }

  private removeSortedId(tabId: TabId) {
    const index = this.sortedId.indexOf(tabId);
    if (index !== -1) {
      this.sortedId.splice(index, 1);
    }
  }

  private areTabIdListsEqual(a: TabId[], b: TabId[]) {
    if (a.length !== b.length) {
      return false;
    }
    for (let i = 0; i < a.length; ++i) {
      if (a[i] !== b[i]) {
        return false;
      }
    }
    return true;
  }

  private flushThumbnailQueue() {
    this.thumbnailTimer = null;
    if (!this.thumbnailQueue.size) {
      return;
    }
    for (const [tabId, dataUri] of this.thumbnailQueue) {
      this.notifyThumbnailChanged(tabId, dataUri);
    }
    this.thumbnailQueue.clear();
  }

  private notifyTabDataUpdated(tab: Tab) {
    this.notifyTabImpl(tab.id, (observer) => {
      observer.onTabDataUpdated(this.cloneTab(tab));
    });
  }

  private notifyTabExtrasUpdated(tabId: TabId, extras: TabExtras) {
    this.notifyTabImpl(tabId, (observer) => {
      observer.onTabExtrasUpdated(this.cloneExtras(extras));
    });
  }

  private notifyTabActiveStateChanged(activeTabId: TabId, oldActiveId: TabId | null) {
    if (oldActiveId !== null) {
      this.notifyTabImpl(oldActiveId, (observer) => {
        observer.onTabActiveStateChanged(false);
      });
    }
    this.notifyTabImpl(activeTabId, (observer) => {
        observer.onTabActiveStateChanged(true);
    });
  }

  private notifyTabImpl(tabId: TabId, callback: (observer: TabObserver) => void) {
    if (!this.initialized) {
      return;
    }
    const observers = this.tabObservers.get(tabId);
    if (!observers?.size) {
      return;
    }
    for (const observer of observers) {
      callback(observer);
    }
  }

  private notifyThumbnailChanged(tabId: TabId, thumbnailUrl: string | null) {
    this.notifyTabImpl(tabId, (observer) => {
      observer.onThumbnailChanged(thumbnailUrl);
    });
  }

  private notifyTabsObservers() {
    if (!this.initialized || !this.tabsObservers.size) {
      return;
    }
    const tabsChanged = this.rebuildTabs();
    if (!tabsChanged) {
      return;
    }
    const pinned = [...this.pinnedTabIds];
    const unpinned = [...this.unpinnedTabIds];
    for (const observer of this.tabsObservers) {
      observer.onTabsUpdated(pinned, unpinned);
    }
  }

  private cloneTab(tab: Tab): Tab {
    const clone: Tab = {...tab};
    // url.mojom.Url is a string in Chromium 146+ bindings; no deep clone needed.
    return clone;
  }

  private cloneExtras(extras: TabExtras): TabExtras {
    return {...extras};
  }
}

export const tabsController = new TabsController();
