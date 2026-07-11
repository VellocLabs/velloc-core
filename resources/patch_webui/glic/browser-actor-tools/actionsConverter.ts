// ---------------------------------------------------------------------------
// Browser Actor Tools — Actions Converter
// ---------------------------------------------------------------------------
// Converts LLM tool arguments ↔ protobuf binary for Glic actor APIs.
//
//   buildActionsProto(taskId, actions)  → Uint8Array   (for performActions)
//   buildObserveProto(taskId, tabId?)   → Uint8Array   (wait-only observe)
//   parseActionsResult(buffer)          → BrowserActResult
//
// Proto field numbers match:
//   components/optimization_guide/proto/features/actions_data.proto
//   components/optimization_guide/proto/features/common_quality_data.proto
//   chrome/common/actor.mojom (ActionResultCode)
// ---------------------------------------------------------------------------

import type { ElementRegistry, ElementRegistryEntry } from './elementRegistry.js'
import { ProtoReader, ProtoWriter } from './protoCodec.js'
import {
  filterElements,
  observationToToolResponse,
  parseTabObservationFull,
} from './observationParser.js'
import { SIMPLE_TOOL_NAME_TO_ACTION_TYPE } from './toolDefinitions.js'
import type {
  ActResultConfig,
  ActionTarget,
  BackActionParams,
  BrowserAction,
  BrowserActResult,
  BrowserObserveResult,
  ClickActionParams,
  CloseTabActionParams,
  CreateTabActionParams,
  ForwardActionParams,
  MediaPauseActionParams,
  MediaPlayActionParams,
  MediaSeekActionParams,
  NavigateActionParams,
  ObservationConfig,
  PageObservation,
  ScrollActionParams,
  ScrollToActionParams,
  ScriptResultInfo,
  SelectActionParams,
  TabInfo,
  TypeActionParams,
  WaitActionParams,
  WindowInfo,
} from './types.js'
import { getActionResultName, isElementTarget, TAB_OBSERVATION_RESULTS } from './types.js'

// =========================================================================
//  Proto field-number constants
//  (kept co-located with the encoder so diffs against the .proto are easy)
// =========================================================================

/** actions_data.proto — Actions message */
const ACTIONS = { ACTIONS: 1, TASK_ID: 2, SKIP_ASYNC: 3 } as const

/** actions_data.proto — Action message (oneof field numbers) */
const ACTION = {
  CLICK: 1,
  TYPE: 2,
  SCROLL: 3,
  SELECT: 6,
  NAVIGATE: 7,
  BACK: 8,
  FORWARD: 9,
  WAIT: 10,
  CREATE_TAB: 11,
  CLOSE_TAB: 12,
  ACTIVATE_TAB: 13,
  SCROLL_TO: 19,
  MEDIA_CONTROL: 21,
} as const

/** ClickAction */
const CLICK = { TARGET: 1, CLICK_TYPE: 2, CLICK_COUNT: 3, TAB_ID: 4 } as const
const CLICK_TYPE = { LEFT: 1, RIGHT: 2 } as const
const CLICK_COUNT = { SINGLE: 1, DOUBLE: 2 } as const

/** TypeAction */
const TYPE = { TARGET: 1, TEXT: 2, FOLLOW_BY_ENTER: 3, MODE: 4, TAB_ID: 5 } as const
const TYPE_MODE = { DELETE_EXISTING: 1 } as const

/** ScrollAction */
const SCROLL = { TARGET: 1, DIRECTION: 2, DISTANCE: 3, TAB_ID: 4 } as const
const SCROLL_DIR = { LEFT: 1, RIGHT: 2, UP: 3, DOWN: 4 } as const

/** SelectAction */
const SELECT = { TARGET: 1, VALUE: 2, TAB_ID: 3 } as const

/** NavigateAction */
const NAVIGATE = { URL: 1, TAB_ID: 2 } as const

/** WaitAction */
const WAIT = { WAIT_TIME_MS: 1, OBSERVE_TAB_ID: 2 } as const

/** HistoryBackAction / HistoryForwardAction */
const HISTORY = { TAB_ID: 1 } as const

