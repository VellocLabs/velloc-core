// Copyright 2025 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// <nexus-bg-tabs-strip>: a clickable preview list of every non-active tab.
// Thumbnails arrive as data-URIs for each `addTrackedPreviewId(id)`; observers
// and tracked ids are registered on connect and all released on disconnect.

import {Tab} from '../../browser_controller.mojom-webui.js';
import type {TabId} from '../../components/tabs_panel/types.js';
import {
  tabsController,
  type TabExtras,
  type TabObserver,
  type TabsObserver,
} from '../../controller/tabs_controller.js';
import {
  CrLitElement,
  css,
  html,
  nothing,
  type CSSResultGroup,
} from 'chrome://resources/lit/v3_0/lit.rollup.js';

import {getString} from './nexus_shared.js';

export class NexusBgTabsStrip extends CrLitElement {
  static get is() {
    return 'nexus-bg-tabs-strip';
  }

  static override styles: CSSResultGroup = css`
    :host {
      background: linear-gradient(
          180deg,
          var(--cr-card-background-color, #fafbfd) 0%,
          var(--md-background-color, #f4f6fa) 100%);
      box-sizing: border-box;
      display: block;
      max-height: 180px;
      overflow: hidden;
      padding: 12px 14px 14px;
    }

    :host([hidden]) {
      display: none !important;
    }

    .strip-label {
      color: var(--cr-secondary-text-color, var(--text-secondary));
      flex-shrink: 0;
      font-size: 10.5px;
      font-weight: 600;
      letter-spacing: 0.6px;
      margin-bottom: 8px;
      text-transform: uppercase;
    }

    .scroll {
      display: flex;
      gap: 10px;
      overflow-x: auto;
      overflow-y: hidden;
      padding-bottom: 6px;
      scrollbar-width: thin;
    }
    .scroll::-webkit-scrollbar {
      height: 8px;
    }
    .scroll::-webkit-scrollbar-track { background: transparent; }
    .scroll::-webkit-scrollbar-thumb {
      background: var(--cr-separator-color);
      background-clip: padding-box;
      border: 2px solid transparent;
      border-radius: 4px;
    }
    .scroll::-webkit-scrollbar-thumb:hover {
      background: var(--google-grey-500);
      background-clip: padding-box;
      border: 2px solid transparent;
    }

    .card {
      background: var(--cr-card-background-color, #fff);
      border: 1px solid var(--cr-separator-color);
      border-radius: 10px;
      box-sizing: border-box;
      cursor: pointer;
      display: flex;
      flex-direction: column;
      flex-shrink: 0;
      font: inherit;
      overflow: hidden;
      padding: 0;
      text-align: left;
      transition: border-color 0.18s, box-shadow 0.18s, transform 0.18s;
      width: 156px;
    }
    .card:hover {
      border-color: var(--google-blue-500);
      box-shadow: 0 6px 16px rgba(15, 23, 42, 0.10),
                  0 2px 4px rgba(15, 23, 42, 0.05);
      transform: translateY(-2px);
    }
    .card:focus-visible {
      outline: 2px solid var(--google-blue-500);
      outline-offset: 2px;
    }

    .shot {
      align-items: center;
      background-color: var(--md-background-color, #eef0f4);
      background-position: center;
      background-repeat: no-repeat;
      background-size: cover;
      border-bottom: 1px solid var(--cr-separator-color);
      display: grid;
      height: 90px;
      place-items: center;
      position: relative;
    }
    .shot__placeholder {
      background: rgba(255, 255, 255, 0.92);
      border-radius: 999px;
      box-shadow: 0 1px 2px rgba(0, 0, 0, 0.05);
      color: var(--cr-primary-text-color, var(--text-primary));
      font-size: 10px;
      font-weight: 600;
      max-width: 90%;
      overflow: hidden;
      padding: 3px 8px;
      text-overflow: ellipsis;
      white-space: nowrap;
    }

    .meta {
      align-items: center;
      background: var(--cr-card-background-color, #fff);
      display: flex;
      gap: 6px;
      padding: 8px 10px;
    }
    .meta__fav {
      background-position: center;
      background-repeat: no-repeat;
      background-size: 12px 12px;
      border-radius: 3px;
      box-shadow: inset 0 0 0 1px rgba(0, 0, 0, 0.05);
      flex-shrink: 0;
      height: 12px;
      width: 12px;
    }
    .meta__title {
      color: var(--cr-primary-text-color, var(--text-primary));
      flex: 1;
      font-size: 11.5px;
      font-weight: 500;
      letter-spacing: -0.01em;
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
    }
  `;

  // --- State ---

  private tabsObserver_: TabsObserver|null = null;
  // Every tab (incl. active) gets an observer so we catch the activation flip.
  private tabObservers_ = new Map<TabId, TabObserver>();
  // Background tabs only — we opt these in to thumbnail delivery.
  private trackedPreviewIds_ = new Set<TabId>();
  private bgTabIds_: TabId[] = [];
  private tabs_ = new Map<TabId, Tab>();
  private thumbnails_ = new Map<TabId, string|null>();

  // --- Lifecycle ---

