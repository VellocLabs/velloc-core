// ---------------------------------------------------------------------------
// Browser Actor Tools — Observation Parser
// ---------------------------------------------------------------------------
// Parses the AnnotatedPageContent (APC) proto from TabObservation into a
// flat list of PageElements that the LLM can reason about.
//
// The APC is a recursive ContentNode tree. This parser walks the tree,
// extracts actionable information (node IDs, text, geometry, form controls,
// roles), and returns a flat array ordered by document position.
//
// Proto field numbers match:
//   components/optimization_guide/proto/features/common_quality_data.proto
// ---------------------------------------------------------------------------

import { ProtoReader } from './protoCodec.js'
import type { ElementRegistry, MappedElement } from './elementRegistry.js'
import type {
  BoundingBox,
  ElementFilterConfig,
  FormControlInfo,
  FrameInfo,
  ObservationConfig,
  PageElement,
  PageObservation,
  SelectOptionInfo,
  SimpleObservation,
  SimpleMappedElement,
} from './types.js'
import {
  ANNOTATED_ROLES,
  CONTENT_ATTRIBUTE_TYPES,
  FORM_CONTROL_TYPES,
} from './types.js'

// =========================================================================
//  Proto field-number constants (common_quality_data.proto)
// =========================================================================

const APC = {
  VERSION: 1,
  ROOT_NODE: 2,
  MAIN_FRAME_DATA: 3,
  VIEWPORT_GEOMETRY: 5,
  TAB_ID: 7,
} as const

const CONTENT_NODE = { CHILDREN: 1, ATTRIBUTES: 2 } as const

const ATTRS = {
  TEXT_DATA: 10,
  IMAGE_DATA: 11,
  ANCHOR_DATA: 14,
  FORM_CONTROL_DATA: 20,
  COMMON_ANCESTOR_DOM_NODE_ID: 2,
  ATTRIBUTE_TYPE: 3,
  GEOMETRY: 4,
  INTERACTION_INFO: 21,
  ANNOTATED_ROLES: 15,
  LABEL: 16,
  ARIA_ROLE: 26,
} as const

const TEXT_INFO = { TEXT_CONTENT: 1 } as const

const ANCHOR_INFO = { URL: 1, REL: 2 } as const

const FORM_CTRL = {
  FIELD_NAME: 1,
  FIELD_VALUE: 2,
  FORM_CONTROL_TYPE: 3,
  SELECT_OPTIONS: 4,
  PLACEHOLDER: 5,
  IS_CHECKED: 6,
  IS_REQUIRED: 7,
} as const

const SELECT_OPT = { VALUE: 1, TEXT: 2 } as const

const GEOM = {
  OUTER_BBOX: 1,
  VISIBLE_BBOX: 2,
} as const

const BBOX = { X: 1, Y: 2, WIDTH: 3, HEIGHT: 4 } as const

const INTERACTION = {
  IS_FOCUSABLE: 7,
  CLICKABILITY_REASONS: 15,
  IS_DISABLED: 16,
} as const

const FRAME_DATA = {
  DOC_IDENTIFIER: 3,
  URL: 4,
  TITLE: 5,
} as const

const DOC_ID = { SERIALIZED_TOKEN: 1 } as const

const META_TAG = { NAME: 1, CONTENT: 2 } as const
const FRAME_META = { URL: 1, META_TAGS: 2 } as const
const PAGE_META = { FRAME_METADATA: 1 } as const

// TabObservation fields
const TAB_OBS = {
  ID: 1,
  APC: 2,
  SCREENSHOT: 3,
  SCREENSHOT_MIME: 4,
  RESULT: 5,
  METADATA: 6,
} as const

// =========================================================================
//  Primitive parsers
// =========================================================================

function parseBoundingBox(r: ProtoReader): BoundingBox {
  const box: BoundingBox = { x: 0, y: 0, width: 0, height: 0 }
  while (r.hasMore()) {
    const tag = r.readTag()
    if (!tag) break
    switch (tag.field) {
      case BBOX.X:
        box.x = r.readInt32()
        break
      case BBOX.Y:
        box.y = r.readInt32()
        break
      case BBOX.WIDTH:
        box.width = r.readInt32()
        break
      case BBOX.HEIGHT:
        box.height = r.readInt32()
        break
      default:
        r.skip(tag.wireType)
    }
  }
  return box
}

function parseSelectOption(r: ProtoReader): SelectOptionInfo {
  const opt: SelectOptionInfo = { value: '', text: '' }
  while (r.hasMore()) {
    const tag = r.readTag()
    if (!tag) break
    switch (tag.field) {
      case SELECT_OPT.VALUE:
        opt.value = r.readString()
        break
      case SELECT_OPT.TEXT:
        opt.text = r.readString()
        break
      default:
        r.skip(tag.wireType)
    }
  }
  return opt
}

