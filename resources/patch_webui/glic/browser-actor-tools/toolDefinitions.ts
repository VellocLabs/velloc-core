// Browser Actor Tools — LLM Tool Definitions
//
// One unified per-action tool set. Every tool targets at most one tab;
// the optional tab_id parameter selects which tab on tab-scoped tools.
// Element-targeting tools derive the tab from the element_id's source.
// Every tool returns a fresh observation alongside the action result.
// Use the dedicated browser_screenshot tool when you need a visual.
//
// Debug reasoning fields (tool_reason, element_reason) are included only when
// NEXUS_DEBUG is true. Set NEXUS_DEBUG to false in nexusTrace.ts for release
// builds to strip them, reducing both schema and model-output token cost.
//
// Description text is intentionally terse. Every word in a shared constant or
// footer is amplified across 13 tools, so prefer schema (`required`, `enum`)
// over prose, and avoid restating in description what is already in the field
// schema.

import {BROWSER_SKILL_TOOL_DEFINITIONS, isBrowserSkillTool} from './skillDefinitions.js'
import {NEXUS_DEBUG} from '../nexusTrace.js'
import type {HostedToolDefinition} from './types.js'

// =========================================================================
//  Tool-definition accessors
// =========================================================================

export function getBrowserToolDefinitions(): HostedToolDefinition[] {
  return NEXUS_DEBUG
    ? BROWSER_SIMPLE_TOOL_DEFINITIONS
    : BROWSER_SIMPLE_TOOL_DEFINITIONS.map(stripReasoningFields)
}

export function getHostedToolDefinitions(): HostedToolDefinition[] {
  return [
    ...getBrowserToolDefinitions(),
    ...BROWSER_SKILL_TOOL_DEFINITIONS,
  ]
}

export function isBrowserActorTool(toolName: string): boolean {
  return BROWSER_ACTOR_TOOL_NAMES.has(toolName)
}

export function isHostedTool(toolName: string): boolean {
  return isBrowserActorTool(toolName) || isBrowserSkillTool(toolName)
}

export function getCompactableToolNames(): string[] {
  return getBrowserToolDefinitions().map(t => t.name)
}

// =========================================================================
//  Debug reasoning field stripping
// =========================================================================

const REASONING_FIELD_NAMES = new Set(['tool_reason', 'element_reason'])

function stripReasoningFields(def: HostedToolDefinition): HostedToolDefinition {
  const props = def.parameters?.properties
  if (!props) return def

  const hasReasoningField = Object.keys(props).some(k => REASONING_FIELD_NAMES.has(k))
  if (!hasReasoningField) return def

  const newProps = { ...props }
  for (const fieldName of REASONING_FIELD_NAMES) {
    delete newProps[fieldName]
  }

  const newRequired = def.parameters.required?.filter(
    r => !REASONING_FIELD_NAMES.has(r)
  )

  return {
    ...def,
    parameters: {
      ...def.parameters,
      properties: newProps,
      required: newRequired && newRequired.length > 0 ? newRequired : undefined,
    },
  }
}

// =========================================================================
//  Shared parameter properties — repeated across many tools, keep terse
// =========================================================================

const TAB_ID_OPTIONAL_PROPERTY = {
  type: 'integer' as const,
  description: 'Optional. Active tab if absent.',
} as const

// NOTE: Test browser-tool-definition-text.test.ts asserts this string
// contains the substring "previous browser_* tool result".
const SIMPLE_ELEMENT_ID_DESCRIPTION =
  'Id from the latest previous browser_* tool result observation.'

const TOOL_REASON_PROPERTY = {
  type: 'string' as const,
  description: 'Why this call now.',
} as const

const ELEMENT_REASON_PROPERTY = {
  type: 'string' as const,
  description:
    'Why this element_id. Cite observed type and visible text/label.',
} as const

// =========================================================================
//  Description footers — appended to per-tool description text
// =========================================================================
// These strings are amplified by 13 tools, so they are deliberately short.
// Keep in sync with SimpleToolResult / SimpleObservation in types.ts and
// with the catalog entry in nexus_web/src/constants/prompts.ts.

// Used on tools that have at least one required arg in production
// (after stripReasoningFields). The "never {}" deterrent is therefore
// always literally true for tools that use this footer.
const ACTION_FOOTER =
  '\nReturns a fresh observation. Every required arg needs a concrete value; never call with {}. On failure: {ok:false, result:<ERROR_CODE>}.'

// Used on tools whose only required field is the debug-only tool_reason
// (back, forward, close_tab, activate_tab, wait). In production these
// have no required args, so {} is a legitimate call.
const SAFE_FOOTER =
  '\nReturns a fresh observation. On failure: {ok:false, result:<ERROR_CODE>}.'