/** CreateTabAction */
const CREATE_TAB = { WINDOW_ID: 1, FOREGROUND: 2 } as const

/** CloseTabAction / ActivateTabAction */
const TAB_MGMT = { TAB_ID: 1 } as const

/** ScrollToAction */
const SCROLL_TO = { TARGET: 3, TAB_ID: 4 } as const

/** MediaControlAction (oneof) */
const MEDIA = { TAB_ID: 2, PLAY: 3, PAUSE: 4, SEEK: 5 } as const
const SEEK = { SEEK_TIME_US: 1 } as const

/** ActionTarget */
const TARGET = { CONTENT_NODE_ID: 1, COORDINATE: 2, DOCUMENT_IDENTIFIER: 3 } as const
const COORDINATE = { X: 1, Y: 2 } as const
const DOC_ID = { SERIALIZED_TOKEN: 1 } as const

/** ActionsResult */
const RESULT = {
  ACTION_RESULT: 1,
  INDEX_OF_FAILED: 2,
  WINDOWS: 3,
  TABS: 4,
  SCRIPT_RESULTS: 5,
} as const

/** WindowObservation */
const WIN_OBS = { ID: 1, ACTIVATED_TAB_ID: 2, ACTIVE: 3, TAB_IDS: 4 } as const

/** TabObservation */
const TAB_OBS = { ID: 1, RESULT: 5 } as const

/** ScriptToolResult */
const SCRIPT_RES = { INDEX: 1, RESULT: 2 } as const

// =========================================================================
//  Encoder — tool args → proto binary
// =========================================================================

function writeTarget(w: ProtoWriter, target: ActionTarget): void {
  if (isElementTarget(target)) {
    w.int32(TARGET.CONTENT_NODE_ID, target.node_id)
    w.message(TARGET.DOCUMENT_IDENTIFIER, (dw) => {
      dw.string(DOC_ID.SERIALIZED_TOKEN, target.document_id)
    })
  } else {
    w.message(TARGET.COORDINATE, (cw) => {
      cw.int32(COORDINATE.X, target.x)
      cw.int32(COORDINATE.Y, target.y)
    })
  }
}

function writeClick(w: ProtoWriter, a: ClickActionParams): void {
  w.message(ACTION.CLICK, (cw) => {
    cw.message(CLICK.TARGET, (tw) => writeTarget(tw, a.target))
    cw.uint32(CLICK.CLICK_TYPE, a.button === 'right' ? CLICK_TYPE.RIGHT : CLICK_TYPE.LEFT)
    cw.uint32(
      CLICK.CLICK_COUNT,
      a.click_count === 'double' ? CLICK_COUNT.DOUBLE : CLICK_COUNT.SINGLE
    )
    cw.int32(CLICK.TAB_ID, a.tab_id)
  })
}

function writeType(w: ProtoWriter, a: TypeActionParams): void {
  w.message(ACTION.TYPE, (tw) => {
    tw.message(TYPE.TARGET, (ttw) => writeTarget(ttw, a.target))
    tw.string(TYPE.TEXT, a.text)
    tw.bool(TYPE.FOLLOW_BY_ENTER, a.submit === true)
    tw.uint32(TYPE.MODE, TYPE_MODE.DELETE_EXISTING)
    tw.int32(TYPE.TAB_ID, a.tab_id)
  })
}

function writeSelect(w: ProtoWriter, a: SelectActionParams): void {
  w.message(ACTION.SELECT, (sw) => {
    sw.message(SELECT.TARGET, (tw) => writeTarget(tw, a.target))
    sw.string(SELECT.VALUE, a.value)
    sw.int32(SELECT.TAB_ID, a.tab_id)
  })
}

function writeNavigate(w: ProtoWriter, a: NavigateActionParams): void {
  w.message(ACTION.NAVIGATE, (nw) => {
    nw.string(NAVIGATE.URL, a.url)
    nw.int32(NAVIGATE.TAB_ID, a.tab_id)
  })
}

function writeScrollTo(w: ProtoWriter, a: ScrollToActionParams): void {
  w.message(ACTION.SCROLL_TO, (sw) => {
    sw.message(SCROLL_TO.TARGET, (tw) => writeTarget(tw, a.target))
    sw.int32(SCROLL_TO.TAB_ID, a.tab_id)
  })
}

