// GENERATED — DO NOT EDIT.
//
// Mirror of nexus_web/src/browser/runtimeProtocol/frames.ts, copied by
// nexus_web/scripts/sync_runtime_protocol_mirror.py so the outer WebUI page can
// speak the UI <-> Runtime protocol without a hand-written envelope. Edit the
// nexus_web source and re-run the script; `--check` fails when this file and
// its source disagree (nexus_web `protocol-mirror.test.ts`).

/**
 * UI ⇄ Runtime protocol — frame types (protocol version 1).
 *
 * These are the shapes every ClientConnection speaks to the RuntimeService
 * (the SharedWorker), regardless of transport: a MessagePort for the desktop
 * guest and the panel host, a WebSocket → relay → trunk for a phone, another
 * machine's Velloc, or a CLI. One connection carries only `Frame`s (JSON text)
 * and blob chunks (binary, `codec.ts`). The design that motivates every field
 * is P-3.
 *
 * Ten kinds, in three groups:
 *   handshake  hello / challenge / auth / welcome / bye   (before the connection exists)
 *   requests   req / res / cancel
 *   push       evt;  blob flow control  ack
 * Subscribing and heartbeats are ordinary requests on the hub's built-in
 * routes (`topics.subscribe` / `topics.unsubscribe`, `sys.ping`) — a request
 * with a response does not need its own frame kind. The protocol version
 * rides only on `hello` and `welcome`: it is a property of the connection,
 * negotiated once, so no other frame carries `v`.
 *
 * Skeleton status: types plus the pure helpers a unit test can pin. No hub,
 * no transport, no capability is registered on these yet.
 *
 * Zero-dependency by rule: nothing under `src/browser/runtimeProtocol/` imports from
 * outside the directory — `src/__tests__/runtime-protocol.test.ts` fails if
 * one does. The directory has to stay liftable into a package a Lit build or
 * a relay can consume.
 *
 * Versioning: additive changes (a new cap / act / topic / optional field /
 * error code) keep `RUNTIME_PROTOCOL_VERSION`; changing a frame's layout or
 * a field's meaning bumps it. This is the UI ↔ Runtime line, independent of
 * `NEXUS_INTERFACE_VERSION` (C++ ↔ JS).
 *
 * Enumerations are declared once as `as const` arrays and the types are
 * derived from them, so `codec.ts` validates against the same list the type
 * is made of — there is no second copy to drift.
 */

import type { WireError } from './errors.js'
import type { RouteKey } from './route.js'

export const RUNTIME_PROTOCOL_VERSION = 1 as const

export type TopicId = string

/**
 * What kind of shell the client is — not how much it is trusted. Another
 * machine's Velloc is a `desktop-shell` too, but it arrives over a trunk with
 * `pair` auth and can never hold `full` scope.
 */
export const CLIENT_KINDS = ['desktop-shell', 'panel-host', 'mobile', 'browser', 'cli'] as const
export type ClientKind = (typeof CLIENT_KINDS)[number]

export const SCOPE_MODES = ['observe', 'control', 'manager', 'full'] as const
export type ScopeMode = (typeof SCOPE_MODES)[number]

export type Scope = {
  readonly mode: ScopeMode
  /** `'all'`, or exactly the session ids this connection may see and act on. */
  readonly sessions: 'all' | readonly string[]
}

export type ClientDescriptor = {
  /** Stable install identifier; survives reconnects. */
  clientId: string
  kind: ClientKind
  displayName?: string
  /** Bundle build stamp (`__NEXUS_BUILD_TIME__`); diagnostics only. */
  build?: string
  /**
   * The browser's `navigator.userAgent`, sent by a client that is NOT this
   * machine's own shell — a remote page, which the desktop's Remote access
   * page lists by device (F-24). It is how "3 devices" becomes "Chrome on
   * Android / Safari on macOS": the peer IP is known only to the C++
   * listener, and the UA is the one identifying fact the client itself can
   * state. Diagnostics only — it grants nothing and is never trusted.
   *
   * The desktop shell omits it: it is not a device anyone needs listed, and
   * an absent field keeps its `remote/state` row free of a UA nobody reads.
   */
  userAgent?: string
  /** desktop-shell / panel-host: the panel this client belongs to (from the nexus-init payload). */
  panelId?: string
}

export type HelloAuth =
  /** Accepted only on a MessagePort transport — the port itself is the trust boundary. */
  | { mode: 'local' }
  /** Remote: followed by `challenge` → `auth` (HMAC over the nonce with the pairing secret). */
  | { mode: 'pair'; pairId: string }

export type ResumeRequest = {
  connectionId: string
  /** Last `seq` seen per topic; the broker replays or re-snapshots from there. */
  topics: Record<TopicId, number>
}

// ---- handshake ---------------------------------------------------------------

