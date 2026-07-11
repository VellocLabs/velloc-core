// ---------------------------------------------------------------------------
// Browser Actor Tools — Element Registry
// ---------------------------------------------------------------------------
// Maps synthetic auto-incrementing element IDs to real Chromium identifiers
// (tab_id, node_id, document_id). The LLM only sees the synthetic ID;
// the registry resolves it to real targets when actions are executed.
//
// One registry instance per session. The caller (web app adapter) creates
// it and passes it to the tool executor.
// ---------------------------------------------------------------------------

import type { FormControlInfo, PageElement } from './types.js'

const INITIAL_ELEMENT_ID = 1000

export type ElementRegistryEntry = {
  tab_id: number
  node_id: number
  document_id: string
  type: string
  text?: string
  title?: string
  label?: string
  form_control?: FormControlInfo
  is_clickable?: boolean
  is_focusable?: boolean
  anchor_href?: string
}

export type MappedElement = Omit<PageElement, 'node_id'> & {
  element_id: number
}

export class ElementRegistry {
  private nextId = INITIAL_ELEMENT_ID
  private map = new Map<number, ElementRegistryEntry>()

  /**
   * Register elements from an observation and return them with synthetic IDs.
   * Each element gets a new globally-unique auto-incrementing integer ID.
   * Old IDs from previous observations remain valid until `clear()`.
   */
  registerObservation(
    tabId: number,
    documentId: string,
    elements: PageElement[]
  ): MappedElement[] {
    return elements.map((el) => {
      const elementId = this.nextId++
      this.map.set(elementId, {
        tab_id: tabId,
        node_id: el.node_id,
        document_id: documentId,
        type: el.type,
        text: el.text,
        title: el.title,
        label: el.label,
        form_control: el.form_control,
        is_clickable: el.is_clickable,
        is_focusable: el.is_focusable,
        anchor_href: el.anchor_href,
      })

      const { node_id: _nodeId, ...rest } = el
      return { ...rest, element_id: elementId }
    })
  }

  /**
   * Resolve a synthetic element ID to the real Chromium identifiers.
   * Throws if the ID is not found — the error message instructs the
   * LLM to use element_ids from the most recent observation.
   */
  resolve(elementId: number): ElementRegistryEntry {
    const entry = this.map.get(elementId)
    if (!entry) {
      throw new Error(
        `INVALID_ELEMENT_ID: element_id ${elementId} not found. Use element_id values from the most recent tool result observation.`
      )
    }
    return entry
  }

  /** Clear all mappings and reset the ID counter. */
  clear(): void {
    this.map.clear()
    this.nextId = INITIAL_ELEMENT_ID
  }

  /** Number of currently registered elements. */
  get size(): number {
    return this.map.size
  }
}
