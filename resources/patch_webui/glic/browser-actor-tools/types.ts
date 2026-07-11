// ---------------------------------------------------------------------------
// Browser Actor Tools — Types
// ---------------------------------------------------------------------------
// LLM-facing action parameter types, result types, and action-result code map.
// These types describe what the LLM sends and receives — the proto layer is
// an internal implementation detail handled by actionsConverter.ts.
// ---------------------------------------------------------------------------

export type HostedToolInputSchema = {
  [key: string]: unknown
  type: 'object'
  properties?: Record<string, unknown>
  required?: string[]
}

export type HostedToolDefinition = {
  type: 'function'
  name: string
  description: string
  parameters: HostedToolInputSchema
  aliases?: string[]
  searchHint?: string
  readOnly?: boolean
}

export type HostedToolExecutionRequest = {
  name: string
  args: Record<string, unknown>
}

export type HostedToolExecutionResult = unknown

// ========================= Action Target =========================

/** Element target identified by APC node id + document token. */
export type ElementTarget = {
  node_id: number
  document_id: string
}

/** Coordinate target in viewport pixels. */
export type CoordinateTarget = {
  x: number
  y: number
}

export type ActionTarget = ElementTarget | CoordinateTarget

export function isElementTarget(t: ActionTarget): t is ElementTarget {
  return 'node_id' in t
}

// ========================= Action Params =========================

export type ClickActionParams = {
  type: 'click'
  tab_id: number
  target: ActionTarget
  button?: 'left' | 'right'
  click_count?: 'single' | 'double'
}

export type TypeActionParams = {
  type: 'type'
  tab_id: number
  target: ActionTarget
  text: string
  submit?: boolean
}

export type SelectActionParams = {
  type: 'select'
  tab_id: number
  target: ActionTarget
  value: string
}

export type NavigateActionParams = {
  type: 'navigate'
  tab_id: number
  url: string
}

export type ScrollToActionParams = {
  type: 'scroll_to'
  tab_id: number
  target: ActionTarget
}

export type ScrollActionParams = {
  type: 'scroll'
  tab_id: number
  target: ActionTarget
  direction: 'up' | 'down' | 'left' | 'right'
  distance: number
}

export type WaitActionParams = {
  type: 'wait'
  wait_ms?: number
  observe_tab_id?: number
}

export type BackActionParams = {
  type: 'back'
  tab_id: number
}

export type ForwardActionParams = {
  type: 'forward'
  tab_id: number
}

export type CreateTabActionParams = {
  type: 'create_tab'
  window_id: number
  foreground?: boolean
}

export type CloseTabActionParams = {
  type: 'close_tab'
  tab_id: number
}

export type ActivateTabActionParams = {
  type: 'activate_tab'
  tab_id: number
}

export type MediaPlayActionParams = {
  type: 'media_play'
  tab_id: number
}

export type MediaPauseActionParams = {
  type: 'media_pause'
  tab_id: number
}

export type MediaSeekActionParams = {
  type: 'media_seek'
  tab_id: number
  seek_time_ms: number
}

/** Union of all supported browser actions. */
export type BrowserAction =
  | ClickActionParams
  | TypeActionParams
  | SelectActionParams
  | NavigateActionParams
  | ScrollToActionParams
  | ScrollActionParams
  | WaitActionParams
  | BackActionParams
  | ForwardActionParams
  | CreateTabActionParams
  | CloseTabActionParams
  | ActivateTabActionParams
  | MediaPlayActionParams
  | MediaPauseActionParams
  | MediaSeekActionParams

// ========================= Tool Args =========================
// Element-targeting tools use a synthetic element_id returned by a previous
// browser_* tool's observation instead of raw node_id/document_id pairs. Every
// tool carries a short tool_reason; element-targeting tools also carry an
// element_reason that explains why the chosen element matches the intent.
// Tab-scoped tools accept an optional tab_id — omit to act on the active tab.
// Every tool returns a fresh observation (no screenshot). Use the dedicated
// browser_screenshot tool when a visual is required.

type SimpleCommonReasonFields = {
  tool_reason: string
}

type SimpleElementTargetField = {
  element_id: number
}

type SimpleElementReasonField = {
  element_reason: string
}

export type SimpleClickArgs = SimpleCommonReasonFields &
  SimpleElementTargetField &
  SimpleElementReasonField & {
  button?: 'left' | 'right'
  click_count?: 'single' | 'double'
}

