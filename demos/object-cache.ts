// Retained object cache protocol (GLASSLYCFW/38, g2flash patches/scene.c).
//
// Objects and assets carry 32-bit ids and versions. A view is one SHOW of
// (id, version) references in paint order; the glasses redraw from what they
// retain and answer with a reply (transport kind 5) before the ACK. Every
// message starts [mode][epoch:u16][request:u16]; encoders here leave the epoch
// as 0 and the link fills in the session epoch (`withEpoch`) so the same bytes
// can be dumped offline and replayed by patches/host/vector_host_test.c.
// No mobile dependencies.

export const MODE = { PUT: 37, SHOW: 38, HIDE: 39, STATE: 40, CONTROL: 41 } as const;
export const ASSET = { IMAGE: 0, FONT: 1, STRING: 2 } as const;
export const T = {
  LINE: 1, RECT: 2, RECT_FILL: 3, CIRCLE: 4, CIRCLE_FILL: 5, TRI: 6, TRI_FILL: 7, QUAD: 8, QUAD_FILL: 9,
  BEZIER2: 10, BEZIER3: 11, ARC: 12, PIE: 13, IMAGE: 14, TEXT: 15, TEXT_CACHED: 16, TEXT_INLINE: 17, PATH: 18,
} as const;
export const VISIBLE = 1;
export const ALL_OBJECTS = 0xffffffff;
export const SHOW_FLAGS = { PRESENT: 0x01, KEEP: 0x02, TAG: 0x04, FREEZE: 0x08 } as const;
export const REPLY = { APPLIED: 0, STALE: 1, MISSING: 2, CAPACITY: 3, REFUSED: 4, UNCHANGED: 5, DELTA: 6, SNAPSHOT: 7, RESET: 8 } as const;
export const REFUSE = ["", "duplicate", "asset", "target", "mask", "active", "edges", "keep", "upload", "kind", "id"];
export const FRAME_PERIOD_MS = 33;
export const MAX_OBJECTS = 256;
export const MAX_INLINE_BYTES = 128;
/** CSS cubic-bezier control points scaled to 0..255. */
export const EASE = { linear: [0, 0, 255, 255], inOut: [107, 0, 148, 255], out: [0, 0, 148, 255], in: [107, 0, 255, 255], ease: [64, 26, 64, 255] } as const;

export const u16 = (v: number) => [v & 0xff, (v >>> 8) & 0xff];
export const i16 = (v: number) => u16(v < 0 ? v + 0x10000 : v);
export const u32 = (v: number) => [v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff];
const int = (v: number, min: number, max: number, name: string): number => {
  if (!Number.isInteger(v) || v < min || v > max) throw new Error(`${name} outside ${min}..${max}: ${v}`);
  return v;
};
const id32 = (v: number, name = "id") => int(v, 1, 0xfffffffe, name);
const ver32 = (v: number) => int(v, 1, 0x7fffffff, "version");

/** FNV-1a over a string, folded into a nonzero 31-bit value: a stable id or a content version. */
export function hash31(s: string): number {
  let h = 0x811c9dc5;
  for (let i = 0; i < s.length; i++) { h ^= s.charCodeAt(i); h = Math.imul(h, 0x01000193) >>> 0; }
  h &= 0x7fffffff;
  return h === 0 ? 1 : h;
}

// ---- shape records (shapes.h) ----------------------------------------------------------------
/** Geometric record: [type][flags][color][width][p0..p7 i16]. */
export function record(type: number, color: number, width: number, ...p: number[]): number[] {
  int(type, 1, 13, "geometric shape");
  const out = [type, VISIBLE, int(color, 0, 255, "color"), int(width, 0, 255, "width")];
  if (p.length > 8) throw new Error("At most eight shape parameters");
  for (let i = 0; i < 8; i++) out.push(...i16(int(p[i] ?? 0, -32768, 32767, "parameter")));
  return out;
}
/** [14][flags][options][0][x][y][image asset:u32] */
export const imageRecord = (options: number, x: number, y: number, asset: number) =>
  [T.IMAGE, VISIBLE, options & 0xff, 0, ...i16(x), ...i16(y), ...u32(id32(asset, "asset"))];
/** [15][flags][options][0][x][y][string asset:u32]: built-in font */
export const textRecord = (options: number, x: number, y: number, asset: number) =>
  [T.TEXT, VISIBLE, options & 0xff, 0, ...i16(x), ...i16(y), ...u32(id32(asset, "asset"))];