function writeScroll(w: ProtoWriter, a: ScrollActionParams): void {
  const dirMap: Record<string, number> = {
    left: SCROLL_DIR.LEFT,
    right: SCROLL_DIR.RIGHT,
    up: SCROLL_DIR.UP,
    down: SCROLL_DIR.DOWN,
  }
  w.message(ACTION.SCROLL, (sw) => {
    sw.message(SCROLL.TARGET, (tw) => writeTarget(tw, a.target))
    sw.uint32(SCROLL.DIRECTION, dirMap[a.direction] ?? SCROLL_DIR.DOWN)
    sw.float(SCROLL.DISTANCE, a.distance)
    sw.int32(SCROLL.TAB_ID, a.tab_id)
  })
}

function writeWait(w: ProtoWriter, a: WaitActionParams): void {
  w.message(ACTION.WAIT, (ww) => {
    ww.int32(WAIT.WAIT_TIME_MS, a.wait_ms ?? 1000)
    if (a.observe_tab_id !== undefined) {
      ww.int32(WAIT.OBSERVE_TAB_ID, a.observe_tab_id)
    }
  })
}

function writeBack(w: ProtoWriter, a: BackActionParams): void {
  w.message(ACTION.BACK, (bw) => {
    bw.int32(HISTORY.TAB_ID, a.tab_id)
  })
}

function writeForward(w: ProtoWriter, a: ForwardActionParams): void {
  w.message(ACTION.FORWARD, (fw) => {
    fw.int32(HISTORY.TAB_ID, a.tab_id)
  })
}

function writeCreateTab(w: ProtoWriter, a: CreateTabActionParams): void {
  w.message(ACTION.CREATE_TAB, (ctw) => {
    ctw.int32(CREATE_TAB.WINDOW_ID, a.window_id)
    ctw.bool(CREATE_TAB.FOREGROUND, a.foreground ?? true)
  })
}

function writeCloseTab(w: ProtoWriter, a: CloseTabActionParams): void {
  w.message(ACTION.CLOSE_TAB, (ctw) => {
    ctw.int32(TAB_MGMT.TAB_ID, a.tab_id)
  })
}

function writeActivateTab(w: ProtoWriter, a: { tab_id: number }): void {
  w.message(ACTION.ACTIVATE_TAB, (atw) => {
    atw.int32(TAB_MGMT.TAB_ID, a.tab_id)
  })
}

function writeMediaPlay(w: ProtoWriter, a: MediaPlayActionParams): void {
  w.message(ACTION.MEDIA_CONTROL, (mw) => {
    mw.int32(MEDIA.TAB_ID, a.tab_id)
    mw.message(MEDIA.PLAY, () => {})
  })
}

function writeMediaPause(w: ProtoWriter, a: MediaPauseActionParams): void {
  w.message(ACTION.MEDIA_CONTROL, (mw) => {
    mw.int32(MEDIA.TAB_ID, a.tab_id)
    mw.message(MEDIA.PAUSE, () => {})
  })
}

function writeMediaSeek(w: ProtoWriter, a: MediaSeekActionParams): void {
  w.message(ACTION.MEDIA_CONTROL, (mw) => {
    mw.int32(MEDIA.TAB_ID, a.tab_id)
    mw.message(MEDIA.SEEK, (sw) => {
      sw.int64(SEEK.SEEK_TIME_US, BigInt(a.seek_time_ms) * 1000n)
    })
  })
}

/**
 * Write a single Action message into the parent writer.
 * The parent writer is the Actions-level writer; each call adds one
 * `repeated Action` entry.
 */