export type SimpleTypeArgs = SimpleCommonReasonFields &
  SimpleElementTargetField &
  SimpleElementReasonField & {
  text: string
  submit?: boolean
}

export type SimpleSelectArgs = SimpleCommonReasonFields &
  SimpleElementTargetField &
  SimpleElementReasonField & {
  tab_id?: number
  value: string
}

export type SimpleNavigateArgs = SimpleCommonReasonFields & {
  tab_id?: number
  url: string
}

export type SimpleScrollToArgs = SimpleCommonReasonFields &
  SimpleElementTargetField &
  SimpleElementReasonField & {
  tab_id?: number
}

export type SimpleScrollArgs = SimpleCommonReasonFields & {
  tab_id?: number
  element_id?: number
  element_reason?: string
  direction: 'up' | 'down' | 'left' | 'right'
  distance: number
}

export type SimpleWaitArgs = SimpleCommonReasonFields & {
  wait_ms?: number
}

export type SimpleBackArgs = SimpleCommonReasonFields & { tab_id?: number }
export type SimpleForwardArgs = SimpleCommonReasonFields & { tab_id?: number }
export type SimpleCreateTabArgs = SimpleCommonReasonFields & {
  url: string
  reuse_visible_tab?: boolean
}
export type SimpleCloseTabArgs = SimpleCommonReasonFields & { tab_id?: number }
export type SimpleActivateTabArgs = SimpleCommonReasonFields & { tab_id?: number }
export type SimpleScreenshotArgs = SimpleCommonReasonFields & { tab_id?: number }

// Read tools (read-only, do not invoke the actor pipeline). They are part of
// the simple tool surface but never produce ELEMENT_OFFSCREEN — they fetch
// content from the latest APC observation and expose it as plain text.
export type SimpleReadPageArgs = SimpleCommonReasonFields & {
  tab_id?: number
  scope?: 'viewport' | 'full_page'
  max_chars?: number
}

export type SimpleReadElementArgs = SimpleCommonReasonFields &
  SimpleElementTargetField &
  SimpleElementReasonField & {
  max_chars?: number
}

export type SimpleFindTextArgs = SimpleCommonReasonFields & {
  tab_id?: number
  query: string
  max_results?: number
}

/** Union of all simple tool argument types. */
export type SimpleToolArgs =
  | SimpleClickArgs
  | SimpleTypeArgs
  | SimpleSelectArgs
  | SimpleNavigateArgs
  | SimpleScrollToArgs
  | SimpleScrollArgs
  | SimpleWaitArgs
  | SimpleBackArgs
  | SimpleForwardArgs
  | SimpleCreateTabArgs
  | SimpleCloseTabArgs
  | SimpleActivateTabArgs
  | SimpleScreenshotArgs
  | SimpleReadPageArgs
  | SimpleReadElementArgs
  | SimpleFindTextArgs

// ========================= Results =========================

export type WindowInfo = {
  id: number
  active_tab_id: number
  is_active: boolean
  tab_ids: number[]
}

export type TabInfo = {
  id: number
  ok: boolean
  error?: string
}

export type ScriptResultInfo = {
  action_index: number
  result: string
}

export type BrowserActResult = {
  success: boolean
  action_result_code: number
  action_result_name: string
  failed_action_index: number | null
  windows: WindowInfo[]
  tabs: TabInfo[]
  script_results: ScriptResultInfo[]
}

// ========================= Action Result Codes =========================
// From chrome/common/actor.mojom ActionResultCode