/** [16][flags][options][0][x][y][string asset:u32][font asset:u32] */
export const textCachedRecord = (options: number, x: number, y: number, asset: number, font: number) =>
  [T.TEXT_CACHED, VISIBLE, options & 0xff, 0, ...i16(x), ...i16(y), ...u32(id32(asset, "asset")), ...u32(id32(font, "font"))];
/** UTF-8 bytes of a line without firmware control bytes (1..31), cut at a codepoint boundary. */
export function textBytes(line: string, limit = MAX_INLINE_BYTES): number[] {
  const out: number[] = [];
  const enc = new TextEncoder();
  for (const ch of line) {
    if (ch.codePointAt(0)! < 32) continue;
    const bytes = enc.encode(ch);
    if (out.length + bytes.length > limit) break;
    out.push(...bytes);
  }
  return out;
}
/** [17][flags][options][0][x][y][w][h][len][bytes]: built-in font, clipped to w/h when > 0. */
export function inlineText(options: number, x: number, y: number, w: number, h: number, s: string | number[]): number[] {
  const bytes = typeof s === "string" ? textBytes(s) : s.slice(0, MAX_INLINE_BYTES);
  if (!bytes.length) throw new Error("inline text needs at least one byte");
  return [T.TEXT_INLINE, VISIBLE, options & 0xff, 0, ...i16(x), ...i16(y), ...i16(w), ...i16(h), bytes.length, ...bytes];
}
/** [18][flags][color][rule][x][y][scale:u16][len:u16][commands]: compiled path (VECTOR_PROTOCOL.md). */
export function pathRecord(commands: number[], rule: "nonzero" | "evenodd" = "nonzero", x = 0, y = 0, scale = 1, color = 15): number[] {
  int(commands.length, 1, 8192, "path bytes");
  return [T.PATH, VISIBLE, int(color, 0, 15, "color"), rule === "evenodd" ? 1 : 0,
    ...i16(int(x, -32768, 32767, "x")), ...i16(int(y, -32768, 32767, "y")),
    ...u16(int(Math.round(scale * 256), 0, 2048, "scale")), ...u16(commands.length), ...commands];
}

// ---- PUT entries -------------------------------------------------------------------------------
/** [0][id][version][kind][total][offset][len][data] */
export function putAsset(id: number, version: number, kind: number, data: ArrayLike<number>, total = data.length, offset = 0): number[] {
  int(kind, 0, 2, "asset kind"); int(data.length, 1, 65535, "chunk length");
  return [0, ...u32(id32(id, "asset")), ...u32(ver32(version)), kind, ...u32(int(total, 1, 256 * 1024, "asset length")),
    ...u32(int(offset, 0, total - data.length, "chunk offset")), ...u16(data.length), ...Array.from(data)];
}
/** [1][id][version][record] */
export const putObject = (id: number, version: number, rec: number[]) => [1, ...u32(id32(id)), ...u32(ver32(version)), ...rec];

export interface Ref { id: number; version: number }

// ---- ops (addressed by object id; ALL_OBJECTS for 2/3/4/6/7) -------------------------------------
export const ops = {
  visible: (id: number, on: boolean) => [2, ...u32(id), on ? 1 : 0],
  move: (id: number, dx: number, dy: number) => [3, ...u32(id), ...i16(dx), ...i16(dy)],
  glide: (id: number, dx: number, dy: number, frames: number, curve: ArrayLike<number>) =>
    [4, ...u32(id), ...i16(dx), ...i16(dy), int(frames, 0, 255, "frames"), ...Array.from(curve).slice(0, 4)],
  /** mask bits 0-7 = p0..p7, 0x100 color, 0x200 width; one i16 value per set bit, low bit first */
  tween: (id: number, mask: number, frames: number, curve: ArrayLike<number>, values: number[]) => {
    let bits = 0; for (let k = 0; k < 10; k++) if (mask & (1 << k)) bits++;
    if (bits !== values.length || !mask || mask & ~0x3ff) throw new Error(`tween mask ${mask} needs ${bits} values, got ${values.length}`);
    return [5, ...u32(id), ...u16(mask), int(frames, 0, 255, "frames"), ...Array.from(curve).slice(0, 4), ...values.flatMap(i16)];
  },
  freeze: (id: number) => [6, ...u32(id)],
  finish: (id: number) => [7, ...u32(id)],
  rotate: (id: number, degrees: number, px: number, py: number, durationMs = 0, curve: ArrayLike<number> = EASE.linear) =>
    [9, ...u32(id), ...u32(int(Math.round(degrees * 256), -9216000, 9216000, "angle") >>> 0),
      ...i16(int(px, -32768, 32767, "pivot x")), ...i16(int(py, -32768, 32767, "pivot y")),
      ...u16(int(durationMs, 0, 65535, "duration")), ...Array.from(curve).slice(0, 4)],
};