function writeAction(w: ProtoWriter, action: BrowserAction): void {
  switch (action.type) {
    case 'click':
      writeClick(w, action)
      break
    case 'type':
      writeType(w, action)
      break
    case 'select':
      writeSelect(w, action)
      break
    case 'navigate':
      writeNavigate(w, action)
      break
    case 'scroll_to':
      writeScrollTo(w, action)
      break
    case 'scroll':
      writeScroll(w, action)
      break
    case 'wait':
      writeWait(w, action)
      break
    case 'back':
      writeBack(w, action)
      break
    case 'forward':
      writeForward(w, action)
      break
    case 'create_tab':
      writeCreateTab(w, action)
      break
    case 'close_tab':
      writeCloseTab(w, action)
      break
    case 'activate_tab':
      writeActivateTab(w, action)
      break
    case 'media_play':
      writeMediaPlay(w, action)
      break
    case 'media_pause':
      writeMediaPause(w, action)
      break
    case 'media_seek':
      writeMediaSeek(w, action)
      break
  }
}

// =========================================================================
//  Public encoder API
// =========================================================================

/**
 * Build a serialized `Actions` proto from tool arguments.
 *
 * @param taskId  - The task ID obtained from createTask().
 * @param actions - The browser actions from the LLM tool call.
 * @param config  - Optional result config. Use config.skipObservation to skip
 *                  APC/screenshot collection at the proto level.
 * @returns Serialized proto bytes ready for performActions(ArrayBuffer).
 */
export function buildActionsProto(
  taskId: number,
  actions: BrowserAction[],
  config?: ActResultConfig
): Uint8Array {
  const w = new ProtoWriter()

  for (const action of actions) {
    w.message(ACTIONS.ACTIONS, (aw) => writeAction(aw, action))
  }

  w.int32(ACTIONS.TASK_ID, taskId)

  const skipObs =
    config?.skipObservation || config?.observationConfig?.skipAsyncObservation
  if (skipObs) {
    w.bool(ACTIONS.SKIP_ASYNC, true)
  }

  return w.finish()
}

/**
 * Build a serialized `Actions` proto for observation only (no real actions).
 * Internally uses a WaitAction with 0ms.
 *
 * @param taskId - The task ID.
 * @param options.observeTabId - Optional tab to observe.
 * @param options.config - Optional observation config for proto-level control.
 */
export function buildObserveProto(
  taskId: number,
  options?: { observeTabId?: number; config?: ObservationConfig }
): Uint8Array {
  const waitAction: WaitActionParams = {
    type: 'wait',
    wait_ms: 0,
    observe_tab_id: options?.observeTabId,
  }
  return buildActionsProto(taskId, [waitAction], {
    skipObservation: options?.config?.skipAsyncObservation,
  })
}

// =========================================================================
//  Decoder — proto binary → BrowserActResult
// =========================================================================

function parseWindowObservation(r: ProtoReader): WindowInfo {
  const win: WindowInfo = { id: 0, active_tab_id: 0, is_active: false, tab_ids: [] }

  while (r.hasMore()) {
    const tag = r.readTag()
    if (!tag) break

    switch (tag.field) {
      case WIN_OBS.ID:
        win.id = r.readInt32()
        break
      case WIN_OBS.ACTIVATED_TAB_ID:
        win.active_tab_id = r.readInt32()
        break
      case WIN_OBS.ACTIVE:
        win.is_active = r.readBool()
        break
      case WIN_OBS.TAB_IDS:
        if (tag.wireType === 2) {
          // Packed repeated int32
          const packed = r.readMessage()
          while (packed.hasMore()) {
            win.tab_ids.push(packed.readInt32())
          }
        } else {
          win.tab_ids.push(r.readInt32())
        }
        break
      default:
        r.skip(tag.wireType)
    }
  }

  return win
}

function parseTabObservation(r: ProtoReader): TabInfo {
  const tab: TabInfo = { id: 0, ok: true }

  while (r.hasMore()) {
    const tag = r.readTag()
    if (!tag) break

    switch (tag.field) {
      case TAB_OBS.ID:
        tab.id = r.readInt32()
        break
      case TAB_OBS.RESULT: {
        const code = r.readUint32()
        tab.ok = code === 0
        if (code !== 0) {
          tab.error = TAB_OBSERVATION_RESULTS[code] ?? `UNKNOWN_${code}`
        }
        break
      }
      default:
        r.skip(tag.wireType)
    }
  }

  return tab
}