// Matches ActionResultCode enum from chrome/common/actor.mojom.
export const ACTION_RESULT_CODES: Record<number, string> = {
  0: 'OK',
  // 10-99: General errors
  11: 'URL_BLOCKED',
  12: 'NEW_TAB_CREATION_FAILED',
  13: 'TAB_WENT_AWAY',
  14: 'TASK_WENT_AWAY',
  15: 'CROSS_ORIGIN_NAVIGATION',
  16: 'TOOL_UNKNOWN',
  17: 'FRAME_WENT_AWAY',
  18: 'INVALID_DOM_NODE_ID',
  19: 'ELEMENT_DISABLED',
  20: 'ELEMENT_OFFSCREEN',
  21: 'COORDINATES_OUT_OF_BOUNDS',
  22: 'ARGUMENTS_INVALID',
  23: 'TASK_PAUSED',
  24: 'EXECUTOR_DESTROYED',
  25: 'WINDOW_WENT_AWAY',
  26: 'FRAME_LOCATION_CHANGED',
  27: 'TRIGGERED_NAVIGATION_BLOCKED',
  28: 'EMPTY_ACTION_SEQUENCE',
  29: 'EXECUTOR_BUSY',
  30: 'OBSERVED_TARGET_DESTROYED',
  31: 'OBSERVED_TARGET_CHANGED',
  32: 'TARGET_INTERACTION_POINT_OBSCURED',
  33: 'TOOL_TIMEOUT',
  34: 'FILE_PICKER_TRIGGERED',
  35: 'FILE_PICKER_CONFIRMED',
  36: 'FILE_PICKER_CANCELLED',
  37: 'EXECUTION_ENGINE_EXISTING_ACTION',
  38: 'EXTERNAL_PROTOCOL_NAVIGATION_BLOCKED',
  39: 'RENDERER_CRASHED',
  40: 'NOT_IMPLEMENTED',
  41: 'INVOKE_CANCELED',
  // 100-199: Navigate errors
  100: 'NAVIGATE_INVALID_URL',
  101: 'NAVIGATE_FAILED_TO_START',
  102: 'NAVIGATE_COMMITTED_ERROR_PAGE',
  // 200-299: Click errors
  200: 'CLICK_SUPPRESSED',
  // 300-399: DragAndRelease errors
  300: 'DRAG_AND_RELEASE_FROM_OFFSCREEN',
  301: 'DRAG_AND_RELEASE_TO_OFFSCREEN',
  302: 'DRAG_AND_RELEASE_FROM_MOVE_SUPPRESSED',
  303: 'DRAG_AND_RELEASE_DOWN_SUPPRESSED',
  304: 'DRAG_AND_RELEASE_TO_MOVE_SUPPRESSED',
  305: 'DRAG_AND_RELEASE_UP_SUPPRESSED',
  // 400-499: MouseMove errors
  400: 'MOUSE_MOVE_EVENT_SUPPRESSED',
  // 500-599: Scroll errors
  500: 'SCROLL_NO_SCROLLING_ELEMENT',
  501: 'SCROLL_TARGET_NOT_USER_SCROLLABLE',
  502: 'SCROLL_OFFSET_DID_NOT_CHANGE',
  // 600-699: Select errors
  600: 'SELECT_INVALID_ELEMENT',
  601: 'SELECT_NO_SUCH_OPTION',
  602: 'SELECT_OPTION_DISABLED',
  // 700-799: Type errors
  700: 'TYPE_TARGET_NOT_ELEMENT',
  701: 'TYPE_TARGET_NOT_FOCUSABLE',
  702: 'TYPE_UNSUPPORTED_CHARACTERS',
  703: 'TYPE_FAILED_MAPPING_CHAR_TO_KEY',
  704: 'TYPE_KEY_DOWN_SUPPRESSED',
  705: 'TYPE_INVALID_TEXT_ENCODING',
  // 800-899: History errors
  800: 'HISTORY_NO_NAVIGATIONS_CREATED',
  801: 'HISTORY_CANCELLED_BEFORE_START',
  802: 'HISTORY_NO_BACK_ENTRIES',
  803: 'HISTORY_NO_FORWARD_ENTRIES',
  804: 'HISTORY_FAILED_BEFORE_COMMIT',
  805: 'HISTORY_ERROR_PAGE',
  806: 'HISTORY_NAVIGATION_ENTRY_CHANGED',
  // 900-999: Login errors
  900: 'LOGIN_NO_CREDENTIALS_AVAILABLE',
  901: 'LOGIN_NOT_LOGIN_PAGE',
  902: 'LOGIN_CREDENTIALS_ENTERED_BUT_ANOTHER_ACTION_NEEDED',
  903: 'LOGIN_NO_FILLABLE_FIELDS',
  904: 'LOGIN_FILLING_NOT_ALLOWED',
  905: 'LOGIN_PAGE_CHANGED_DURING_SELECTION',
  906: 'LOGIN_DEVICE_REAUTH_REQUIRED',
  907: 'LOGIN_DEVICE_REAUTH_FAILED',
  908: 'LOGIN_FEATURE_DISABLED',
  909: 'LOGIN_TOO_MANY_REQUESTS',
  // 1000-1099: MediaControl errors
  1000: 'MEDIA_CONTROL_NO_MEDIA',
  // 1100-1199: FormFilling errors
  1100: 'FORM_FILLING_AUTOFILL_UNAVAILABLE',
  1101: 'FORM_FILLING_NO_SUGGESTIONS_AVAILABLE',
  1102: 'FORM_FILLING_DIALOG_ERROR',
  1103: 'FORM_FILLING_FIELD_NOT_FOUND',
  1104: 'FORM_FILLING_UNKNOWN_AUTOFILL_ERROR',
  1105: 'FORM_FILLING_NO_LAST_TAB_OBSERVATION',
  1106: 'FORM_FILLING_INVALID_SUGGESTION_ID',
  // 1200-1299: ScriptTool errors
  1200: 'SCRIPT_TOOL_NO_RESPONSE',
  // 1300-1399: Actor UI errors
  1300: 'ACTOR_UI_ERROR',
}