// ---- messages (epoch 0, filled in by the link) ---------------------------------------------------
const header = (mode: number, request: number) => [mode, 0, 0, ...u16(int(request, 0, 65535, "request"))];

export const put = (entries: number[][], request = 0) => Uint8Array.from([...header(MODE.PUT, request), ...entries.flat()]);

export interface ShowOptions {
  request?: number; present?: boolean; keep?: boolean; tag?: boolean; freeze?: boolean; bg?: number;
  puts?: number[][]; refs?: Ref[]; ops?: number[][];
}
export function show(o: ShowOptions): Uint8Array {
  const puts = o.puts ?? [], refs = o.refs ?? [], opList = o.ops ?? [];
  int(puts.length, 0, 255, "embedded puts");
  if (o.keep && refs.length) throw new Error("KEEP shows carry no references");
  int(refs.length, 0, MAX_OBJECTS, "references");
  const flags = (o.present === false ? 0 : SHOW_FLAGS.PRESENT) | (o.keep ? SHOW_FLAGS.KEEP : 0) | (o.tag ? SHOW_FLAGS.TAG : 0) | (o.freeze ? SHOW_FLAGS.FREEZE : 0);
  return Uint8Array.from([...header(MODE.SHOW, o.request ?? 0), flags, int(o.bg ?? 0, 0, 15, "background"), puts.length, ...puts.flat(),
    ...u16(refs.length), ...refs.flatMap((r) => [...u32(id32(r.id)), ...u32(ver32(r.version))]), ...opList.flat()]);
}
export const hide = (o: { request?: number; present?: boolean; tag?: boolean; bg?: number } = {}) =>
  Uint8Array.from([...header(MODE.HIDE, o.request ?? 0), (o.present === false ? 0 : SHOW_FLAGS.PRESENT) | (o.tag ? SHOW_FLAGS.TAG : 0), int(o.bg ?? 0, 0, 15, "background")]);
/** [40][epoch][revision:u32][page:u16] */
export const state = (revision: number, page = 0) => Uint8Array.from([MODE.STATE, 0, 0, ...u32(revision >>> 0), ...u16(int(page, 0, 65535, "page"))]);
export const control = {
  /** The link fills the epoch: a RESET adopts it as the session epoch on every lens. `replyCapacity` declares how many bytes one notification carries on this link. */
  reset: (request = 0, replyCapacity = 0) => Uint8Array.from([...header(MODE.CONTROL, request), 0, ...(replyCapacity ? [int(replyCapacity, 16, 255, "reply capacity")] : [])]),
  drop: (ids: number[], request = 0) => Uint8Array.from([...header(MODE.CONTROL, request), 1, int(ids.length, 0, 255, "ids"), ...ids.flatMap((id) => u32(id32(id)))]),
  period: (ms: number, request = 0) => Uint8Array.from([...header(MODE.CONTROL, request), 2, int(ms, 10, 250, "period")]),
};
export const isCacheMessage = (m: Uint8Array) => m.length >= 5 && m[0]! >= MODE.PUT && m[0]! <= MODE.CONTROL;
/**
 * Copy of `m` with the session epoch filled in and, when the encoder left the
 * request id 0 (PUT/SHOW/HIDE/CONTROL), `request` stamped: the firmware treats
 * a SHOW or HIDE repeating the last applied request id as a retransmission and
 * applies nothing, so every distinct message needs its own id. Non-cache
 * messages are returned as-is.
 */
export function withEpoch(m: Uint8Array, epoch: number, request = 0): Uint8Array {
  if (!isCacheMessage(m)) return m;
  const out = Uint8Array.from(m);
  out[1] = epoch & 0xff; out[2] = (epoch >>> 8) & 0xff;
  if (request && m[0] !== MODE.STATE && m[3] === 0 && m[4] === 0) { out[3] = request & 0xff; out[4] = (request >>> 8) & 0xff; }
  return out;
}