function parseScriptResult(r: ProtoReader): ScriptResultInfo {
  const res: ScriptResultInfo = { action_index: 0, result: '' }

  while (r.hasMore()) {
    const tag = r.readTag()
    if (!tag) break

    switch (tag.field) {
      case SCRIPT_RES.INDEX:
        res.action_index = r.readInt32()
        break
      case SCRIPT_RES.RESULT:
        res.result = r.readString()
        break
      default:
        r.skip(tag.wireType)
    }
  }

  return res
}

/**
 * Parse a serialized `ActionsResult` proto into a simplified result object.
 * Only extracts action status, window list, tab IDs, and script results.
 * Does NOT parse APC or screenshots — use parseActionsResultWithObservations
 * for that.
 *
 * @param buffer - The ArrayBuffer returned by performActions().
 * @returns A simplified result suitable for returning to the LLM.
 */
export function parseActionsResult(buffer: ArrayBuffer): BrowserActResult {
  const r = ProtoReader.fromArrayBuffer(buffer)
  const result: BrowserActResult = {
    success: true,
    action_result_code: 0,
    action_result_name: 'OK',
    failed_action_index: null,
    windows: [],
    tabs: [],
    script_results: [],
  }

  while (r.hasMore()) {
    const tag = r.readTag()
    if (!tag) break

    switch (tag.field) {
      case RESULT.ACTION_RESULT: {
        const code = r.readInt32()
        result.action_result_code = code
        result.action_result_name = getActionResultName(code)
        result.success = code === 0
        break
      }
      case RESULT.INDEX_OF_FAILED: {
        const idx = r.readInt32()
        result.failed_action_index = idx >= 0 ? idx : null
        break
      }
      case RESULT.WINDOWS:
        result.windows.push(parseWindowObservation(r.readMessage()))
        break
      case RESULT.TABS:
        result.tabs.push(parseTabObservation(r.readMessage()))
        break
      case RESULT.SCRIPT_RESULTS:
        result.script_results.push(parseScriptResult(r.readMessage()))
        break
      default:
        r.skip(tag.wireType)
    }
  }

  return result
}

/**
 * Parse a serialized `ActionsResult` proto with full observation data
 * (APC DOM tree, screenshots, metadata). Used to build the observation
 * returned alongside every browser_* tool result.
 *
 * @param buffer - The ArrayBuffer returned by performActions().
 * @param config - Optional observation config for filtering/compressing elements.
 * @returns Result with full page observations including DOM elements.
 */
export function parseActionsResultWithObservations(
  buffer: ArrayBuffer,
  config?: ObservationConfig
): BrowserObserveResult {
  const r = ProtoReader.fromArrayBuffer(buffer)
  const result: BrowserObserveResult = {
    success: true,
    action_result_code: 0,
    action_result_name: 'OK',
    windows: [],
    observations: [],
  }

  while (r.hasMore()) {
    const tag = r.readTag()
    if (!tag) break

    switch (tag.field) {
      case RESULT.ACTION_RESULT: {
        const code = r.readInt32()
        result.action_result_code = code
        result.action_result_name = getActionResultName(code)
        result.success = code === 0
        break
      }
      case RESULT.INDEX_OF_FAILED:
        r.readInt32()
        break
      case RESULT.WINDOWS:
        result.windows.push(parseWindowObservation(r.readMessage()))
        break
      case RESULT.TABS:
        result.observations.push(
          applyObservationConfig(parseTabObservationFull(r.readMessage()), config)
        )
        break
      case RESULT.SCRIPT_RESULTS:
        r.skip(tag.wireType)
        break
      default:
        r.skip(tag.wireType)
    }
  }

  return result
}

/**
 * Apply ObservationConfig to a parsed PageObservation.
 * Filters elements, drops screenshots/metadata per config.
 */
function applyObservationConfig(
  obs: PageObservation,
  config?: ObservationConfig
): PageObservation {
  if (!config) return obs

  const result = { ...obs }

  result.elements = filterElements(result.elements, config.elements)

  if (config.screenshot?.include === false) {
    result.screenshot = undefined
  } else if (config.screenshot?.maxSizeBytes != null && result.screenshot) {
    const sizeBytes = result.screenshot.data_base64.length * 0.75
    if (sizeBytes > config.screenshot.maxSizeBytes) {
      result.screenshot = undefined
    }
  }

  if (config.includeMetadata === false) {
    result.metadata = undefined
  }

  return result
}