export function getActionResultName(code: number): string {
  return ACTION_RESULT_CODES[code] ?? `UNKNOWN_${code}`
}

// ========================= Observation Types =========================

export type BoundingBox = {
  x: number
  y: number
  width: number
  height: number
}

export type SelectOptionInfo = {
  value: string
  text: string
}

export type FormControlInfo = {
  field_name: string
  field_value: string
  control_type: string
  placeholder?: string
  is_checked?: boolean
  is_required?: boolean
  options?: SelectOptionInfo[]
}

/**
 * A flattened page element extracted from the APC ContentNode tree.
 * Contains everything the LLM needs to identify and target elements.
 */
export type PageElement = {
  node_id: number
  type: string
  text?: string
  title?: string
  label?: string
  roles?: string[]
  aria_role?: string
  bbox?: BoundingBox
  visible_bbox?: BoundingBox
  is_focusable?: boolean
  is_disabled?: boolean
  is_clickable?: boolean
  form_control?: FormControlInfo
  anchor_href?: string
  children_count?: number
}

export type ScreenshotData = {
  data_base64: string
  mime_type: string
}

export type FrameInfo = {
  url: string
  title: string
  document_id: string
}

export type PageObservation = {
  tab_id: number
  frame?: FrameInfo
  viewport?: BoundingBox
  elements: PageElement[]
  screenshot?: ScreenshotData
  metadata?: Array<{ name: string; content: string }>
}

/** Full result from a proto observe call: status + page observations. */
export type BrowserObserveResult = {
  success: boolean
  action_result_code: number
  action_result_name: string
  windows: WindowInfo[]
  observations: PageObservation[]
}

// ========================= Adapter-layer Configuration =========================
// These types are NOT exposed to the LLM. They're used by the web app adapter
// to control observation detail level, manage token budgets, and handle
// screenshot lifecycle across turns.

/** Controls which elements from APC are included in the observation. */
export type ElementFilterConfig = {
  /** Only include elements with a non-empty visible_bbox. Default false. */
  onlyVisible?: boolean
  /** Only include interactive elements (clickable, focusable, or form_control). Default false. */
  onlyInteractive?: boolean
  /**
   * When used with onlyInteractive, also keep non-interactive elements that
   * carry text content (headings, paragraphs, labels, etc.). This lets the
   * model read page content while still filtering out empty structural nodes.
   */
  includeTextElements?: boolean
  /** Max number of elements to return. Keeps top-priority elements. Default unlimited. */
  maxElements?: number
  /** Truncate text content per element to this many characters. Default unlimited. */
  maxTextLength?: number
  /** Include bounding box geometry. Default true. */
  includeBoundingBoxes?: boolean
  /** Include full form control details (options, placeholder, etc.). Default true. */
  includeFormDetails?: boolean
  /** Element types to exclude (e.g. ['root', 'container']). Default []. */
  excludeTypes?: string[]
}

/** Controls screenshot handling at the adapter level. */
export type ScreenshotConfig = {
  /** Whether to include screenshot in this observation. Default true. */
  include?: boolean
  /** Drop screenshot if larger than this (bytes). Avoids oversized images. */
  maxSizeBytes?: number
}

/**
 * Adapter-level configuration for observation output.
 * Passed by the web app layer to control token pressure.
 *
 * Example presets:
 *   FULL:     { elements: {}, screenshot: { include: true } }
 *   COMPACT:  { elements: { onlyInteractive: true, maxElements: 50, maxTextLength: 80 } }
 *   MINIMAL:  { elements: { onlyInteractive: true, maxElements: 20, includeBoundingBoxes: false },
 *               screenshot: { include: false } }
 */