function parseFormControlData(r: ProtoReader): FormControlInfo {
  const fc: FormControlInfo = { field_name: '', field_value: '', control_type: 'unspecified' }
  while (r.hasMore()) {
    const tag = r.readTag()
    if (!tag) break
    switch (tag.field) {
      case FORM_CTRL.FIELD_NAME:
        fc.field_name = r.readString()
        break
      case FORM_CTRL.FIELD_VALUE:
        fc.field_value = r.readString()
        break
      case FORM_CTRL.FORM_CONTROL_TYPE:
        fc.control_type = FORM_CONTROL_TYPES[r.readUint32()] ?? 'unspecified'
        break
      case FORM_CTRL.SELECT_OPTIONS: {
        if (!fc.options) fc.options = []
        fc.options.push(parseSelectOption(r.readMessage()))
        break
      }
      case FORM_CTRL.PLACEHOLDER:
        fc.placeholder = r.readString()
        break
      case FORM_CTRL.IS_CHECKED:
        fc.is_checked = r.readBool()
        break
      case FORM_CTRL.IS_REQUIRED:
        fc.is_required = r.readBool()
        break
      default:
        r.skip(tag.wireType)
    }
  }
  return fc
}

// =========================================================================
//  ContentNode → PageElement[] (recursive flattening)
// =========================================================================

function parseGeometry(r: ProtoReader): { bbox?: BoundingBox; visible_bbox?: BoundingBox } {
  let bbox: BoundingBox | undefined
  let visible_bbox: BoundingBox | undefined
  while (r.hasMore()) {
    const tag = r.readTag()
    if (!tag) break
    switch (tag.field) {
      case GEOM.OUTER_BBOX:
        bbox = parseBoundingBox(r.readMessage())
        break
      case GEOM.VISIBLE_BBOX:
        visible_bbox = parseBoundingBox(r.readMessage())
        break
      default:
        r.skip(tag.wireType)
    }
  }
  return { bbox, visible_bbox }
}

function parseInteractionInfo(
  r: ProtoReader
): { is_focusable?: boolean; is_disabled?: boolean; is_clickable?: boolean } {
  let is_focusable: boolean | undefined
  let is_disabled: boolean | undefined
  let clickability_count = 0
  while (r.hasMore()) {
    const tag = r.readTag()
    if (!tag) break
    switch (tag.field) {
      case INTERACTION.IS_FOCUSABLE:
        is_focusable = r.readBool()
        break
      case INTERACTION.IS_DISABLED:
        is_disabled = r.readBool()
        break
      case INTERACTION.CLICKABILITY_REASONS:
        if (tag.wireType === 2) {
          const packed = r.readMessage()
          while (packed.hasMore()) {
            packed.readUint32()
            clickability_count++
          }
        } else {
          r.readUint32()
          clickability_count++
        }
        break
      default:
        r.skip(tag.wireType)
    }
  }
  return {
    is_focusable: is_focusable || undefined,
    is_disabled: is_disabled || undefined,
    is_clickable: clickability_count > 0 ? true : undefined,
  }
}