// browser_create_tab also returns created_tabs alongside the observation.
const CREATE_TAB_FOOTER =
  ACTION_FOOTER + ' Also includes created_tabs:[{tab_id,active}].'

// browser_screenshot is the only tool whose observation carries a screenshot;
// the actual image is delivered to the model out-of-band.
const SCREENSHOT_FOOTER =
  '\nReturns a fresh observation with has_screenshot:true; image is delivered out-of-band.'

// =========================================================================
//  Tool definitions — element_id targeting, flat params, SimpleToolResult
// =========================================================================

export const BROWSER_CLICK_TOOL: HostedToolDefinition = {
  type: 'function',
  name: 'browser_click',
  searchHint: 'browser actor click element',
  description: `Click a page element.${ACTION_FOOTER}`,
  parameters: {
    type: 'object',
    properties: {
      element_id: { type: 'integer', description: SIMPLE_ELEMENT_ID_DESCRIPTION },
      tool_reason: TOOL_REASON_PROPERTY,
      element_reason: ELEMENT_REASON_PROPERTY,
      button: { type: 'string', enum: ['left', 'right'] },
      click_count: { type: 'string', enum: ['single', 'double'] },
    },
    required: ['element_id', 'tool_reason', 'element_reason'],
  },
}

export const BROWSER_TYPE_TOOL: HostedToolDefinition = {
  type: 'function',
  name: 'browser_type',
  searchHint: 'browser actor type into element',
  description: `Type text into a form_control (replaces content); use browser_navigate for URLs.${ACTION_FOOTER}`,
  parameters: {
    type: 'object',
    properties: {
      element_id: { type: 'integer', description: SIMPLE_ELEMENT_ID_DESCRIPTION },
      text: { type: 'string', description: 'Text to type.' },
      tool_reason: TOOL_REASON_PROPERTY,
      element_reason: ELEMENT_REASON_PROPERTY,
      submit: { type: 'boolean', description: 'Press Enter after typing.' },
    },
    required: ['element_id', 'text', 'tool_reason', 'element_reason'],
  },
}

export const BROWSER_SELECT_TOOL: HostedToolDefinition = {
  type: 'function',
  name: 'browser_select',
  searchHint: 'browser actor select option',
  description: `Pick an option from a <select>; value must match exactly.${ACTION_FOOTER}`,
  parameters: {
    type: 'object',
    properties: {
      element_id: { type: 'integer', description: SIMPLE_ELEMENT_ID_DESCRIPTION },
      tab_id: TAB_ID_OPTIONAL_PROPERTY,
      value: { type: 'string', description: 'Option value (case-sensitive).' },
      tool_reason: TOOL_REASON_PROPERTY,
      element_reason: ELEMENT_REASON_PROPERTY,
    },
    required: ['element_id', 'value', 'tool_reason', 'element_reason'],
  },
}

export const BROWSER_NAVIGATE_TOOL: HostedToolDefinition = {
  type: 'function',
  name: 'browser_navigate',
  searchHint: 'browser actor navigate tab',
  description: `Navigate the tab to a URL (preferred over browser_type for URLs).${ACTION_FOOTER}`,
  parameters: {
    type: 'object',
    properties: {
      tab_id: TAB_ID_OPTIONAL_PROPERTY,
      url: { type: 'string', description: 'Target URL.' },
      tool_reason: TOOL_REASON_PROPERTY,
    },
    required: ['url', 'tool_reason'],
  },
}

export const BROWSER_SCROLL_TO_TOOL: HostedToolDefinition = {
  type: 'function',
  name: 'browser_scroll_to',
  searchHint: 'browser actor scroll to element',
  description: `Scroll an element into view.${ACTION_FOOTER}`,
  parameters: {
    type: 'object',
    properties: {
      element_id: { type: 'integer', description: SIMPLE_ELEMENT_ID_DESCRIPTION },
      tab_id: TAB_ID_OPTIONAL_PROPERTY,
      tool_reason: TOOL_REASON_PROPERTY,
      element_reason: ELEMENT_REASON_PROPERTY,
    },
    required: ['element_id', 'tool_reason', 'element_reason'],
  },
}

export const BROWSER_SCROLL_TOOL: HostedToolDefinition = {
  type: 'function',
  name: 'browser_scroll',
  searchHint: 'browser actor scroll page',
  description: `Scroll the viewport, or scroll an element by setting element_id. direction and distance are required.${ACTION_FOOTER}`,
  parameters: {
    type: 'object',
    properties: {
      element_id: {
        type: 'integer',
        description:
          'Id from a previous browser_* tool result observation. Optional; viewport scrolls when absent.',
      },
      tab_id: TAB_ID_OPTIONAL_PROPERTY,
      tool_reason: TOOL_REASON_PROPERTY,
      element_reason: {
        type: 'string',
        description:
          'Required when element_id is set. Cite observed type and text.',
      },
      direction: { type: 'string', enum: ['up', 'down', 'left', 'right'] },
      distance: { type: 'number', description: 'Pixels.' },
    },
    required: ['direction', 'distance', 'tool_reason'],
  },
}