export type ObservationConfig = {
  /** Element filtering options. */
  elements?: ElementFilterConfig
  /** Screenshot options. */
  screenshot?: ScreenshotConfig
  /** Whether to include page <meta> tags. Default true. */
  includeMetadata?: boolean
  /**
   * Proto-level flag: skip APC + screenshot collection entirely.
   * When true, the browser won't fetch APC or screenshot at all (fastest).
   * Overrides all other options.
   */
  skipAsyncObservation?: boolean
}

/**
 * Configuration for browser_act result handling.
 * Controls whether and how observations are collected after acting.
 */
export type ActResultConfig = {
  /**
   * Skip observation collection after acting.
   * When true, the result will only contain action status and window/tab IDs.
   */
  skipObservation?: boolean
  /**
   * If not skipping, apply these filters to the observation.
   * Useful for getting a lightweight post-act status check.
   */
  observationConfig?: ObservationConfig
}

/** Pre-built observation presets for common scenarios. */
export const OBSERVATION_PRESETS = {
  /** Full observation: all elements, screenshots, metadata. */
  FULL: {
    elements: {},
    screenshot: { include: true },
    includeMetadata: true,
  } satisfies ObservationConfig,

  /** Interactive elements only, with bounding boxes. Good for most act→observe loops. */
  COMPACT: {
    elements: {
      onlyInteractive: true,
      maxElements: 60,
      maxTextLength: 100,
      excludeTypes: ['root', 'container'],
    },
    screenshot: { include: true },
    includeMetadata: false,
  } satisfies ObservationConfig,

  /** Interactive elements + text-bearing elements (headings, paragraphs, labels). */
  INTERACTIVE_WITH_TEXT: {
    elements: {
      onlyInteractive: true,
      includeTextElements: true,
      onlyVisible: true,
      maxElements: 80,
      maxTextLength: 100,
      excludeTypes: ['root', 'container'],
    },
    screenshot: { include: true },
    includeMetadata: false,
  } satisfies ObservationConfig,

  /**
   * Default for action-tool observations: only actionable elements
   * (clickable, focusable, form_control, anchor). Drops bare text nodes —
   * the model uses browser_read_page / browser_read_element / browser_find_text
   * to access page text on demand. This avoids the ELEMENT_OFFSCREEN trap
   * where the model targets a non-WebElement text node and the renderer
   * rejects it via the DynamicTo<WebElement>() cast.
   */
  ACTIONABLE_ONLY: {
    elements: {
      onlyInteractive: true,
      includeTextElements: false,
      onlyVisible: true,
      maxElements: 80,
      maxTextLength: 100,
      excludeTypes: ['root', 'container'],
    },
    screenshot: { include: true },
    includeMetadata: false,
  } satisfies ObservationConfig,

  /**
   * Read-only preset used internally by browser_read_page / find_text /
   * read_element to fetch the FULL element list including text nodes.
   * Never exposed to the LLM via the registry — text-node element_ids that
   * leaked here would still fail an actor click. The read tools strip ids
   * before returning content to the model.
   */
  FULL_FOR_READ: {
    elements: {
      // No interactivity filter — keep paragraphs, headings, list items.
      onlyVisible: true,
      maxElements: 1000,
      excludeTypes: ['root', 'container'],
    },
    screenshot: { include: false },
    includeMetadata: false,
  } satisfies ObservationConfig,

  /** Minimal: interactive elements, no geometry, no screenshot. Cheapest for token budget. */
  MINIMAL: {
    elements: {
      onlyInteractive: true,
      maxElements: 30,
      maxTextLength: 60,
      includeBoundingBoxes: false,
      includeFormDetails: true,
      excludeTypes: ['root', 'container', 'paragraph', 'heading'],
    },
    screenshot: { include: false },
    includeMetadata: false,
  } satisfies ObservationConfig,

  /** Status only: no elements, no screenshot. Just windows/tabs/URL. */
  STATUS_ONLY: {
    skipAsyncObservation: true,
  } satisfies ObservationConfig,
} as const

// ========================= Tab Observation Result =========================