// =========================================================================
//  Convenience: format result as concise JSON string for LLM consumption
// =========================================================================

/**
 * Compress a BrowserActResult into a compact JSON string for the LLM.
 * Strips empty arrays and null fields to save tokens.
 */
export function resultToToolResponse(
  result: BrowserActResult,
  options?: {
    includeWindows?: boolean
    includeTabs?: boolean
  }
): string {
  const out: Record<string, unknown> = {
    ok: result.success,
    result: result.action_result_name,
  }
  const includeWindows = options?.includeWindows ?? true
  const includeTabs = options?.includeTabs ?? true

  if (result.failed_action_index !== null) {
    out['failed_at'] = result.failed_action_index
  }

  if (includeWindows && result.windows.length > 0) {
    out['windows'] = result.windows.map((w) => ({
      id: w.id,
      active_tab: w.active_tab_id,
      is_active: w.is_active,
      tabs: w.tab_ids,
    }))
  }

  if (includeTabs && result.tabs.length > 0) {
    out['tabs'] = result.tabs.map((t) => {
      const entry: Record<string, unknown> = { id: t.id }
      if (!t.ok) entry['error'] = t.error
      return entry
    })
  }

  if (result.script_results.length > 0) {
    out['script_results'] = result.script_results
  }

  return JSON.stringify(out)
}

/**
 * Compress a BrowserObserveResult into a JSON string for the LLM.
 * Includes full DOM element list, frame info, and metadata.
 * Screenshots are referenced but not inlined (too large for text context).
 *
 * @param result - The parsed observe result.
 * @param config - Optional observation config for element filtering/compression.
 *                 If you already applied config during parseActionsResultWithObservations,
 *                 you can pass undefined here to avoid double-filtering.
 */
export function observeResultToToolResponse(
  result: BrowserObserveResult,
  config?: ObservationConfig
): string {
  const out: Record<string, unknown> = {
    ok: result.success,
    result: result.action_result_name,
  }

  if (result.windows.length > 0) {
    out['windows'] = result.windows.map((w) => ({
      id: w.id,
      active_tab: w.active_tab_id,
      is_active: w.is_active,
      tabs: w.tab_ids,
    }))
  }

  if (result.observations.length > 0) {
    out['pages'] = result.observations.map((obs) =>
      observationToToolResponse(obs, config)
    )
  }

  return JSON.stringify(out)
}

/**
 * Extract raw screenshot data from an observe result.
 * Returns the screenshots separately for multimodal model consumption,
 * since base64-encoded images are too large for text tool responses.
 *
 * @param result - The parsed observe result.
 * @param config - Optional screenshot config. When maxSizeBytes is set,
 *                 oversized screenshots are dropped.
 */
export function extractScreenshots(
  result: BrowserObserveResult,
  config?: ObservationConfig
): Array<{ tab_id: number; data_base64: string; mime_type: string }> {
  if (config?.screenshot?.include === false) return []

  const maxSize = config?.screenshot?.maxSizeBytes

  return result.observations
    .filter((obs) => {
      if (obs.screenshot == null) return false
      if (maxSize != null) {
        const sizeBytes = obs.screenshot.data_base64.length * 0.75
        if (sizeBytes > maxSize) return false
      }
      return true
    })
    .map((obs) => ({
      tab_id: obs.tab_id,
      data_base64: obs.screenshot!.data_base64,
      mime_type: obs.screenshot!.mime_type,
    }))
}

// =========================================================================
//  Simple mode: convert element_id-based tool args → BrowserAction
// =========================================================================

function isPositiveInteger(value: unknown): value is number {
  return typeof value === 'number' && Number.isInteger(value) && value > 0
}

export type SimpleToolCallError = {
  error: string
  missing_fields: string[]
}

/** Type guard to distinguish validation errors from valid actions. */
export function isSimpleToolCallError(
  result: BrowserAction | SimpleToolCallError | null
): result is SimpleToolCallError {
  return result != null && 'error' in result
}

