// ---------------------------------------------------------------------------
// Minimal Protobuf Binary Codec
// ---------------------------------------------------------------------------
// Implements just enough of the protobuf wire format to encode Actions and
// decode ActionsResult without depending on protobuf.js or google-protobuf.
//
// Wire types:
//   0 = Varint  (int32, int64, uint32, uint64, bool, enum)
//   1 = 64-bit  (fixed64, sfixed64, double)
//   2 = Length-delimited (string, bytes, embedded messages)
//   5 = 32-bit  (fixed32, sfixed32, float)
// ---------------------------------------------------------------------------

// ========================= Encoder =========================

export class ProtoWriter {
  private buf: number[] = []

  // ---- Low-level ----

  private rawByte(b: number): void {
    this.buf.push(b & 0xff)
  }

  private rawBytes(bytes: Uint8Array): void {
    for (const byte of bytes) {
      this.buf.push(byte)
    }
  }

  private varint(value: number): void {
    // Unsigned varint encoding (up to 32-bit positive values).
    let v = value >>> 0
    while (v > 0x7f) {
      this.rawByte((v & 0x7f) | 0x80)
      v >>>= 7
    }
    this.rawByte(v)
  }

  private signedVarint(value: number): void {
    if (value >= 0) {
      this.varint(value)
      return
    }
    // Negative int32: sign-extend to 10-byte varint (protobuf spec).
    let v = value
    for (let i = 0; i < 9; i++) {
      this.rawByte((v & 0x7f) | 0x80)
      v = Math.floor(v / 128)
    }
    this.rawByte(1)
  }

  private tag(field: number, wireType: number): void {
    this.varint((field << 3) | wireType)
  }

  // ---- Field writers ----

  int32(field: number, value: number): void {
    this.tag(field, 0)
    this.signedVarint(value)
  }

  uint32(field: number, value: number): void {
    this.tag(field, 0)
    this.varint(value)
  }

  bool(field: number, value: boolean): void {
    this.tag(field, 0)
    this.varint(value ? 1 : 0)
  }

  string(field: number, value: string): void {
    const encoded = new TextEncoder().encode(value)
    this.tag(field, 2)
    this.varint(encoded.length)
    this.rawBytes(encoded)
  }

  bytes(field: number, value: Uint8Array): void {
    this.tag(field, 2)
    this.varint(value.length)
    this.rawBytes(value)
  }

  float(field: number, value: number): void {
    this.tag(field, 5)
    const ab = new ArrayBuffer(4)
    new DataView(ab).setFloat32(0, value, true)
    this.rawBytes(new Uint8Array(ab))
  }

  int64(field: number, value: bigint): void {
    this.tag(field, 0)
    let v = value & 0xffff_ffff_ffff_ffffn
    while (v > 0x7fn) {
      this.rawByte(Number(v & 0x7fn) | 0x80)
      v >>= 7n
    }
    this.rawByte(Number(v))
  }

  /**
   * Write an embedded message field.
   * The callback receives a fresh writer; its output is length-prefixed.
   */
  message(field: number, writeFn: (w: ProtoWriter) => void): void {
    const inner = new ProtoWriter()
    writeFn(inner)
    const data = inner.finish()
    this.tag(field, 2)
    this.varint(data.length)
    this.rawBytes(data)
  }

  /** Finalize and return the encoded bytes. */
  finish(): Uint8Array {
    return new Uint8Array(this.buf)
  }

  /** Convenience: finish and return as ArrayBuffer (for performActions). */
  toArrayBuffer(): ArrayBuffer {
    const arr = this.finish()
    const copy = new Uint8Array(arr.byteLength)
    copy.set(arr)
    return copy.buffer
  }
}

// ========================= Decoder =========================

export type ProtoField = {
  field: number
  wireType: number
}

export class ProtoReader {
  private data: Uint8Array
  private pos: number
  private end: number

  constructor(data: Uint8Array) {
    this.data = data
    this.pos = 0
    this.end = data.length
  }

  static fromArrayBuffer(buf: ArrayBuffer): ProtoReader {
    return new ProtoReader(new Uint8Array(buf))
  }

  hasMore(): boolean {
    return this.pos < this.end
  }

  // ---- Low-level ----

  readTag(): ProtoField | null {
    if (!this.hasMore()) return null
    const v = this.readVarint()
    return { field: v >>> 3, wireType: v & 7 }
  }

  readVarint(): number {
    let result = 0
    let shift = 0
    while (this.pos < this.end) {
      const byte = this.data[this.pos]
      if (byte === undefined) break
      this.pos++
      result |= (byte & 0x7f) << shift
      if ((byte & 0x80) === 0) return result >>> 0
      shift += 7
      if (shift >= 35) {
        // Drain remaining bytes of an oversized (64-bit) varint.
        while (this.pos < this.end) {
          const currentByte = this.data[this.pos]
          if (currentByte === undefined || (currentByte & 0x80) === 0) break
          this.pos++
        }
        if (this.pos < this.end) this.pos++
        break
      }
    }
    return result >>> 0
  }

  // ---- Typed readers ----

  readInt32(): number {
    const v = this.readVarint()
    return v | 0 // reinterpret as signed
  }

  readUint32(): number {
    return this.readVarint()
  }

  readBool(): boolean {
    return this.readVarint() !== 0
  }

  readString(): string {
    const len = this.readVarint()
    const bytes = this.data.subarray(this.pos, this.pos + len)
    this.pos += len
    return new TextDecoder().decode(bytes)
  }

  readBytes(): Uint8Array {
    const len = this.readVarint()
    const bytes = this.data.slice(this.pos, this.pos + len)
    this.pos += len
    return bytes
  }

  readMessage(): ProtoReader {
    const len = this.readVarint()
    const sub = new ProtoReader(this.data.subarray(this.pos, this.pos + len))
    this.pos += len
    return sub
  }

  readFloat(): number {
    const view = new DataView(this.data.buffer, this.data.byteOffset + this.pos, 4)
    this.pos += 4
    return view.getFloat32(0, true)
  }

  readFixed64(): bigint {
    const view = new DataView(this.data.buffer, this.data.byteOffset + this.pos, 8)
    this.pos += 8
    return view.getBigUint64(0, true)
  }

  /** Skip a field value based on its wire type. */
  skip(wireType: number): void {
    switch (wireType) {
      case 0:
        this.readVarint()
        break
      case 1:
        this.pos += 8
        break
      case 2: {
        const len = this.readVarint()
        this.pos += len
        break
      }
      case 5:
        this.pos += 4
        break
    }
  }
}