function parseContentAttributes(r: ProtoReader): Omit<PageElement, 'children_count'> {
  const el: Omit<PageElement, 'children_count'> = { node_id: 0, type: 'unknown' }

  while (r.hasMore()) {
    const tag = r.readTag()
    if (!tag) break
    switch (tag.field) {
      case ATTRS.COMMON_ANCESTOR_DOM_NODE_ID:
        el.node_id = r.readInt32()
        break
      case ATTRS.ATTRIBUTE_TYPE:
        el.type = CONTENT_ATTRIBUTE_TYPES[r.readUint32()] ?? 'unknown'
        break
      case ATTRS.LABEL:
        el.label = r.readString()
        break
      case ATTRS.ARIA_ROLE:
        el.aria_role = String(r.readUint32())
        break
      case ATTRS.ANNOTATED_ROLES: {
        if (tag.wireType === 2) {
          const packed = r.readMessage()
          while (packed.hasMore()) {
            const roleVal = packed.readUint32()
            const roleName = ANNOTATED_ROLES[roleVal]
            if (roleName && roleName !== 'unknown') {
              if (!el.roles) el.roles = []
              el.roles.push(roleName)
            }
          }
        } else {
          const roleVal = r.readUint32()
          const roleName = ANNOTATED_ROLES[roleVal]
          if (roleName && roleName !== 'unknown') {
            if (!el.roles) el.roles = []
            el.roles.push(roleName)
          }
        }
        break
      }
      case ATTRS.GEOMETRY: {
        const geom = parseGeometry(r.readMessage())
        el.bbox = geom.bbox
        el.visible_bbox = geom.visible_bbox
        break
      }
      case ATTRS.INTERACTION_INFO: {
        const info = parseInteractionInfo(r.readMessage())
        el.is_focusable = info.is_focusable
        el.is_disabled = info.is_disabled
        el.is_clickable = info.is_clickable
        break
      }
      case ATTRS.TEXT_DATA: {
        const textReader = r.readMessage()
        while (textReader.hasMore()) {
          const textTag = textReader.readTag()
          if (!textTag) break
          if (textTag.field === TEXT_INFO.TEXT_CONTENT) {
            el.text = textReader.readString()
          } else {
            textReader.skip(textTag.wireType)
          }
        }
        break
      }
      case ATTRS.ANCHOR_DATA: {
        const anchorReader = r.readMessage()
        while (anchorReader.hasMore()) {
          const anchorTag = anchorReader.readTag()
          if (!anchorTag) break
          if (anchorTag.field === ANCHOR_INFO.URL) {
            el.anchor_href = anchorReader.readString()
          } else {
            anchorReader.skip(anchorTag.wireType)
          }
        }
        break
      }
      case ATTRS.FORM_CONTROL_DATA:
        el.form_control = parseFormControlData(r.readMessage())
        break
      default:
        r.skip(tag.wireType)
    }
  }

  return el
}

/**
 * Recursively walk a ContentNode and flatten into a list of PageElements.
 * Nodes without a node_id (e.g. pseudo-elements) are skipped unless they
 * carry text content.
 */
function flattenContentNode(r: ProtoReader, out: PageElement[]): number {
  let childCount = 0
  let attrs: Omit<PageElement, 'children_count'> | undefined

  while (r.hasMore()) {
    const tag = r.readTag()
    if (!tag) break
    switch (tag.field) {
      case CONTENT_NODE.CHILDREN: {
        childCount += flattenContentNode(r.readMessage(), out)
        break
      }
      case CONTENT_NODE.ATTRIBUTES:
        attrs = parseContentAttributes(r.readMessage())
        break
      default:
        r.skip(tag.wireType)
    }
  }

  if (attrs && (attrs.node_id !== 0 || attrs.text)) {
    const element: PageElement = { ...attrs }
    if (childCount > 0) element.children_count = childCount
    out.push(element)
    return 1
  }

  return childCount > 0 ? childCount : 0
}

function normalizeObservationText(value: string | undefined): string | undefined {
  if (typeof value !== 'string') {
    return undefined
  }

  const normalized = value.replace(/\s+/g, ' ').trim()
  return normalized.length > 0 ? normalized : undefined
}

function appendUniqueTextPart(
  parts: string[],
  seen: Set<string>,
  value: string | undefined
): void {
  const normalized = normalizeObservationText(value)
  if (!normalized) {
    return
  }

  const dedupeKey = normalized.toLowerCase()
  if (seen.has(dedupeKey)) {
    return
  }

  seen.add(dedupeKey)
  parts.push(normalized)
}

function joinTextParts(parts: string[], maxLength = 120): string | undefined {
  if (parts.length === 0) {
    return undefined
  }

  let out = ''
  for (const part of parts) {
    const candidate = out ? `${out} ${part}` : part
    if (candidate.length <= maxLength) {
      out = candidate
      continue
    }

    if (!out) {
      return `${part.slice(0, Math.max(0, maxLength - 1)).trim()}…`
    }

    return `${out}…`
  }

  return out || undefined
}

function getElementDescendants(
  elements: readonly PageElement[],
  index: number
): readonly PageElement[] {
  const childCount = elements[index]?.children_count
  if (typeof childCount !== 'number' || childCount <= 0) {
    return []
  }

  const start = Math.max(0, index - childCount)
  return elements.slice(start, index)
}

/**
 * Precomputes the immediate-parent index for every element in a single O(n)
 * pass. Returns an Int32Array where `parents[i]` is the index of the nearest
 * ancestor of element `i`, or `-1` if `i` has no ancestor in the list.
 *
 * The APC tree is serialized in post-order, so for an element `i` with
 * descendant count `k` its subtree spans `[i - k, i]` (inclusive). Direct
 * children are the roots of that span — found by walking backwards from
 * `i - 1` and jumping over each child's own subtree.
 *
 * Callers that run many ancestor queries over the same observation should
 * build this index once and reuse it (see `findEnclosingElementIndex`).
 */