// ---- replies (kind-5 payload after the 5-byte transport prefix) ----------------------------------
export interface Reply {
  mode: number; request: number; status: number; epoch: number; revision: number;
  evicted?: number; reason?: number; missing?: number[]; more?: boolean;
  /** RESET replies: the store the glasses could allocate, in KiB (256 normally; less on a tight heap). */
  storeKiB?: number;
  entries?: { asset: boolean; id: number; version: number; lastUse?: number }[]; total?: number; page?: number;
}
export function parseReply(p: Uint8Array): Reply | null {
  if (p.length < 10) return null;
  const rd16 = (i: number) => p[i]! | (p[i + 1]! << 8);
  const rd32 = (i: number) => (p[i]! | (p[i + 1]! << 8) | (p[i + 2]! << 16) | (p[i + 3]! << 24)) >>> 0;
  const r: Reply = { mode: p[0]!, request: rd16(1), status: p[3]!, epoch: rd16(4), revision: rd32(6) };
  const x = 10;
  switch (r.status) {
    case REPLY.APPLIED: case REPLY.CAPACITY:
      if (p.length > x) r.evicted = p[x]!;
      if (r.mode === MODE.CONTROL && p.length >= x + 3) r.storeKiB = rd16(x + 1);
      break;
    case REPLY.REFUSED: if (p.length > x) r.reason = p[x]!; break;
    case REPLY.MISSING: {
      if (p.length < x + 2) break;
      const n = p[x]!; r.more = p[x + 1] !== 0; r.missing = [];
      for (let i = 0; i < n && x + 2 + 4 * i + 4 <= p.length; i++) r.missing.push(rd32(x + 2 + 4 * i));
      break;
    }
    case REPLY.DELTA: {
      if (p.length < x + 1) break;
      const n = p[x]!; r.entries = [];
      for (let i = 0; i < n; i++) { const at = x + 1 + 9 * i; if (at + 9 > p.length) break; r.entries.push({ asset: p[at] === 1, id: rd32(at + 1), version: rd32(at + 5) }); }
      break;
    }
    case REPLY.SNAPSHOT: {
      if (p.length < x + 5) break;
      r.total = rd16(x); r.page = rd16(x + 2); const n = p[x + 4]!; r.entries = [];
      for (let i = 0; i < n; i++) { const at = x + 5 + 13 * i; if (at + 13 > p.length) break; r.entries.push({ asset: p[at] === 1, id: rd32(at + 1), version: rd32(at + 5), lastUse: rd32(at + 9) }); }
      break;
    }
  }
  return r;
}
export const STATUS_NAMES = ["applied", "stale", "missing", "capacity", "refused", "unchanged", "delta", "snapshot", "reset"];
export function describeReply(r: Reply): string {
  const s = STATUS_NAMES[r.status] ?? `status ${r.status}`;
  if (r.status === REPLY.REFUSED) return `${s} (${REFUSE[r.reason ?? 0] ?? r.reason})`;
  if (r.status === REPLY.MISSING) return `${s} ${r.missing?.join(",")}${r.more ? ",…" : ""}`;
  if (r.evicted) return `${s}, evicted ${r.evicted}`;
  return s;
}

// ---- RLE image assets --------------------------------------------------------------------------
/** [w][h][4bpp RLE] over exactly w*h pixels (no row padding), from a row-major 0..15 gray buffer. */
export function rleImage(gray: ArrayLike<number>, width: number, height: number): number[] {
  int(width, 1, 255, "image width"); int(height, 1, 255, "image height");
  if (gray.length !== width * height) throw new Error("image buffer does not match its dimensions");
  const out = [width, height];
  let i = 0;
  while (i < gray.length) {
    const c = gray[i]! & 15;
    let n = 1;
    while (i + n < gray.length && (gray[i + n]! & 15) === c && n < 65535) n++;
    if (n <= 15) out.push((n << 4) | c);
    else if (n <= 255) out.push(c, n);
    else out.push(c, 0, n & 0xff, n >> 8);
    i += n;
  }
  return out;
}
/** A font asset: 96 relative uint32 glyph offsets (chars 32..127) followed by the glyph images. */
export function fontAsset(glyphs: Map<string, number[]>): number[] {
  const table = new Array<number>(96 * 4).fill(0);
  const body: number[] = [];
  for (const [ch, image] of glyphs) {
    const code = ch.codePointAt(0)!;
    if (code < 32 || code > 127) throw new Error(`font glyph ${JSON.stringify(ch)} outside 32..127`);
    const off = 96 * 4 + body.length;
    table.splice((code - 32) * 4, 4, ...u32(off));
    body.push(...image);
  }
  return [...table, ...body];
}