  override connectedCallback(): void {
    super.connectedCallback();
    this.tabsObserver_ = {
      onTabsUpdated: (pinned: TabId[], unpinned: TabId[]) =>
          this.rebuild_(pinned, unpinned),
    };
    tabsController.addTabsObserver(this.tabsObserver_);
    this.rebuild_(
        tabsController.getPinnedTabIds(),
        tabsController.getUnpinnedTabIds());
  }

  override disconnectedCallback(): void {
    if (this.tabsObserver_) {
      tabsController.removeTabsObserver(this.tabsObserver_);
      this.tabsObserver_ = null;
    }
    for (const [id, obs] of this.tabObservers_) {
      tabsController.removeTabObserver(id, obs);
    }
    for (const id of this.trackedPreviewIds_) {
      tabsController.removeTrackedPreviewId(id);
    }
    this.tabObservers_.clear();
    this.trackedPreviewIds_.clear();
    this.tabs_.clear();
    this.thumbnails_.clear();
    super.disconnectedCallback();
  }

  // --- Subscription management ---

  private rebuild_(pinned: TabId[], unpinned: TabId[]): void {
    const allIds = new Set<TabId>([...pinned, ...unpinned]);

    // Drop observers for tabs that no longer exist.
    for (const oldId of [...this.tabObservers_.keys()]) {
      if (!allIds.has(oldId)) {
        const obs = this.tabObservers_.get(oldId)!;
        tabsController.removeTabObserver(oldId, obs);
        this.tabObservers_.delete(oldId);
        this.tabs_.delete(oldId);
        this.thumbnails_.delete(oldId);
      }
    }

    // Add observers for newly-tracked tabs.
    for (const id of allIds) {
      if (this.tabObservers_.has(id)) {
        continue;
      }
      const obs: TabObserver = {
        onTabDataUpdated: (tab: Tab) => this.onTabDataUpdated_(id, tab),
        onTabExtrasUpdated: (_extras: TabExtras) => {},
        onThumbnailChanged: (url: string|null) =>
            this.onThumbnailChanged_(id, url),
        onTabActiveStateChanged: (_active: boolean) =>
            this.rebuildBackgroundIds_(),
      };
      tabsController.addTabObserver(id, obs);
      this.tabObservers_.set(id, obs);
      const tab = tabsController.getTab(id);
      if (tab) {
        this.tabs_.set(id, tab);
      }
    }

    this.rebuildBackgroundIds_();
  }

  private rebuildBackgroundIds_(): void {
    const activeId = tabsController.getActiveTabId();
    const allIds = [
      ...tabsController.getPinnedTabIds(),
      ...tabsController.getUnpinnedTabIds(),
    ];
    const newBg = allIds.filter(id => id !== activeId);

    // Untrack tabs that are no longer background.
    for (const id of [...this.trackedPreviewIds_]) {
      if (!newBg.includes(id)) {
        tabsController.removeTrackedPreviewId(id);
        this.trackedPreviewIds_.delete(id);
      }
    }

    // Track newly-background tabs.
    for (const id of newBg) {
      if (!this.trackedPreviewIds_.has(id)) {
        tabsController.addTrackedPreviewId(id);
        this.trackedPreviewIds_.add(id);
      }
    }

    this.bgTabIds_ = newBg;
    this.requestUpdate();
  }

  private onTabDataUpdated_(tabId: TabId, tab: Tab): void {
    this.tabs_.set(tabId, tab);
    this.requestUpdate();
  }

  private onThumbnailChanged_(tabId: TabId, url: string|null): void {
    if (url) {
      this.thumbnails_.set(tabId, url);
    } else {
      this.thumbnails_.delete(tabId);
    }
    this.requestUpdate();
  }

  // --- Click handler ---

  private onCardClick_(tabId: TabId): void {
    tabsController.activateTab(tabId);
    // No manual rebuild — onTabActiveStateChanged on the previous and new
    // active tabs will fire and trigger rebuildBackgroundIds_.
  }

  // --- Render ---

  protected override render() {
    if (this.bgTabIds_.length === 0) {
      return nothing;
    }
    const label = getString(
        'customBrowserNexusBgTabsLabel',
        `Background tabs · ${this.bgTabIds_.length}`);
    return html`
      <div class="strip-label">${label}</div>
      <div class="scroll">
        ${this.bgTabIds_.map(id => this.renderCard_(id))}
      </div>
    `;
  }

  private renderCard_(id: TabId) {
    const tab = this.tabs_.get(id);
    if (!tab) {
      return nothing;
    }
    const thumb = this.thumbnails_.get(id) || null;
    const titleText = tab.title || tab.domain || tab.url || '';
    const shotStyle = thumb ? `background-image: url(${thumb});` : '';
    const favStyle = tab.faviconUrl
        ? `background-image: url(${tab.faviconUrl});`
        : '';
    return html`
      <button class="card"
          type="button"
          @click=${() => this.onCardClick_(id)}
          title=${tab.url || titleText}>
        <div class="shot" style=${shotStyle}>
          ${thumb ? nothing : html`
            <span class="shot__placeholder">${
                titleText.slice(0, 18)}</span>`}
        </div>
        <div class="meta">
          <span class="meta__fav" style=${favStyle}></span>
          <span class="meta__title">${titleText}</span>
        </div>
      </button>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'nexus-bg-tabs-strip': NexusBgTabsStrip;
  }
}

customElements.define(NexusBgTabsStrip.is, NexusBgTabsStrip);