export function buildParentIndex(
  elements: readonly PageElement[]
): Int32Array {
  const parents = new Int32Array(elements.length).fill(-1)
  for (let i = 0; i < elements.length; i++) {
    const childCount = elements[i]?.children_count ?? 0
    let cursor = i - 1
    let remaining = childCount
    while (remaining > 0 && cursor >= 0) {
      parents[cursor] = i
      const subtreeSize = (elements[cursor]?.children_count ?? 0) + 1
      cursor -= subtreeSize
      remaining -= subtreeSize
    }
  }
  return parents
}

/**
 * Walk from a target index outward to find the smallest enclosing element
 * that satisfies `predicate`. Returns its index, or -1 if none match.
 *
 * Uses a parent-index precomputed by `buildParentIndex`, so each call is
 * O(ancestor depth) rather than O(n). The previous implementation scanned
 * every index `> target` and tested `getElementSubtreeRange` containment,
 * which made bulk callers (e.g. `browser_find_text`) O(n²) in the element
 * count. Common predicates: "is clickable / focusable / has anchor_href /
 * is form_control".
 */
export function findEnclosingElementIndex(
  elements: readonly PageElement[],
  parents: Int32Array,
  index: number,
  predicate: (e: PageElement) => boolean
): number {
  let cur = parents[index] ?? -1
  while (cur !== -1) {
    if (predicate(elements[cur]!)) {
      return cur
    }
    cur = parents[cur] ?? -1
  }
  return -1
}

/**
 * Flatten the text content of an element and its descendants into a single
 * string. Uses the same dedup/normalize rules as the existing descriptor
 * logic so we don't repeat the same "Search" label five times for nested
 * <span><span><span>Search</span></span></span> structures.
 *
 * Caps the return value at maxChars (with a trailing "…" if truncated).
 */
export function collectElementText(
  elements: readonly PageElement[],
  index: number,
  maxChars: number
): { text: string; truncated: boolean } {
  if (index < 0 || index >= elements.length) {
    return { text: '', truncated: false }
  }
  const element = elements[index]!
  const descendants = getElementDescendants(elements, index)
  const seen = new Set<string>()
  const parts: string[] = []

  appendUniqueTextPart(parts, seen, element.text)
  appendUniqueTextPart(parts, seen, element.label)
  appendUniqueTextPart(parts, seen, element.title)
  for (const d of descendants) {
    appendUniqueTextPart(parts, seen, d.text)
    appendUniqueTextPart(parts, seen, d.label)
  }

  const joined = parts.join(' ').trim()
  if (joined.length <= maxChars) {
    return { text: joined, truncated: false }
  }
  return {
    text: joined.slice(0, Math.max(0, maxChars - 1)).trim() + '…',
    truncated: true,
  }
}

/**
 * Walk the whole element list and join the visible text of every element
 * (text + label + title), deduplicated by lowercase form. Suitable for
 * "give me a plain-text dump of the page" requests.
 *
 * When `viewportOnly` is true, only elements with a non-empty visible_bbox
 * are considered — that mirrors what's actually drawn to the user.
 *
 * Caps at maxChars; sets truncated=true when content is dropped.
 */
export function joinPageVisibleText(
  elements: readonly PageElement[],
  options: { maxChars: number; viewportOnly: boolean }
): { text: string; truncated: boolean } {
  const seen = new Set<string>()
  const parts: string[] = []
  for (const e of elements) {
    if (options.viewportOnly) {
      const bb = e.visible_bbox
      if (!bb || bb.width <= 0 || bb.height <= 0) continue
    }
    appendUniqueTextPart(parts, seen, e.text)
    appendUniqueTextPart(parts, seen, e.label)
    appendUniqueTextPart(parts, seen, e.title)
    appendUniqueTextPart(parts, seen, e.form_control?.placeholder)
    appendUniqueTextPart(parts, seen, e.form_control?.field_value)
  }
  const joined = parts.join('\n')
  if (joined.length <= options.maxChars) {
    return { text: joined, truncated: false }
  }
  return {
    text: joined.slice(0, Math.max(0, options.maxChars - 1)).trim() + '…',
    truncated: true,
  }
}

function collectVisibleText(elements: readonly PageElement[]): string | undefined {
  const parts: string[] = []
  const seen = new Set<string>()

  for (const element of elements) {
    appendUniqueTextPart(parts, seen, element.text)
    if (!normalizeObservationText(element.text)) {
      appendUniqueTextPart(parts, seen, element.label)
    }
  }

  return joinTextParts(parts)
}