export const BROWSER_WAIT_TOOL: HostedToolDefinition = {
  type: 'function',
  name: 'browser_wait',
  searchHint: 'browser actor wait',
  readOnly: true,
  description: `Wait for page loads or animations.${SAFE_FOOTER}`,
  parameters: {
    type: 'object',
    properties: {
      tab_id: TAB_ID_OPTIONAL_PROPERTY,
      tool_reason: TOOL_REASON_PROPERTY,
      wait_ms: { type: 'integer', description: 'Default 1000.' },
    },
    required: ['tool_reason'],
  },
}

export const BROWSER_BACK_TOOL: HostedToolDefinition = {
  type: 'function',
  name: 'browser_back',
  searchHint: 'browser actor history back',
  description: `Go back in tab history.${SAFE_FOOTER}`,
  parameters: {
    type: 'object',
    properties: {
      tab_id: TAB_ID_OPTIONAL_PROPERTY,
      tool_reason: TOOL_REASON_PROPERTY,
    },
    required: ['tool_reason'],
  },
}

export const BROWSER_FORWARD_TOOL: HostedToolDefinition = {
  type: 'function',
  name: 'browser_forward',
  searchHint: 'browser actor history forward',
  description: `Go forward in tab history.${SAFE_FOOTER}`,
  parameters: {
    type: 'object',
    properties: {
      tab_id: TAB_ID_OPTIONAL_PROPERTY,
      tool_reason: TOOL_REASON_PROPERTY,
    },
    required: ['tool_reason'],
  },
}

export const BROWSER_CREATE_TAB_TOOL: HostedToolDefinition = {
  type: 'function',
  name: 'browser_create_tab',
  searchHint: 'browser actor open url in tab',
  description: `Open a URL in a new tab (preferred over browser_type for URLs).${CREATE_TAB_FOOTER}`,
  parameters: {
    type: 'object',
    properties: {
      url: { type: 'string', description: 'URL to open.' },
      tool_reason: TOOL_REASON_PROPERTY,
      reuse_visible_tab: { type: 'boolean', description: 'Reuse current tab.' },
    },
    required: ['url', 'tool_reason'],
  },
}

export const BROWSER_CLOSE_TAB_TOOL: HostedToolDefinition = {
  type: 'function',
  name: 'browser_close_tab',
  searchHint: 'browser actor close tab',
  description: `Close a tab.${SAFE_FOOTER}`,
  parameters: {
    type: 'object',
    properties: {
      tab_id: TAB_ID_OPTIONAL_PROPERTY,
      tool_reason: TOOL_REASON_PROPERTY,
    },
    required: ['tool_reason'],
  },
}

export const BROWSER_ACTIVATE_TAB_TOOL: HostedToolDefinition = {
  type: 'function',
  name: 'browser_activate_tab',
  searchHint: 'browser actor activate tab',
  description: `Switch to a tab.${SAFE_FOOTER}`,
  parameters: {
    type: 'object',
    properties: {
      tab_id: TAB_ID_OPTIONAL_PROPERTY,
      tool_reason: TOOL_REASON_PROPERTY,
    },
    required: ['tool_reason'],
  },
}

export const BROWSER_SCREENSHOT_TOOL: HostedToolDefinition = {
  type: 'function',
  name: 'browser_screenshot',
  searchHint: 'browser actor capture screenshot',
  readOnly: true,
  description: `Observe with screenshot. Use only when a visual is needed; other browser_* tools already return observations without one.${SCREENSHOT_FOOTER}`,
  parameters: {
    type: 'object',
    properties: {
      tab_id: TAB_ID_OPTIONAL_PROPERTY,
      tool_reason: TOOL_REASON_PROPERTY,
    },
    required: ['tool_reason'],
  },
}

// =========================================================================
//  Read-only tools — fetch page text without invoking the actor pipeline
// =========================================================================
// Action observations include only actionable elements (clickable, focusable,
// form_control, anchor). To read body text, headings, paragraphs, or to act
// on a JS-only widget the model uses these tools. None of them goes through
// the actor pipeline, so none can produce ELEMENT_OFFSCREEN.

const READ_FOOTER =
  '\nReturns plain text in page_text/element_text. On failure: {ok:false, result:<ERROR_CODE>}.'

