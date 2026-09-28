// GENERATED — DO NOT EDIT.
//
// Mirror of nexus_web/src/browser/runtimeProtocol/codec.ts, copied by
// nexus_web/scripts/sync_runtime_protocol_mirror.py so the outer WebUI page can
// speak the UI <-> Runtime protocol without a hand-written envelope. Edit the
// nexus_web source and re-run the script; `--check` fails when this file and
// its source disagree (nexus_web `protocol-mirror.test.ts`).

/**
 * UI ⇄ Runtime protocol — wire codec.
 *
 * Frames travel as JSON text; blob chunks travel as binary messages laid out
 * `u32be headerLen | header (JSON, UTF-8) | chunk bytes`. That layout is the
 * one thing every transport shares: a WebSocket sends the encoded buffer as
 * one binary message (a copy, unavoidably); a MessagePort transport posts
 * `{header, bytes}` with the `ArrayBuffer` in the transfer list instead and
 * never calls `encodeBlobChunk` at all. Either way the receiver ends up with
 * the same `{header, bytes}` pair, so nothing above the transport knows which
 * path the bytes took.
 *
 * `isFrame` is the port-level gate (the successor of the direct bridge's
 * `claudeBridge: true` tag) and it validates the ENVELOPE completely: kind,
 * every required field, every enumeration (against the same `as const` lists
 * the types are derived from), and every element of a nested collection — so
 * code that trusts the `value is Frame` narrowing never dispatches on an
 * unknown `bye.code` or reads undefined out of `server.capabilities[]`.
 * Two things stay opaque on purpose: `payload` / `result` (capability
 * handlers own those schemas — the hub's own built-in payloads have guards
 * below), and unknown extra keys (additive fields must cross versions without
 * a bump). And because `JSON.stringify` drops a key whose value is
 * `undefined`, an absent `payload` / `result` is accepted and simply reads as
 * `undefined`.
 *
 * The protocol version is checked by the hub, not here: `hello` / `welcome`
 * only need `v` to be a positive integer to be frames, so a client speaking a
 * version this runtime does not serve still gets a `bye` instead of silence.
 */

import {
  BYE_CODES,
  CLIENT_KINDS,
  EVT_FORMS,
  FRAME_KINDS,
  SCOPE_MODES,
  type BlobChunkHeader,
  type BlobRef,
  type Frame,
  type FrameKind,
  type SysPingPayload,
  type SysPutBlobPayload,
  type TopicsSubscribePayload,
  type TopicsUnsubscribePayload,
} from './frames.js'
import { ERROR_CODES, isWireError } from './errors.js'
import { isRouteKey } from './route.js'

type Dict = Record<string, unknown>

function isDict(value: unknown): value is Dict {
  return typeof value === 'object' && value !== null && !Array.isArray(value)
}
const isStr = (value: unknown): value is string => typeof value === 'string'
const isNum = (value: unknown): value is number => typeof value === 'number' && Number.isFinite(value)
const isBool = (value: unknown): value is boolean => typeof value === 'boolean'
const isUint = (value: unknown): value is number => Number.isInteger(value) && (value as number) >= 0
const isStrArray = (value: unknown): value is string[] => Array.isArray(value) && value['every'](isStr)
const isOneOf =
  (allowed: readonly string[]) =>
  (value: unknown): value is string =>
    isStr(value) && allowed.includes(value)

const isClientKind = isOneOf(CLIENT_KINDS)
const isScopeMode = isOneOf(SCOPE_MODES)
const isByeCode = isOneOf(BYE_CODES)
const isEvtForm = isOneOf(EVT_FORMS)

/** A protocol version as carried by `hello` / `welcome`: a positive integer. Which versions are SERVED is the hub's call. */
const isProtocolVersion = (value: unknown): value is number => isUint(value) && value >= 1

function isHelloAuth(value: unknown): boolean {
  if (!isDict(value)) return false
  if (value['mode'] === 'local') return true
  return value['mode'] === 'pair' && isStr(value['pairId'])
}

function isResumeRequest(value: unknown): boolean {
  return (
    isDict(value) &&
    isStr(value['connectionId']) &&
    isDict(value['topics']) &&
    Object.values(value['topics']).every(isUint)
  )
}

function isScope(value: unknown): boolean {
  return (
    isDict(value) &&
    isScopeMode(value['mode']) &&
    (value['sessions'] === 'all' || isStrArray(value['sessions']))
  )
}

function isCapabilityAdvert(value: unknown): boolean {
  return isDict(value) && isStr(value['cap']) && isStrArray(value['acts'])
}

/** Per-kind envelope checks. `payload` / `result` are not inspected (see the header). */
/** The value at `key` when it is a dict, else null — one narrowing step so the
 *  nested reads below are index-signature safe in every strictness setting. */
function dictAt(f: Dict, key: string): Dict | null {
  const value = f[key]
  return isDict(value) ? value : null
}