function collectDescriptorText(
  elements: readonly PageElement[]
): string | undefined {
  const parts: string[] = []
  const seen = new Set<string>()

  for (const element of elements) {
    appendUniqueTextPart(parts, seen, element.label)
    appendUniqueTextPart(parts, seen, element.text)
    appendUniqueTextPart(parts, seen, element.form_control?.field_name)
    appendUniqueTextPart(parts, seen, element.form_control?.placeholder)
  }

  return joinTextParts(parts)
}

function enrichElementDescriptors(elements: PageElement[]): PageElement[] {
  return elements.map((element, index) => {
    const descendants = getElementDescendants(elements, index)
    const next: PageElement = { ...element }
    const ownText = normalizeObservationText(element.text)

    if (
      !ownText &&
      descendants.length > 0 &&
      (isInteractive(element) || !!element.anchor_href || element.type === 'anchor')
    ) {
      const derivedVisibleText = collectVisibleText(descendants)
      if (derivedVisibleText) {
        next.text = derivedVisibleText
      }
    }

    const normalizedLabel = normalizeObservationText(next.label)
    const normalizedText = normalizeObservationText(next.text)
    const descriptorText =
      collectDescriptorText([next]) ?? collectDescriptorText(descendants)
    if (
      descriptorText &&
      descriptorText !== normalizedLabel &&
      descriptorText !== normalizedText
    ) {
      next.title = descriptorText
    }

    return next
  })
}

// =========================================================================
//  AnnotatedPageContent parser
// =========================================================================

type ApcParseResult = {
  elements: PageElement[]
  frame?: FrameInfo
  viewport?: BoundingBox
  tab_id?: number
}

function parseFrameData(r: ProtoReader): FrameInfo {
  const frame: FrameInfo = { url: '', title: '', document_id: '' }
  while (r.hasMore()) {
    const tag = r.readTag()
    if (!tag) break
    switch (tag.field) {
      case FRAME_DATA.URL:
        frame.url = r.readString()
        break
      case FRAME_DATA.TITLE:
        frame.title = r.readString()
        break
      case FRAME_DATA.DOC_IDENTIFIER: {
        const docReader = r.readMessage()
        while (docReader.hasMore()) {
          const docTag = docReader.readTag()
          if (!docTag) break
          if (docTag.field === DOC_ID.SERIALIZED_TOKEN) {
            frame.document_id = docReader.readString()
          } else {
            docReader.skip(docTag.wireType)
          }
        }
        break
      }
      default:
        r.skip(tag.wireType)
    }
  }
  return frame
}

export function parseAnnotatedPageContent(r: ProtoReader): ApcParseResult {
  const result: ApcParseResult = { elements: [] }

  while (r.hasMore()) {
    const tag = r.readTag()
    if (!tag) break
    switch (tag.field) {
      case APC.ROOT_NODE:
        flattenContentNode(r.readMessage(), result.elements)
        break
      case APC.MAIN_FRAME_DATA:
        result.frame = parseFrameData(r.readMessage())
        break
      case APC.VIEWPORT_GEOMETRY:
        result.viewport = parseBoundingBox(r.readMessage())
        break
      case APC.TAB_ID:
        result.tab_id = r.readInt32()
        break
      default:
        r.skip(tag.wireType)
    }
  }

  result.elements = enrichElementDescriptors(result.elements)
  return result
}

// =========================================================================
//  PageMetadata parser
// =========================================================================

function parseMetaTag(r: ProtoReader): { name: string; content: string } {
  const meta = { name: '', content: '' }
  while (r.hasMore()) {
    const tag = r.readTag()
    if (!tag) break
    switch (tag.field) {
      case META_TAG.NAME:
        meta.name = r.readString()
        break
      case META_TAG.CONTENT:
        meta.content = r.readString()
        break
      default:
        r.skip(tag.wireType)
    }
  }
  return meta
}

export function parsePageMetadata(
  r: ProtoReader
): Array<{ name: string; content: string }> {
  const metas: Array<{ name: string; content: string }> = []

  while (r.hasMore()) {
    const tag = r.readTag()
    if (!tag) break
    if (tag.field === PAGE_META.FRAME_METADATA) {
      const frameReader = r.readMessage()
      while (frameReader.hasMore()) {
        const fTag = frameReader.readTag()
        if (!fTag) break
        if (fTag.field === FRAME_META.META_TAGS) {
          metas.push(parseMetaTag(frameReader.readMessage()))
        } else {
          frameReader.skip(fTag.wireType)
        }
      }
    } else {
      r.skip(tag.wireType)
    }
  }

  return metas
}

// =========================================================================
//  TabObservation → PageObservation (full parse)
// =========================================================================

/**
 * Parse a single TabObservation proto message into a PageObservation,
 * including APC (DOM elements), screenshot, and metadata.
 */