/** Tools that require an element_id to resolve their target. */
const ELEMENT_TARGETING_TOOLS = new Set([
  'browser_click', 'browser_type', 'browser_select', 'browser_scroll_to',
])

/** Tools where element_id is optional (e.g. browser_scroll). */
const OPTIONAL_ELEMENT_TOOLS = new Set(['browser_scroll'])

const EDITABLE_FORM_CONTROL_TYPES = new Set([
  'content_editable',
  'date',
  'datetime_local',
  'email',
  'month',
  'number',
  'password',
  'search',
  'telephone',
  'text',
  'textarea',
  'time',
  'url',
  'week',
])

function getElementPreviewText(entry: ElementRegistryEntry): string | undefined {
  const candidates = [
    entry.text,
    entry.title,
    entry.label,
    entry.form_control?.placeholder,
    entry.form_control?.field_name,
    entry.anchor_href,
  ]

  return candidates.find(
    (value): value is string => typeof value === 'string' && value.trim().length > 0
  )
}

function describeResolvedElement(entry: ElementRegistryEntry): string {
  const controlType = entry.form_control?.control_type
  const elementType = controlType ? `${entry.type}/${controlType}` : entry.type
  const previewText = getElementPreviewText(entry)
  return previewText ? `${elementType} "${previewText}"` : elementType
}

function isEditableTypeTarget(entry: ElementRegistryEntry): boolean {
  if (entry.type !== 'form_control') {
    return false
  }

  const controlType = entry.form_control?.control_type
  if (!controlType || controlType === 'unspecified') {
    return true
  }

  return EDITABLE_FORM_CONTROL_TYPES.has(controlType)
}

/**
 * Resolve element_id from args using the registry.
 * Returns the resolved target and tab_id, or a SimpleToolCallError.
 */
function resolveElementTarget(
  toolName: string,
  args: Record<string, unknown>,
  registry: ElementRegistry
): {
  element_id: number
  entry: ElementRegistryEntry
  tab_id: number
  target: ActionTarget
} | SimpleToolCallError {
  const elementId = args['element_id']
  if (!isPositiveInteger(elementId)) {
    return {
      error: `${toolName}: missing required element_id`,
      missing_fields: ['element_id'],
    }
  }
  let entry: ElementRegistryEntry
  try {
    entry = registry.resolve(elementId)
  } catch (err) {
    const msg = err instanceof Error ? err.message : String(err)
    if (msg.startsWith('INVALID_ELEMENT_ID:')) {
      return {
        error: msg.replace(/^INVALID_ELEMENT_ID:\s*/, ''),
        missing_fields: [],
      }
    }
    throw err
  }

  if (!entry.document_id) {
    return {
      error:
        `${toolName}: element_id ${elementId} has an empty document_id ` +
        '(the page may have navigated since the last observation). ' +
        'Use the element_id values from the most recent tool result observation.',
      missing_fields: [],
    }
  }

  const explicitTabId = isPositiveInteger(args['tab_id'])
    ? args['tab_id']
    : undefined

  return {
    element_id: elementId,
    entry,
    tab_id: explicitTabId ?? entry.tab_id,
    target: { node_id: entry.node_id, document_id: entry.document_id },
  }
}

/**
 * Convert a simple-mode tool call into a BrowserAction using the ElementRegistry.
 *
 * Element-targeting tools (click, type, select, scroll_to) resolve element_id
 * to (tab_id, node_id, document_id) via the registry. Tab-level tools
 * (navigate, back, forward, etc.) use the explicit tab_id arg or 0 for fallback.
 *
 * @param toolName - The simple tool name (e.g. "browser_click").
 * @param args     - The flat JSON args from the LLM tool call.
 * @param registry - The element registry for resolving element_id → real targets.
 *
 * @returns BrowserAction on success, SimpleToolCallError on validation failure,
 *          or null if the tool name is not recognized.
 */