const FIND_TEXT_FOOTER =
  '\nReturns matches:[{text,box,clickable_ancestor_id?,ancestor_type?,ancestor_text?}]. ' +
  'When clickable_ancestor_id is present, pass it to browser_click — never the text node itself. ' +
  'On failure: {ok:false, result:<ERROR_CODE>}.'

export const BROWSER_READ_PAGE_TOOL: HostedToolDefinition = {
  type: 'function',
  name: 'browser_read_page',
  searchHint: 'browser read page text content',
  readOnly: true,
  description:
    `Get visible page text. Use when the action observation lacks the body content you need.${READ_FOOTER}`,
  parameters: {
    type: 'object',
    properties: {
      tab_id: TAB_ID_OPTIONAL_PROPERTY,
      scope: {
        type: 'string',
        enum: ['viewport', 'full_page'],
        description: 'Default viewport.',
      },
      max_chars: {
        type: 'integer',
        description: 'Default 4000.',
      },
      tool_reason: TOOL_REASON_PROPERTY,
    },
    required: ['tool_reason'],
  },
}

export const BROWSER_READ_ELEMENT_TOOL: HostedToolDefinition = {
  type: 'function',
  name: 'browser_read_element',
  searchHint: 'browser read element text content',
  readOnly: true,
  description:
    `Get the text inside one element by element_id (e.g. read a result card before clicking).${READ_FOOTER}`,
  parameters: {
    type: 'object',
    properties: {
      element_id: { type: 'integer', description: SIMPLE_ELEMENT_ID_DESCRIPTION },
      max_chars: {
        type: 'integer',
        description: 'Default 2000.',
      },
      tool_reason: TOOL_REASON_PROPERTY,
      element_reason: ELEMENT_REASON_PROPERTY,
    },
    required: ['element_id', 'tool_reason', 'element_reason'],
  },
}

export const BROWSER_FIND_TEXT_TOOL: HostedToolDefinition = {
  type: 'function',
  name: 'browser_find_text',
  searchHint: 'browser find clickable element by visible text',
  readOnly: true,
  description:
    `Find visible text on the page and resolve it to a clickable element_id. ` +
    `Use this when the action observation is missing the autocomplete/suggestion/menu item you want to click ` +
    `(common on JS-onclick widgets like search suggestions).${FIND_TEXT_FOOTER}`,
  parameters: {
    type: 'object',
    properties: {
      tab_id: TAB_ID_OPTIONAL_PROPERTY,
      query: {
        type: 'string',
        description: 'Substring to match (case-insensitive) against visible text.',
      },
      max_results: {
        type: 'integer',
        description: 'Default 5.',
      },
      tool_reason: TOOL_REASON_PROPERTY,
    },
    required: ['query', 'tool_reason'],
  },
}

export const BROWSER_SIMPLE_TOOL_DEFINITIONS: HostedToolDefinition[] = [
  BROWSER_CLICK_TOOL,
  BROWSER_TYPE_TOOL,
  BROWSER_SELECT_TOOL,
  BROWSER_NAVIGATE_TOOL,
  BROWSER_SCROLL_TO_TOOL,
  BROWSER_SCROLL_TOOL,
  BROWSER_WAIT_TOOL,
  BROWSER_BACK_TOOL,
  BROWSER_FORWARD_TOOL,
  BROWSER_CREATE_TAB_TOOL,
  BROWSER_CLOSE_TAB_TOOL,
  BROWSER_ACTIVATE_TAB_TOOL,
  BROWSER_SCREENSHOT_TOOL,
  BROWSER_READ_PAGE_TOOL,
  BROWSER_READ_ELEMENT_TOOL,
  BROWSER_FIND_TEXT_TOOL,
]

// =========================================================================
//  Simple tool name → action type mapping
// =========================================================================

export const SIMPLE_TOOL_NAME_TO_ACTION_TYPE: Record<string, string> = {
  browser_click: 'click',
  browser_type: 'type',
  browser_select: 'select',
  browser_navigate: 'navigate',
  browser_scroll_to: 'scroll_to',
  browser_scroll: 'scroll',
  browser_wait: 'wait',
  browser_back: 'back',
  browser_forward: 'forward',
  browser_create_tab: 'create_tab',
  browser_close_tab: 'close_tab',
  browser_activate_tab: 'activate_tab',
}

// Includes screenshot and the read-only tools, which are observation-only
// (not action-mapped via SIMPLE_TOOL_NAME_TO_ACTION_TYPE).
const BROWSER_ACTOR_TOOL_NAMES: ReadonlySet<string> = new Set([
  ...Object.keys(SIMPLE_TOOL_NAME_TO_ACTION_TYPE),
  'browser_screenshot',
  'browser_read_page',
  'browser_read_element',
  'browser_find_text',
])