export function parseTabObservationFull(r: ProtoReader): PageObservation {
  const obs: PageObservation = { tab_id: 0, elements: [] }
  let tabResult = 0
  let screenshotBytes: Uint8Array | undefined
  let screenshotMime: string | undefined

  while (r.hasMore()) {
    const tag = r.readTag()
    if (!tag) break
    switch (tag.field) {
      case TAB_OBS.ID:
        obs.tab_id = r.readInt32()
        break
      case TAB_OBS.APC: {
        const apc = parseAnnotatedPageContent(r.readMessage())
        obs.elements = apc.elements
        obs.frame = apc.frame
        obs.viewport = apc.viewport
        break
      }
      case TAB_OBS.SCREENSHOT:
        screenshotBytes = r.readBytes()
        break
      case TAB_OBS.SCREENSHOT_MIME:
        screenshotMime = r.readString()
        break
      case TAB_OBS.RESULT:
        tabResult = r.readUint32()
        break
      case TAB_OBS.METADATA:
        obs.metadata = parsePageMetadata(r.readMessage())
        break
      default:
        r.skip(tag.wireType)
    }
  }

  if (screenshotBytes) {
    obs.screenshot = {
      data_base64: uint8ArrayToBase64(screenshotBytes),
      mime_type: screenshotMime ?? 'image/jpeg',
    }
  }

  if (tabResult !== 0) {
    obs.elements = []
    obs.frame = undefined
  }

  return obs
}

// =========================================================================
//  Element filtering & compression
// =========================================================================

function isInteractive(e: PageElement): boolean {
  return !!(e.is_clickable || e.is_focusable || e.form_control)
}

function hasNonEmptyText(e: PageElement): boolean {
  return !!(
    (e.text && e.text.trim().length > 0) ||
    (e.label && e.label.trim().length > 0)
  )
}

function isIdentifiableInteractive(e: PageElement): boolean {
  if (!isInteractive(e)) return false
  if (e.text && e.text.trim().length > 0) return true
  if (e.title && e.title.trim().length > 0) return true
  if (e.label && e.label.trim().length > 0) return true
  if (e.form_control) return true
  if (e.anchor_href) return true
  return false
}

function hasVisibleBounds(e: PageElement): boolean {
  const bb = e.visible_bbox ?? e.bbox
  return bb != null && bb.width > 0 && bb.height > 0
}

export function isActionable(e: PageElement): boolean {
  return !!(
    e.is_clickable ||
    e.is_focusable ||
    e.form_control ||
    e.anchor_href
  )
}

/**
 * Collapse whitespace runs and trim `text`, `title`, `label` on every element.
 * Drops the field entirely if it normalizes to empty. This kills
 * verbose-whitespace text dumps like "\n   新闻 \n   ..." and makes downstream
 * filters / dedup honest by comparing against canonical strings.
 */
function compactWhitespaceFields(elements: PageElement[]): PageElement[] {
  return elements.map((e) => {
    const text = normalizeObservationText(e.text)
    const title = normalizeObservationText(e.title)
    const label = normalizeObservationText(e.label)

    // Bail out if nothing changed — keeps allocations down on hot pages.
    if (text === e.text && title === e.title && label === e.label) {
      return e
    }

    const next: PageElement = { ...e }
    if (text) next.text = text
    else delete next.text
    if (title) next.title = title
    else delete next.title
    if (label) next.label = label
    else delete next.label
    return next
  })
}

/**
 * Drop `type:"text"` elements that carry no usable content and no
 * interactivity — pure-whitespace placeholders, decorative spans, etc. After
 * compactWhitespaceFields these have no `text`/`title`/`label` at all.
 */
function dropNoiseTextElements(elements: PageElement[]): PageElement[] {
  return elements.filter((e) => {
    if (e.type !== 'text') return true
    if (isActionable(e)) return true
    return !!(e.text || e.title || e.label)
  })
}

/**
 * Drop `type:"text"` non-actionable elements whose text duplicates an
 * actionable element's text/label/title. Common pattern on sites like
 * Baidu where every top-nav anchor is shadowed by a sibling text node
 * carrying the same string. The model already sees the actionable, so
 * the bare text is redundant noise.
 */
function dedupTextNearActionables(elements: PageElement[]): PageElement[] {
  const actionableTexts = new Set<string>()
  for (const e of elements) {
    if (!isActionable(e)) continue
    if (e.text) actionableTexts.add(e.text)
    if (e.label) actionableTexts.add(e.label)
    if (e.title) actionableTexts.add(e.title)
  }
  if (actionableTexts.size === 0) return elements

  return elements.filter((e) => {
    if (e.type !== 'text') return true
    if (isActionable(e)) return true
    if (e.text && actionableTexts.has(e.text)) return false
    return true
  })
}