const REQUIRED: Record<FrameKind, (f: Dict) => boolean> = {
  hello: f =>
    isProtocolVersion(f['v']) &&
    isStr(dictAt(f, 'client')?.['clientId']) &&
    isClientKind(dictAt(f, 'client')?.['kind']) &&
    isHelloAuth(f['auth']) &&
    (f['scopeCeiling'] === undefined || isScopeMode(f['scopeCeiling'])) &&
    (f['resume'] === undefined || isResumeRequest(f['resume'])),
  challenge: f => isStr(f['nonce']),
  auth: f => isStr(f['proof']),
  welcome: f =>
    isProtocolVersion(f['v']) &&
    isStr(f['connectionId']) &&
    isBool(f['resumed']) &&
    isScope(f['scope']) &&
    isStr(dictAt(f, 'server')?.['runtimeVersion']) &&
    Array.isArray(dictAt(f, 'server')?.['capabilities']) &&
    (dictAt(f, 'server')?.['capabilities'] as unknown[]).every(isCapabilityAdvert),
  bye: f => isByeCode(f['code']) && (f['retryAfterMs'] === undefined || isUint(f['retryAfterMs'])),
  req: f => isStr(f['id']) && isRouteKey(f['route']),
  res: f => isStr(f['id']) && (f['ok'] === true || (f['ok'] === false && isWireError(f['error']))),
  cancel: f => isStr(f['id']),
  evt: f =>
    isStr(f['topic']) &&
    isUint(f['seq']) &&
    (f['type'] === undefined || isStr(f['type'])) &&
    isEvtForm(f['form']) &&
    // A batch says so in `form`, so its payload must actually be a list.
    (f['form'] !== 'deltas' || Array.isArray(f['payload'])) &&
    (f['more'] === undefined || f['more'] === true),
  ack: f => isStr(f['id']) && isUint(f['upTo']),
}

export function isFrameKind(value: unknown): value is FrameKind {
  return isStr(value) && (FRAME_KINDS as readonly string[]).includes(value)
}

export function isFrame(value: unknown): value is Frame {
  if (!isDict(value)) return false
  if (!isFrameKind(value['kind'])) return false
  return REQUIRED[value['kind']](value)
}

/**
 * The RECEIVE gate: `isFrame`, except that an enumeration value this build
 * does not know is read as its documented fallback instead of dropping the
 * frame. A later version may add an error code, a scope, a bye code or an
 * event form; a receiver built before it must degrade, not go silent — a
 * dropped `res` hangs its request until the timeout, a dropped `welcome`
 * never connects, a dropped `evt` leaves a seq hole that re-subscribes into
 * the same frame. Only a STRING of the wrong value is tolerated; a missing
 * or mistyped field still fails the frame. Fallbacks (D-2 invariant
 * "receivers tolerate unknown values"):
 *
 * - `res.error.code` → `internal`; the wire's `retryable` is kept and the
 *   original code travels as `unknownCode`.
 * - `welcome.scope.mode`, `hello.scopeCeiling` → `observe` (least; the
 *   advertised capabilities are already filtered by the sender).
 * - `hello.client.kind` → `browser`.
 * - `hello.auth.mode` → a `pair` with no pairing id, which the hub refuses
 *   with `bye unauthorized` instead of leaving the client waiting.
 * - `bye.code` → `shutting_down`: the peer left for a reason we cannot name;
 *   a reconnecting client retries on its backoff, which is how it learns more.
 * - `evt.form` → an empty `deltas` batch: the seq advances, no listener fires.
 *
 * An unknown frame KIND is still dropped: the protocol adds none (invariant 1).
 * Returns a normalized copy when it had to change anything, else `value`.
 */
export function readFrame(value: unknown): Frame | null {
  if (!isDict(value) || !isFrameKind(value['kind'])) return null
  const frame = tolerateUnknownValues(value)
  return REQUIRED[value['kind']](frame) ? (frame as Frame) : null
}

const unknownStr = (value: unknown, known: readonly string[]): value is string =>
  isStr(value) && !known.includes(value)

function tolerateUnknownValues(f: Dict): Dict {
  switch (f['kind']) {
    case 'res': {
      const error = dictAt(f, 'error')
      if (f['ok'] !== false || !error || !unknownStr(error['code'], ERROR_CODES)) return f
      return { ...f, error: { ...error, code: 'internal', unknownCode: error['code'] } }
    }
    case 'welcome': {
      const scope = dictAt(f, 'scope')
      if (!scope || !unknownStr(scope['mode'], SCOPE_MODES)) return f
      return { ...f, scope: { ...scope, mode: 'observe' } }
    }
    case 'hello': {
      let out = f
      const client = dictAt(f, 'client')
      if (client && unknownStr(client['kind'], CLIENT_KINDS)) {
        out = { ...out, client: { ...client, kind: 'browser' } }
      }
      if (unknownStr(f['scopeCeiling'], SCOPE_MODES)) out = { ...out, scopeCeiling: 'observe' }
      const auth = dictAt(f, 'auth')
      if (auth && unknownStr(auth['mode'], ['local', 'pair'])) {
        out = { ...out, auth: { mode: 'pair', pairId: '' } }
      }
      return out
    }
    case 'bye':
      return unknownStr(f['code'], BYE_CODES) ? { ...f, code: 'shutting_down' } : f
    case 'evt': {
      if (!unknownStr(f['form'], EVT_FORMS)) return f
      const { more: _more, ...rest } = f
      return { ...rest, form: 'deltas', payload: [] }
    }
    default:
      return f
  }
}