export function parseSimpleToolCall(
  toolName: string,
  args: Record<string, unknown>,
  registry?: ElementRegistry
): BrowserAction | SimpleToolCallError | null {
  const actionType = SIMPLE_TOOL_NAME_TO_ACTION_TYPE[toolName]
  if (!actionType) return null

  if (ELEMENT_TARGETING_TOOLS.has(toolName)) {
    if (!registry) {
      return {
        error: `${toolName}: ElementRegistry is required for element-targeting tools`,
        missing_fields: ['element_id'],
      }
    }
    const resolved = resolveElementTarget(toolName, args, registry)
    if ('error' in resolved) return resolved

    switch (actionType) {
      case 'click':
        return {
          type: 'click',
          tab_id: resolved.tab_id,
          target: resolved.target,
          button: args['button'] === 'right' ? 'right' : undefined,
          click_count: args['click_count'] === 'double' ? 'double' : undefined,
        }
      case 'type': {
        if (typeof args['text'] !== 'string' || args['text'] === '') {
          return { error: `browser_type: missing text`, missing_fields: ['text'] }
        }
        if (!isEditableTypeTarget(resolved.entry)) {
          return {
            error:
              `browser_type: element_id ${resolved.element_id} targets ` +
              `${describeResolvedElement(resolved.entry)}, not an editable form control. ` +
              'Choose a form_control element_id from the most recent observation that matches by text, title, or placeholder.',
            missing_fields: [],
          }
        }
        return {
          type: 'type',
          tab_id: resolved.tab_id,
          target: resolved.target,
          text: args['text'],
          submit: args['submit'] === true ? true : undefined,
        }
      }
      case 'select': {
        if (typeof args['value'] !== 'string') {
          return { error: `browser_select: missing value`, missing_fields: ['value'] }
        }
        return {
          type: 'select',
          tab_id: resolved.tab_id,
          target: resolved.target,
          value: args['value'],
        }
      }
      case 'scroll_to':
        return {
          type: 'scroll_to',
          tab_id: resolved.tab_id,
          target: resolved.target,
        }
      default:
        return null
    }
  }

  if (OPTIONAL_ELEMENT_TOOLS.has(toolName)) {
    const missing: string[] = []
    if (typeof args['direction'] !== 'string') missing.push('direction')
    if (typeof args['distance'] !== 'number') missing.push('distance')
    if (missing.length > 0) {
      return { error: `browser_scroll: missing ${missing.join(', ')}`, missing_fields: missing }
    }

    const elementId = args['element_id']
    if (isPositiveInteger(elementId) && registry) {
      const resolved = resolveElementTarget(toolName, args, registry)
      if ('error' in resolved) return resolved
      return {
        type: 'scroll',
        tab_id: resolved.tab_id,
        target: resolved.target,
        direction: args['direction'] as 'up' | 'down' | 'left' | 'right',
        distance: args['distance'] as number,
      }
    }

    // No element_id: scroll at page center — tab_id resolved by executor
    const tabId = isPositiveInteger(args['tab_id']) ? args['tab_id'] : 0
    return {
      type: 'scroll',
      tab_id: tabId,
      target: { x: 0, y: 0 },
      direction: args['direction'] as 'up' | 'down' | 'left' | 'right',
      distance: args['distance'] as number,
    }
  }

  // Tab-level tools: use explicit tab_id or 0 for fallback resolution
  const tabId = isPositiveInteger(args['tab_id']) ? args['tab_id'] : 0

  switch (actionType) {
    case 'navigate': {
      if (typeof args['url'] !== 'string' || args['url'] === '') {
        return { error: `browser_navigate: missing url`, missing_fields: ['url'] }
      }
      return { type: 'navigate', tab_id: tabId, url: args['url'] }
    }
    case 'wait':
      return {
        type: 'wait',
        wait_ms: typeof args['wait_ms'] === 'number' ? args['wait_ms'] : undefined,
      }
    case 'back':
      return { type: 'back', tab_id: tabId }
    case 'forward':
      return { type: 'forward', tab_id: tabId }
    case 'create_tab':
      return {
        error:
          'browser_create_tab is handled by executeHostedTool',
        missing_fields: [],
      }
    case 'close_tab':
      return { type: 'close_tab', tab_id: tabId }
    case 'activate_tab':
      return { type: 'activate_tab', tab_id: tabId }
    default:
      return null
  }
}