export type HelloFrame = {
  kind: 'hello'
  /**
   * The `RUNTIME_PROTOCOL_VERSION` the client speaks. A hello whose version
   * this runtime does not serve is still a valid frame — the hub answers it
   * with `bye protocol_too_old | protocol_too_new` instead of dropping it.
   */
  v: number
  client: ClientDescriptor
  auth: HelloAuth
  /**
   * Ask for LESS than the transport would grant. The hub clamps the granted
   * scope down to this and never up: a desktop guest connects as `control`
   * (`?rtScope=control`) so the scope table is exercised without a remote
   * client. Absent = whatever the auth mode grants.
   */
  scopeCeiling?: ScopeMode
  resume?: ResumeRequest
}

export type ChallengeFrame = { kind: 'challenge'; nonce: string }

/** `proof = base64url(HMAC-SHA256(pairSecret, nonce + '\n' + clientId))`. */
export type AuthFrame = { kind: 'auth'; proof: string }

export type CapabilityAdvert = { cap: string; acts: readonly string[] }

export type WelcomeFrame = {
  kind: 'welcome'
  /** The protocol version this connection will speak (the server's choice). */
  v: number
  connectionId: string
  resumed: boolean
  scope: Scope
  server: {
    runtimeVersion: string
    /** Already filtered by `scope`, at action granularity — what `can(cap, act)` reads. */
    capabilities: readonly CapabilityAdvert[]
  }
  /** Only when `resumed` is false; a resumed connection relies on topic replay. */
  bootstrap?: {
    homeDir: string
    platform: string
    pathSep: '/' | '\\'
    /**
     * Who the runtime is running as — the DESKTOP's identity, for a client
     * that is somewhere else (F-24). A remote page shows it in the topbar's
     * connection tooltip, so the user can tell which machine the session
     * they are driving actually runs on. Both fields are best-effort: an
     * older host binary reports no machine name, and a runtime whose host
     * call failed reports neither. Absent renders as one less line, never as
     * a blank row.
     */
    hostName?: string
    /** The desktop's operating system, human-readable (`Windows 11`). */
    osName?: string
  }
}

export const BYE_CODES = [
  'protocol_too_old',
  'protocol_too_new',
  'unauthorized',
  'replaced',
  'shutting_down',
  /**
   * The runtime never came up: the SharedWorker entry stub could not load the
   * runtime body (`agentRuntimeConnectGate.ts`). Final — there is nothing to
   * resume into; the page renders its runtime-connection-failed state.
   */
  'runtime_unavailable',
] as const
export type ByeCode = (typeof BYE_CODES)[number]

export type ByeFrame = { kind: 'bye'; code: ByeCode; retryAfterMs?: number }

// ---- request / response ------------------------------------------------------

/**
 * `payload` (and `res.result`) are typed as present because senders always
 * set them — but `JSON.stringify` drops a key whose value is `undefined`, so
 * a receiver reads an absent key as `undefined` and never rejects the frame
 * for it (`codec.ts`). `payload` / `result` are opaque to the protocol;
 * capability handlers own their schemas.
 */
export type ReqFrame = { kind: 'req'; id: string; route: RouteKey; payload: unknown }

export type ResFrame =
  | { kind: 'res'; id: string; ok: true; result: unknown }
  | { kind: 'res'; id: string; ok: false; error: WireError }

/** Either direction: aborts the request with this id (`HandlerRequest.signal` on the server). */
export type CancelFrame = { kind: 'cancel'; id: string }

// ---- built-in requests served by the hub itself ------------------------------

/** `req topics.subscribe` — payload `TopicsSubscribePayload`; the `res` only confirms, data follows as `evt`. */
export const TOPICS_SUBSCRIBE_ROUTE: RouteKey = 'topics.subscribe'
/** `req topics.unsubscribe` — payload `TopicsUnsubscribePayload`. */
export const TOPICS_UNSUBSCRIBE_ROUTE: RouteKey = 'topics.unsubscribe'
/** `req sys.ping` — payload and result are both `SysPingPayload`; end-to-end liveness + RTT. */
export const SYS_PING_ROUTE: RouteKey = 'sys.ping'
/**
 * `req sys.putBlob` — payload `SysPutBlobPayload`, result a `BlobRef`: the
 * client announces bytes it is about to push, then sends them as chunks under
 * the returned id (the runtime `ack`s), then names the ref in a later request
 * (`fsx.commitUpload`). The ONE upload path; there is no inline byte form.
 */
export const SYS_PUT_BLOB_ROUTE: RouteKey = 'sys.putBlob'

export type TopicSubscription = { topic: TopicId; since?: number }
export type TopicsSubscribePayload = { topics: readonly TopicSubscription[] }
export type TopicsUnsubscribePayload = { topics: readonly TopicId[] }
export type SysPingPayload = { t: number }
export type SysPutBlobPayload = { bytes: number; mime?: string }

// ---- push --------------------------------------------------------------------

