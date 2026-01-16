// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {CrUrlListItemElement} from '//resources/cr_elements/cr_url_list_item/cr_url_list_item.js';
import {css} from '//resources/lit/v3_0/lit.rollup.js';


export class CrUrlListItemElementEx extends CrUrlListItemElement {
  static override get is() {
    return 'cr-url-list-item-ex';
  }
  static override get properties() {
    return {
      ...super.properties,
      only_icon: {type: Boolean, reflect: true},
    };
  }

  accessor only_icon: boolean = false;
  constructor() {
    super();
    this.imageUrls = [];
  }
  static override get styles() {
    return [ super.styles, css`
      :host {
        --cr-icon-button-hover-background-color: var(--app-bg);
        --cr-hover-background-color: transparent;
        --cr-active-background-color: transparent;
      }

      #iconContainer {
        display: none;
      }

      .title {
        font-weight: 400;
      }
      
      :host([only_icon=true]) #content,
      :host([only_icon=true]) #metadata,
      :host([only_icon=true]) .suffix {
        display: none;
      }

      :host([only_icon=true]) #item {
        justify-content: center;
        --cr-url-list-item-padding: 0;
      }

      :host([only_icon=true]) ::slotted([slot=prefix]) {
        margin-inline: 0;
      }
    `];
  }

  private backupTitle: string = '';

  override render() {
    // Ensure imageUrls is always an array to avoid undefined access in the base
    // template when slots are not providing images.
    if (this.title != '') {
      this.backupTitle = this.title;
    } else {
      this.title = this.backupTitle;
    }
    let ret = super.render();
    this.title = '';
    return ret;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'cr-url-list-item-ex': CrUrlListItemElementEx;
  }
}

customElements.define(CrUrlListItemElementEx.is, CrUrlListItemElementEx);
