// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/* eslint-disable @webui-eslint/lit-element-template-structure */

import {html, css, nothing, type CSSResultGroup} from '//resources/lit/v3_0/lit.rollup.js';

import type {TabItem} from './tab_item.js';
import {CrUrlListItemSize} from '//resources/cr_elements/cr_url_list_item/cr_url_list_item.js';

export const tabItemStyles: CSSResultGroup = css`
    :host {
      /* The tab's outer box - size usually controlled by parent */
      --tabstrip-tab-transition-duration: 250ms;
      display: flex;
      flex-direction: column;
      box-sizing: border-box;
      position: relative; /* needed for the ::after separator positioning */
      inline-size: auto;
      max-inline-size: 100%;
      overflow: hidden;
      transition: box-shadow 80ms linear;

      --hover-bg: var(--vtp-list-hover-bg, var(--color-toolbar-background-subtle-emphasis-hovered));
      --hover-shadow: var(--vtp-list-hover-shadow, var(--cr-elevation-2));

      --active-bg: var(--vtp-list-active-bg, var(--color-toolbar-background-subtle-emphasis));
      --active-shadow: var(--vtp-list-active-shadow, var(--cr-elevation-2));

      --active-border: var(--vtp-list-active-border, var(--color-web-ui-tab-strip-focus-outline));
      --cr-url-list-item-height: var(--vtp-list-item-height, 34px);
    }



    :host([split="start"]) {
      border-radius: var(--vtp-list-radius) var(--vtp-list-radius) 0 0;
      border: 1px dashed var(--active-border);
      border-bottom: none;
    }

    :host([split="end"]) {
      border-radius: 0 0 var(--vtp-list-radius) var(--vtp-list-radius);
      border: 1px dashed var(--active-border);
      border-top: none;
    }

    :host([hidden]) {
      display: none !important;
    }

    #preview-thumbnail {
      display: none;
    }

    #container {
      margin: var(--vtp-list-margin, 3px);
      border: 1px solid color-mix(in oklch, var(--app-bg) 85%, transparent);
      border-radius: var(--vtp-list-radius);
      box-sizing: border-box;
    }

    :host(:hover) #container  {
      background-color: var(--hover-bg);
      box-shadow: var(--hover-shadow);
    }

    :host([active]) #container {
      background-color: var(--active-bg);
      box-shadow: var(--active-shadow);
    }

    :host([show_preview]) #container{
      --cr-url-list-item-height: 28px;
      box-shadow: var(--hover-shadow);
    }

    :host([show_preview]) #preview-thumbnail {
      inline-size: 100%;
      aspect-ratio: 1.78;
      border-radius: 12px;
      overflow: hidden;
      display: grid;
      place-items: center;
    }

    :host([show_preview]) #preview-thumbnail img {
      inline-size: 100%;
      block-size: 100%;
      display: block;
      object-fit: cover;
    }

    cr-icon-button {
      --cr-icon-button-size: 17px;
      --cr-icon-button-icon-size: 13px;
      --cr-icon-button-hover-background-color: color-mix(in oklch, var(--app-bg) 55%, transparent);
    }

    /* Left side: favicon container */
    #faviconContainer {
      --favicon-size: 16px;
      flex-shrink: 0;
      height: var(--favicon-size);
      margin-inline-end: 5px;
      margin-inline-start: 2px;
      max-width: var(--favicon-size);
      position: relative;
      /* When transitioning to the default visible state, the margin and max-width
       * transitions should finish first, then the opacity should be set to 1.
       * This prevents the favicon and loading spinners from looking cropped
       * while the element transitions. */
      transition: margin var(--tabstrip-tab-transition-duration),
                  max-width var(--tabstrip-tab-transition-duration),
                  opacity 0ms linear var(--tabstrip-tab-transition-duration);
      width: var(--favicon-size);
    }

    :host([item_mode="icon"]) #faviconContainer {
      margin-inline: 0;
      --cr-url-list-item-padding: 0;
    }

    /* Pinned tabs in grid mode: round icon buttons */
    :host([item_mode="icon"][pinned]) {
      border-radius: 50%;
    }

    :host([item_mode="icon"][pinned]) #container {
      border-radius: 50%;
      margin: 0;
      inline-size: 100%;
      block-size: 100%;
      overflow: hidden;
    }

    :host([hide-icon_]) #faviconContainer {
      margin-inline-end: 0;
      max-width: 0;
      opacity: 0;
      /* When transitioning to the hidden state, set opacity immediately to 0
       * while transitioning the other values normally. */
      transition: margin var(--tabstrip-tab-transition-duration),
                  max-width var(--tabstrip-tab-transition-duration),
                  opacity 0ms;
    }

    /* Favicon and related indicators */
    #progressSpinner,
    #favicon,
    #crashedIcon {
      height: var(--favicon-size);
      left: 50%;
      position: absolute;
      top: 50%;
      transform: translate(-50%, -50%);
      width: var(--favicon-size);
    }

    #progressSpinner {
      -webkit-mask:
          url(chrome://resources/images/throbber_small.svg)
          center/contain no-repeat;
      display: none;
    }

    #favicon {
      background-size: contain;
      transition: border-radius var(--tabstrip-tab-transition-duration);
    }

    #crashedIcon {
      -webkit-mask:
          url(chrome://theme/IDR_CRASH_SAD_FAVICON@2x)
          center/contain no-repeat;
      background-color: currentColor;
      opacity: 0;
      transform: translate(-50%, 100%);
    }

    #blocked {
      background: var(--color-web-ui-tab-strip-tab-blocked);
      border: solid 1px var(--color-web-ui-tab-strip-tab-background);
      border-radius: 50%;
      bottom: 0;
      display: none;
      height: 6px;
      position: absolute;
      right: 0;
      transform: translate(50%, 50%);
      width: 6px;
    }

    :host([waiting_]) #progressSpinner,
    :host([loading_]) #progressSpinner {
      display: block;
    }

    :host([loading_]) #favicon {
      border-radius: 50%;
      height: calc(var(--favicon-size) - 4px);
      overflow: hidden;
      width: calc(var(--favicon-size) - 4px);
    }

    :host([waiting_]) #progressSpinner {
      background-color: var(--color-web-ui-tab-strip-tab-waiting-spinning);
      transform: translate(-50%, -50%) scaleX(-1); /* Center first, then flip horizontally. */
    }

    :host([active][waiting_]) #progressSpinner {
      background-color: var(--color-web-ui-tab-strip-tab-active-title-content);
    }

    :host([waiting_]) #favicon {
      display: none;
    }

    :host([loading_]) #progressSpinner {
      background-color: var(--color-web-ui-tab-strip-tab-loading-spinning);
    }

    :host([active][loading_]) #progressSpinner {
      background-color: var(--color-web-ui-tab-strip-tab-active-title-content);
    }

    :host([crashed_]) #favicon {
      opacity: 0;
      transform: translate(-50%, 100%);
      transition:
          opacity var(--tabstrip-tab-transition-duration),
          transform var(--tabstrip-tab-transition-duration);
    }

    :host([crashed_]) #crashedIcon {
      opacity: 1;
      transform: translate(-50%, -50%);
      transition:
          opacity var(--tabstrip-tab-transition-duration),
          transform var(--tabstrip-tab-transition-duration);
      /* Wait until transition for #favicon finishes. */
      transition-delay: var(--tabstrip-tab-transition-duration);
    }

    :host([blocked_]) #blocked {
      display: block;
    }

    :host([active][blocked_]) #blocked {
      display: none;
    }

    /* Suffix: fixed width, right side */
    #alertIndicators {
      flex: 0 0 auto;
      width: auto; /* adjust as needed */
      display: flex;
      justify-content: flex-end;
      align-items: center;
      margin: 2px;
      --indicator-color: var(--color-web-ui-tab-strip-tab-text);
    }

    :host(:not([has-alert-states_])) #alertIndicators {
      display: none;
    }
  `;