/**
 * Apply adapter-layer filtering to a raw element list.
 * Called by observationToToolResponse and available for custom pipelines.
 */
export function filterElements(
  elements: PageElement[],
  config?: ElementFilterConfig
): PageElement[] {
  if (!config) return elements

  let filtered = elements.filter((e) => e.node_id !== 0)

  if (config.excludeTypes && config.excludeTypes.length > 0) {
    const excluded = new Set(config.excludeTypes)
    filtered = filtered.filter((e) => !excluded.has(e.type))
  }

  // Aggressive noise reduction: normalize text fields, then drop empty and
  // duplicate text-only elements before the configured filters run. These
  // passes never touch actionable elements (clickable / focusable / form /
  // anchor) so the model never loses a target it could click.
  filtered = compactWhitespaceFields(filtered)
  filtered = dropNoiseTextElements(filtered)
  filtered = dedupTextNearActionables(filtered)

  if (config.onlyVisible) {
    filtered = filtered.filter(hasVisibleBounds)
  }

  if (config.onlyInteractive) {
    if (config.includeTextElements) {
      filtered = filtered.filter(
        (e) => hasNonEmptyText(e) || isIdentifiableInteractive(e)
      )
    } else {
      filtered = filtered.filter(isInteractive)
    }
  }

  if (config.maxElements != null && filtered.length > config.maxElements) {
    filtered = prioritizeElements(filtered, config.maxElements)
  }

  if (config.maxTextLength != null) {
    const maxTextLength = config.maxTextLength
    filtered = filtered.map((e) => {
      const next = { ...e }
      if (next.text && next.text.length > maxTextLength) {
        next.text = next.text.slice(0, maxTextLength) + '…'
      }
      if (next.title && next.title.length > maxTextLength) {
        next.title = next.title.slice(0, maxTextLength) + '…'
      }
      return next
    })
  }

  return filtered
}

/**
 * Keep the most useful elements when capping count.
 * Priority: form_control > interactive > anchors > everything else.
 */
function prioritizeElements(elements: PageElement[], max: number): PageElement[] {
  const scored = elements.map((e, idx) => {
    let score = 0
    if (e.form_control) score += 100
    if (e.is_clickable) score += 50
    if (e.is_focusable) score += 40
    if (e.anchor_href) score += 30
    if (e.text) score += 10
    if (e.title) score += 8
    score -= idx * 0.001 // Stable sort by document order
    return { e, score }
  })
  scored.sort((a, b) => b.score - a.score)
  const kept = scored.slice(0, max).map((s) => s.e)
  return kept
}

// =========================================================================
//  Utility
// =========================================================================

function uint8ArrayToBase64(bytes: Uint8Array): string {
  let binary = ''
  for (let i = 0; i < bytes.length; i++) {
    const byte = bytes[i]
    if (byte === undefined) continue
    binary += String.fromCharCode(byte)
  }
  return btoa(binary)
}

/**
 * Compress a PageObservation into a token-efficient JSON object for the LLM.
 * Strips empty/undefined fields and abbreviates bounding boxes.
 *
 * @param obs - The parsed page observation.
 * @param config - Optional adapter-level filtering configuration.
 *                 Controls which elements are included and how they're compressed.
 */