/**
 * `deltas` is a BATCH: `payload` is an array of delta payloads for this topic,
 * in order, and the frame takes one `seq` like any other event. Without the
 * marker a batch is ambiguous — a topic whose delta is itself an array (a page
 * of messages) reads identically — which is why it is a form and not a
 * convention. A receiver that sees `deltas` hands each element to its
 * listeners as one delta.
 */
export const EVT_FORMS = ['snapshot', 'delta', 'deltas'] as const
export type EvtForm = (typeof EVT_FORMS)[number]

export type EvtFrame = {
  kind: 'evt'
  topic: TopicId
  /** Topic-global, monotonic; assigned by the broker at publish time. */
  seq: number
  /**
   * The event kind within the topic (`updated`, `removed`, `transcriptPage`…).
   * Omitted when the payload already carries its own discriminator — an
   * `SDKMessage` on a `stream` topic has `type`, so the envelope does not
   * repeat it.
   */
  type?: string
  /** One event's payload — or, when `form` is `deltas`, an array of them. */
  payload: unknown
  form: EvtForm
  /** Snapshot paging: another snapshot page follows before deltas resume. */
  more?: true
}

// ---- blob flow control -------------------------------------------------------

/** Receiver → sender: chunks with `index <= upTo` of blob `id` have arrived. */
export type AckFrame = { kind: 'ack'; id: string; upTo: number }

// ---- the union ---------------------------------------------------------------

export type Frame =
  | HelloFrame
  | ChallengeFrame
  | AuthFrame
  | WelcomeFrame
  | ByeFrame
  | ReqFrame
  | ResFrame
  | CancelFrame
  | EvtFrame
  | AckFrame

export const FRAME_KINDS = [
  'hello',
  'challenge',
  'auth',
  'welcome',
  'bye',
  'req',
  'res',
  'cancel',
  'evt',
  'ack',
] as const

export type FrameKind = (typeof FRAME_KINDS)[number]

type AssertEqual<A, B> = [A] extends [B] ? ([B] extends [A] ? true : never) : never
// Compile-time pin: a frame type added to the union without its kind in
// FRAME_KINDS (or the reverse) turns this into `never` and typecheck fails.
const frameKindsCoverUnion: AssertEqual<FrameKind, Frame['kind']> = true
void frameKindsCoverUnion

/**
 * Every top-level key each frame kind may carry, required and optional alike.
 * Pinned to the TS types at compile time (below) and to the conformance
 * vectors by `runtime-protocol.test.ts` (every vector key must be listed here;
 * every key listed here must appear in some vector) — so a field added to a
 * type, the key table, or the vectors without the other two fails instead of
 * drifting. The wire itself stays open to additive keys: `isFrame` does not
 * reject unknown keys, or an older runtime could not talk to a newer client.
 */
export const FRAME_KEYS = {
  hello: ['kind', 'v', 'client', 'auth', 'scopeCeiling', 'resume'],
  challenge: ['kind', 'nonce'],
  auth: ['kind', 'proof'],
  welcome: ['kind', 'v', 'connectionId', 'resumed', 'scope', 'server', 'bootstrap'],
  bye: ['kind', 'code', 'retryAfterMs'],
  req: ['kind', 'id', 'route', 'payload'],
  res: ['kind', 'id', 'ok', 'result', 'error'],
  cancel: ['kind', 'id'],
  evt: ['kind', 'topic', 'seq', 'type', 'payload', 'form', 'more'],
  ack: ['kind', 'id', 'upTo'],
} as const satisfies Record<FrameKind, readonly string[]>

type FrameOf<K extends FrameKind> = Extract<Frame, { kind: K }>
// Distributive keyof: `res` is a union of two variants and must contribute
// the keys of both (`result` and `error`), not only their intersection.
type KeysOfUnion<T> = T extends unknown ? keyof T : never
type SameKeys<A, B> = [A] extends [B] ? ([B] extends [A] ? true : false) : false
type FrameKeysMatchTypes = {
  [K in FrameKind]: SameKeys<(typeof FRAME_KEYS)[K][number], KeysOfUnion<FrameOf<K>>>
}
// Compile-time pin: one mismatching kind makes the indexed union `boolean`,
// which no longer extends `[true]`, and the assignment below fails typecheck.
// (`false`, not `never`, on mismatch — `never` would vanish from the union.)
const frameKeysMatchTypes: [FrameKeysMatchTypes[FrameKind]] extends [true] ? true : never = true
void frameKeysMatchTypes

// ---- blobs -------------------------------------------------------------------

/**
 * The only representation bytes have inside a payload. The side that owns the
 * bytes pushes chunks (see `codec.ts`) right after the frame carrying the ref;
 * the other side acks. No inline base64, no per-route policy.
 */
export type BlobRef = { $blob: string; bytes: number; mime?: string; sha256?: string }

export type BlobChunkHeader = { id: string; index: number; last: boolean }