export function getHtml(this: TabItem) {
  const item_size = CrUrlListItemSize.COMPACT;
  return html`
<div id="container">
  <cr-url-list-item-ex
    size="${item_size}"
    tabindex="0"
    title="${this.title}"
    only_icon=${this.item_mode === "icon"}
    description="${item_size !== CrUrlListItemSize.COMPACT ? this.domain : ''}"
    description-meta="${item_size !== CrUrlListItemSize.COMPACT && this.openedCount > 0 ? this.openedCount.toString() : ''}"
    @mouseover=${this.onMouseOver_}
    @mouseenter=${this.onMouseEnter_}
    @mouseleave=${this.onMouseLeave_}
    @click=${this.onActivate_}
    @contextmenu=${this.onContextMenu_}
  >
    ${html`
      <div id="faviconContainer" aria-hidden="true" slot="prefix">
        <div id="progressSpinner"></div>
        <div id="favicon"></div>
        <div id="crashedIcon"></div>
        <div id="blocked"></div>
      </div>
    `}
    <div class="badge" slot="badges">
      <tabstrip-alert-indicators id="alertIndicators"></tabstrip-alert-indicators>
    </div>
    <cr-icon-button iron-icon="cr:close" slot="suffix" @click=${this.onClose_}>
    </cr-icon-button>
    <div id="preview-thumbnail" slot="footer" aria-hidden="true">
      ${this.thumbnailUrl
        ? html`<img
                  src=${this.thumbnailUrl}
                  alt=""
                  decoding="async"
                  loading="lazy"
                  draggable="false"
                  @error=${() => { this.thumbnailUrl = null; }}
                >`
        : nothing}
    </div>
  </cr-url-list-item-ex>
</div>
`;
}
