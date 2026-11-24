// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

import {CrLitElement, css, html, type CSSResultGroup} from 'chrome://resources/lit/v3_0/lit.rollup.js';

import {bootstrapRuntimeHost} from './nexus_runtime_bootstrap.js';

// Query parameter that marks the profile's runtime host page.
const RUNTIME_HOST_PAGE_PARAM = 'nexusRuntimeHost';

// Whether this document is the runtime host page: the top-chrome page no
// window shows, whose one guest holds the runtime's process. (P-2)
export function isRuntimeHostPage(search: string = window.location.search):
    boolean {
  return new URLSearchParams(search).get(RUNTIME_HOST_PAGE_PARAM) === '1';
}

// The runtime host page's whole content: one guest of the "nexus" partition
// without the name that marks a window's guest, so it joins the partition's
// runtime process group.
export class NexusRuntimeHostElement extends CrLitElement {
  static override styles: CSSResultGroup = css`
    :host,
    webview {
      display: block;
      width: 100%;
      height: 100%;
    }
  `;

  override firstUpdated() {
    bootstrapRuntimeHost(
        this.shadowRoot?.getElementById('nexusRuntimeHost') ?? null);
  }

  protected override render() {
    return html`
      <webview id="nexusRuntimeHost" partition="persist:nexus"></webview>
    `;
  }
}

customElements.define('nexus-runtime-host', NexusRuntimeHostElement);