export function encodeFrame(frame: Frame): string {
  return JSON.stringify(frame)
}

/** `null` for anything that does not parse as a frame. */
export function decodeFrame(text: string): Frame | null {
  let parsed: unknown
  try {
    parsed = JSON.parse(text)
  } catch {
    return null
  }
  return readFrame(parsed)
}

// ---- payloads of the hub's built-in requests ---------------------------------

function isTopicSubscription(value: unknown): boolean {
  return isDict(value) && isStr(value['topic']) && (value['since'] === undefined || isUint(value['since']))
}

/** Payload of `req topics.subscribe`. */
export function isTopicsSubscribePayload(value: unknown): value is TopicsSubscribePayload {
  return isDict(value) && Array.isArray(value['topics']) && value['topics'].every(isTopicSubscription)
}

/** Payload of `req topics.unsubscribe`. */
export function isTopicsUnsubscribePayload(value: unknown): value is TopicsUnsubscribePayload {
  return isDict(value) && isStrArray(value['topics'])
}

/** Payload and result of `req sys.ping`. */
export function isSysPingPayload(value: unknown): value is SysPingPayload {
  return isDict(value) && isNum(value['t'])
}

/** Payload of `req sys.putBlob`. */
export function isSysPutBlobPayload(value: unknown): value is SysPutBlobPayload {
  return isDict(value) && isUint(value['bytes']) && (value['mime'] === undefined || isStr(value['mime']))
}

// ---- blobs -------------------------------------------------------------------

/** Upper bound on the encoded header; a header is three short fields, so this is generous. */
export const BLOB_CHUNK_HEADER_MAX_BYTES = 4096

const textEncoder = new TextEncoder()
const textDecoder = new TextDecoder('utf-8', { fatal: true })

export function isBlobChunkHeader(value: unknown): value is BlobChunkHeader {
  return isDict(value) && isStr(value['id']) && isUint(value['index']) && isBool(value['last'])
}

export function isBlobRef(value: unknown): value is BlobRef {
  if (!isDict(value)) return false
  if (!isStr(value['$blob']) || !isUint(value['bytes'])) return false
  if (value['mime'] !== undefined && !isStr(value['mime'])) return false
  if (value['sha256'] !== undefined && !isStr(value['sha256'])) return false
  return true
}

/**
 * `u32be headerLen | header JSON | bytes`. The header is serialised with a
 * fixed key order (`id`, `index`, `last`) so two implementations produce
 * byte-identical output for the same chunk — that is what the conformance
 * vectors in `vectors/v1.json` pin (generated independently by
 * `scripts/gen_runtime_protocol_vectors.py`).
 */
export function encodeBlobChunk(header: BlobChunkHeader, bytes: Uint8Array): Uint8Array {
  const headerBytes = textEncoder.encode(
    JSON.stringify({ id: header.id, index: header.index, last: header.last }),
  )
  if (headerBytes.byteLength > BLOB_CHUNK_HEADER_MAX_BYTES) {
    throw new RangeError(
      `[protocol] blob chunk header is ${headerBytes.byteLength} bytes; max ${BLOB_CHUNK_HEADER_MAX_BYTES}`,
    )
  }
  const out = new Uint8Array(4 + headerBytes.byteLength + bytes.byteLength)
  new DataView(out.buffer).setUint32(0, headerBytes.byteLength, false)
  out.set(headerBytes, 4)
  out.set(bytes, 4 + headerBytes.byteLength)
  return out
}

/**
 * Inverse of `encodeBlobChunk`. `bytes` is a view into `buf` (no copy).
 * `null` for a truncated buffer, an oversize or malformed header, or a header
 * missing a required field.
 */
export function decodeBlobChunk(
  buf: Uint8Array,
): { header: BlobChunkHeader; bytes: Uint8Array } | null {
  if (buf.byteLength < 4) return null
  const headerLen = new DataView(buf.buffer, buf.byteOffset, buf.byteLength).getUint32(0, false)
  if (headerLen > BLOB_CHUNK_HEADER_MAX_BYTES || 4 + headerLen > buf.byteLength) return null
  let header: unknown
  try {
    header = JSON.parse(textDecoder.decode(buf.subarray(4, 4 + headerLen)))
  } catch {
    return null
  }
  if (!isBlobChunkHeader(header)) return null
  return { header, bytes: buf.subarray(4 + headerLen) }
}