export const TAB_OBSERVATION_RESULTS: Record<number, string> = {
  0: 'OK',
  1: 'TAB_WENT_AWAY',
  2: 'SCREENSHOT_TIMEOUT',
  3: 'PAGE_CRASHED',
  4: 'UNKNOWN_ERROR',
  5: 'WEB_CONTENTS_CHANGED',
  6: 'PAGE_CONTEXT_NOT_ELIGIBLE',
  7: 'FETCH_ERROR',
}

// ========================= ContentAttributeType Map =========================

// Matches ContentAttributeType enum from common_quality_data.proto.
// Values 12-19 are reserved in the proto.
export const CONTENT_ATTRIBUTE_TYPES: Record<number, string> = {
  0: 'unknown',
  1: 'root',
  2: 'container',
  3: 'iframe',
  4: 'paragraph',
  5: 'heading',
  6: 'ordered_list',
  7: 'unordered_list',
  8: 'form',
  9: 'image',
  10: 'table',
  11: 'table_cell',
  20: 'text',
  21: 'table_row',
  22: 'anchor',
  23: 'list_item',
  24: 'form_control',
  25: 'svg',
  26: 'canvas',
  27: 'video',
}

// Matches FormControlType enum from common_quality_data.proto.
// Value 14 is skipped in the proto.
export const FORM_CONTROL_TYPES: Record<number, string> = {
  0: 'unspecified',
  1: 'content_editable',
  2: 'checkbox',
  3: 'email',
  4: 'month',
  5: 'number',
  6: 'password',
  7: 'radio',
  8: 'search',
  9: 'telephone',
  10: 'text',
  11: 'url',
  12: 'select_one',
  13: 'select_multiple',
  15: 'textarea',
  16: 'button',
  17: 'submit',
  18: 'reset',
  19: 'popover',
  20: 'fieldset',
  21: 'input_button',
  22: 'color',
  23: 'date',
  24: 'datetime_local',
  25: 'file',
  26: 'hidden',
  27: 'image',
  28: 'range',
  29: 'input_reset',
  30: 'input_submit',
  31: 'time',
  32: 'week',
  33: 'output',
}

export const ANNOTATED_ROLES: Record<number, string> = {
  0: 'unknown',
  5: 'header',
  6: 'nav',
  7: 'search',
  8: 'main',
  9: 'article',
  10: 'section',
  11: 'aside',
  12: 'footer',
  13: 'content_hidden',
  14: 'paid_content',
}

// ========================= Simple Mode — Unified Result Types =========================

/** Tab info exposed to the LLM in the unified simple tool result. */
export type SimpleTabInfo = {
  tab_id: number
  url?: string
  title?: string
  active: boolean
}

/** Element with a synthetic element_id (replaces raw node_id for simple mode). */
export type SimpleMappedElement = {
  element_id: number
  type: string
  text?: string
  title?: string
  label?: string
  roles?: string[]
  clickable?: boolean
  focusable?: boolean
  disabled?: boolean
  form?: FormControlInfo
  href?: string
  box?: [number, number, number, number]
}

/** Observation section of the unified result. */
export type SimpleObservation = {
  tab_id: number
  url: string
  title: string
  viewport?: BoundingBox
  elements: SimpleMappedElement[]
  has_screenshot: boolean
}

/**
 * One match from browser_find_text. text is the visible string that matched;
 * box is its viewport-pixel rect. clickable_ancestor_id, when present, is the
 * element_id of the nearest clickable/focusable/form_control/anchor ancestor —
 * the model should pass THIS id to browser_click, not the text id, since
 * Chromium's actor pipeline rejects non-WebElement (text) targets with
 * ELEMENT_OFFSCREEN. ancestor_type and ancestor_text help the model verify
 * the resolved ancestor matches its intent.
 */
export type FindTextMatch = {
  text: string
  box?: [number, number, number, number]
  clickable_ancestor_id?: number
  ancestor_type?: string
  ancestor_text?: string
}

/**
 * Unified return type for all simple-mode tools.
 * Every tool returns this same shape — the LLM never has to
 * handle different response formats. Successful action tools may omit tabs
 * when they do not add useful state beyond result/observation.
 *
 * Read-only tools (browser_read_page, browser_read_element, browser_find_text)
 * populate page_text / element_text / matches respectively.
 */
export type SimpleToolResult = {
  ok: boolean
  result: string
  message?: string
  tabs?: SimpleTabInfo[]
  created_tabs?: SimpleTabInfo[]
  observation?: SimpleObservation
  page_text?: string
  element_text?: string
  truncated?: boolean
  matches?: FindTextMatch[]
}