export function observationToToolResponse(
  obs: PageObservation,
  config?: ObservationConfig
): Record<string, unknown> {
  const out: Record<string, unknown> = { tab_id: obs.tab_id }

  if (obs.frame) {
    out['url'] = obs.frame.url
    out['title'] = obs.frame.title
    out['document_id'] = obs.frame.document_id
  }

  if (obs.viewport) {
    out['viewport'] = obs.viewport
  }

  const includeBboxes = config?.elements?.includeBoundingBoxes !== false
  const includeFormDetails = config?.elements?.includeFormDetails !== false

  const elements = filterElements(obs.elements, config?.elements)

  if (elements.length > 0) {
    out['elements'] = elements.map((e) => {
      const entry: Record<string, unknown> = { id: e.node_id, type: e.type }
      if (e.text) entry['text'] = e.text
      if (e.title) entry['title'] = e.title
      if (e.label) entry['label'] = e.label
      if (e.roles && e.roles.length > 0) entry['roles'] = e.roles
      if (includeBboxes) {
        if (e.visible_bbox) {
          entry['box'] = [
            e.visible_bbox.x,
            e.visible_bbox.y,
            e.visible_bbox.width,
            e.visible_bbox.height,
          ]
        } else if (e.bbox) {
          entry['box'] = [e.bbox.x, e.bbox.y, e.bbox.width, e.bbox.height]
        }
      }
      if (e.is_clickable) entry['clickable'] = true
      if (e.is_focusable) entry['focusable'] = true
      if (e.is_disabled) entry['disabled'] = true
      if (e.form_control && includeFormDetails) {
        const fc: Record<string, unknown> = {
          type: e.form_control.control_type,
        }
        if (e.form_control.field_name) fc['name'] = e.form_control.field_name
        if (e.form_control.field_value) fc['value'] = e.form_control.field_value
        if (e.form_control.placeholder) fc['placeholder'] = e.form_control.placeholder
        if (e.form_control.is_checked) fc['checked'] = true
        if (e.form_control.is_required) fc['required'] = true
        if (e.form_control.options) {
          fc['options'] = e.form_control.options.map((o) => ({ v: o.value, t: o.text }))
        }
        entry['form'] = fc
      } else if (e.form_control) {
        entry['form'] = { type: e.form_control.control_type }
      }
      if (e.anchor_href) entry['href'] = e.anchor_href
      return entry
    })
  }

  if (obs.screenshot && config?.screenshot?.include !== false) {
    const sizeBytes = obs.screenshot.data_base64.length * 0.75
    const maxSize = config?.screenshot?.maxSizeBytes
    if (maxSize == null || sizeBytes <= maxSize) {
      out['has_screenshot'] = true
      out['screenshot_mime'] = obs.screenshot.mime_type
    } else {
      out['screenshot_skipped'] = 'exceeds_max_size'
    }
  }

  const includeMetadata = config?.includeMetadata !== false
  if (includeMetadata && obs.metadata && obs.metadata.length > 0) {
    out['meta'] = obs.metadata
  }

  return out
}

// =========================================================================
//  Simple mode: PageObservation → SimpleObservation with synthetic IDs
// =========================================================================

/**
 * Convert a MappedElement (from ElementRegistry) into the compact
 * SimpleMappedElement format for LLM consumption.
 */
function toSimpleMappedElement(
  el: MappedElement,
  includeBoxes: boolean,
  includeFormDetails: boolean
): SimpleMappedElement {
  const out: SimpleMappedElement = {
    element_id: el.element_id,
    type: el.type,
  }
  if (el.text) out.text = el.text
  if (el.title) out.title = el.title
  if (el.label) out.label = el.label
  if (el.roles && el.roles.length > 0) out.roles = el.roles
  if (includeBoxes) {
    const bb = el.visible_bbox ?? el.bbox
    if (bb) out.box = [bb.x, bb.y, bb.width, bb.height]
  }
  if (el.is_clickable) out.clickable = true
  if (el.is_focusable) out.focusable = true
  if (el.is_disabled) out.disabled = true
  if (el.form_control && includeFormDetails) {
    out.form = {
      ...el.form_control,
      control_type: el.form_control.control_type,
    }
  } else if (el.form_control) {
    out.form = {
      field_name: '',
      field_value: '',
      control_type: el.form_control.control_type,
    }
  }
  if (el.anchor_href) out.href = el.anchor_href
  return out
}

/**
 * Convert a PageObservation into a SimpleObservation, registering elements
 * in the ElementRegistry and producing synthetic element_id values.
 *
 * @param obs      - The raw page observation from the proto parser.
 * @param registry - The element registry to assign synthetic IDs.
 * @param config   - Optional observation config for element filtering.
 */
export function toSimpleObservation(
  obs: PageObservation,
  registry: ElementRegistry,
  config?: ObservationConfig
): SimpleObservation {
  const filteredElements = filterElements(obs.elements, config?.elements)
  const documentId = obs.frame?.document_id ?? ''
  const mappedElements = registry.registerObservation(
    obs.tab_id,
    documentId,
    filteredElements
  )

  const includeBoxes = config?.elements?.includeBoundingBoxes !== false
  const includeFormDetails = config?.elements?.includeFormDetails !== false

  let hasScreenshot = false
  if (obs.screenshot && config?.screenshot?.include !== false) {
    const maxSize = config?.screenshot?.maxSizeBytes
    if (maxSize == null) {
      hasScreenshot = true
    } else {
      const sizeBytes = obs.screenshot.data_base64.length * 0.75
      hasScreenshot = sizeBytes <= maxSize
    }
  }

  return {
    tab_id: obs.tab_id,
    url: obs.frame?.url ?? '',
    title: obs.frame?.title ?? '',
    viewport: obs.viewport,
    elements: mappedElements.map((el) =>
      toSimpleMappedElement(el, includeBoxes, includeFormDetails)
    ),
    has_screenshot: hasScreenshot,
  }
}
