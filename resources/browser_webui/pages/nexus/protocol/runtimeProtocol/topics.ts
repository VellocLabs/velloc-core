// GENERATED — DO NOT EDIT.
//
// Mirror of nexus_web/src/browser/runtimeProtocol/topics.ts, copied by
// nexus_web/scripts/sync_runtime_protocol_mirror.py so the outer WebUI page can
// speak the UI <-> Runtime protocol without a hand-written envelope. Edit the
// nexus_web source and re-run the script; `--check` fails when this file and
// its source disagree (nexus_web `protocol-mirror.test.ts`).

/**
 * UI ⇄ Runtime protocol — topic ids.
 *
 * A topic is a string the TopicBroker keys its `seq` line and replay ring on.
 * The builders here are the only place the shapes are spelled out; `parseTopic`
 * is what a `TopicProvider.authorize` implementation uses to decide whether a
 * connection may see a topic (e.g. `session/<id>/…` against `scope.sessions`).
 *
 * The builders and `parseTopic` are inverses, and the builders enforce it:
 * a segment that is empty or contains `/` would produce an id the parser
 * cannot read back, so it is rejected where the string is made (a developer
 * error, hence a throw) instead of surfacing later as a silent
 * `forbidden_scope` on the server.
 */

import type { TopicId } from './frames.js'

export const SESSION_TOPIC_KINDS = ['state', 'transcript', 'stream', 'diff'] as const
export type SessionTopicKind = (typeof SESSION_TOPIC_KINDS)[number]

/** One `/`-separated part of a topic id: non-empty, slash-free. */
export function isTopicSegment(value: unknown): value is string {
  return typeof value === 'string' && value.length > 0 && !value.includes('/')
}

function segment(value: string, what: string): string {
  if (!isTopicSegment(value)) {
    throw new RangeError(
      `[protocol] topic ${what} must be non-empty and contain no '/': ${JSON.stringify(value)}`,
    )
  }
  return value
}

export const topic = {
  /** Session list + summary (`sessions.updated` / `sessions.removed` deltas). */
  sessions: (): TopicId => 'sessions',
  /** One session's state snapshot, persisted transcript, live stream, or diff history. */
  session: (sessionId: string, kind: SessionTopicKind): TopicId =>
    `session/${segment(sessionId, 'sessionId')}/${kind}`,
  /** One tool call's detail record. */
  sessionTool: (sessionId: string, toolCallId: string): TopicId =>
    `session/${segment(sessionId, 'sessionId')}/tool/${segment(toolCallId, 'toolCallId')}`,
  /** Background tasks table. */
  tasks: (): TopicId => 'tasks',
  /** `<cap>.changed` fan-outs from the settings capabilities. */
  settings: (cap: string): TopicId => `settings/${segment(cap, 'cap')}`,
  /** Shell bus for one panel host: layout / bootProgress / buildInfo / dismissed. */
  shell: (panelId: string): TopicId => `shell/${segment(panelId, 'panelId')}`,
  notifications: (): TopicId => 'notifications',
  /** The user's workspace index (`workspace` capability): the whole state, snapshot and `state` deltas. */
  workspace: (): TopicId => 'workspace',
  presence: (): TopicId => 'presence',
  /** Worker-realm tracer spans (full scope only). */
  diagTrace: (): TopicId => 'diag/trace',
  /** What became of every session's mods (hooks modules): `mods/state`. */
  mods: (): TopicId => 'mods/state',
  /** One session's mod-authored lines (status, notices) and toasts. */
  modsUi: (sessionId: string): TopicId => `mods/ui/${segment(sessionId, 'sessionId')}`,
} as const

export type ParsedTopic =
  | { family: 'sessions' }
  | { family: 'session'; sessionId: string; kind: SessionTopicKind }
  | { family: 'sessionTool'; sessionId: string; toolCallId: string }
  | { family: 'tasks' }
  | { family: 'settings'; cap: string }
  | { family: 'shell'; panelId: string }
  | { family: 'notifications' }
  | { family: 'workspace' }
  | { family: 'presence' }
  | { family: 'diagTrace' }
  | { family: 'mods' }
  | { family: 'modsUi'; sessionId: string }

function isSessionTopicKind(value: string): value is SessionTopicKind {
  return (SESSION_TOPIC_KINDS as readonly string[]).includes(value)
}

/** Inverse of the builders; `null` for anything they would not have produced. */
export function parseTopic(id: string): ParsedTopic | null {
  if (id === 'sessions') return { family: 'sessions' }
  if (id === 'tasks') return { family: 'tasks' }
  if (id === 'notifications') return { family: 'notifications' }
  if (id === 'workspace') return { family: 'workspace' }
  if (id === 'presence') return { family: 'presence' }
  if (id === 'diag/trace') return { family: 'diagTrace' }
  if (id === 'mods/state') return { family: 'mods' }

  const parts = id.split('/')
  if (!parts.every(isTopicSegment)) return null

  // Bound each segment once: an index read is `string | undefined` wherever
  // `noUncheckedIndexedAccess` is on (the Chromium WebUI build, which compiles
  // the mirror of this file), and `every(isTopicSegment)` above does not narrow
  // the elements themselves.
  const [head, second, third, fourth] = parts
  if (head === 'session' && second) {
    if (parts.length === 3 && third && isSessionTopicKind(third)) {
      return { family: 'session', sessionId: second, kind: third }
    }
    if (parts.length === 4 && third === 'tool' && fourth) {
      return { family: 'sessionTool', sessionId: second, toolCallId: fourth }
    }
    return null
  }
  if (head === 'settings' && parts.length === 2 && second) {
    return { family: 'settings', cap: second }
  }
  if (head === 'shell' && parts.length === 2 && second) {
    return { family: 'shell', panelId: second }
  }
  if (head === 'mods' && second === 'ui' && parts.length === 3 && third) {
    return { family: 'modsUi', sessionId: third }
  }
  return null
}
