#!/usr/bin/env bun
// The glassly example-miniapp's three Bad Apple tests, played straight at the
// glasses over BLE. The miniapp renders the same clip through display.render()
// three ways; this script sends what the phone would put on the air for each:
//
//   --text    half-block mono text (▀ ▄ █) at 78×40. The phone rasterizes
//             mono text that is not plain ASCII, and a full-canvas element
//             does not fit the texture cache, so this rides the raster path:
//             one mode-6 keyframe, then mode-3 bounding-box deltas.
//   --bitmap  one full-canvas 4bpp image element per frame at the stored
//             156×80. Same raster path, finer pixels: the cost of streaming
//             images rather than glyph cells.
//   --shapes  up to 80 filled rects per frame covering the silhouette, ids
//             carried from frame to frame, a one-frame linear transition:
//             mode-37 scene patches where the moving parts are TWEENs and the
//             rest is untouched, so the glasses animate the silhouette.
//
// Frames come from bad_apple_quarter.gif sampled exactly as the miniapp's
// scripts/generate-bad-apple.ts does (156×80, 1-bit, 10 fps, 300 frames), so
// what plays here is the miniapp's clip, not a re-encode.
//
//     bun bad-apple-tests.ts                       # text, bitmap, shapes in turn
//     bun bad-apple-tests.ts --shapes              # one mode
//     bun bad-apple-tests.ts --text --bitmap       # several, in this order
//     bun bad-apple-tests.ts --shapes --loops 3    # play the clip three times
//     bun bad-apple-tests.ts --frames 100 --fps 15 # shorter, faster
//     bun bad-apple-tests.ts --dry-run             # decode + encode + report, no glasses
//     bun bad-apple-tests.ts --trace               # per-frame bytes and ack times
//     bun bad-apple-tests.ts --gif other.gif       # another grayscale GIF
//
// Every mode plays by the clock: the frame due now goes out, and a frame the
// link could not keep up with is skipped rather than queued, as the miniapp
// does. Needs the glassly-cfw firmware, revision 26 or later.

import {
  G2Session,
  buildCreateStartUpPageContainer,
  buildImageContainers,
  buildImageRawData,
  planImageFragments,
  querySettings,
  type ImageContainerSpec,
} from "g2-kit/ble";
import { describeCfw, queryGlasslyCfw, REQUIRED_REVISION } from "./glassly-cfw";
import { CacheLink, CfwTransport } from "./cfw-transport";
import { EASE, hash31, hide, ops, putObject, record as geometricRecord, show, T as SHAPES, type Ref } from "./object-cache";
import { startHeartbeat } from "g2-kit/ui";
import { deflateSync } from "node:zlib";
import { GifReader } from "omggif";

// ---- CLI ---------------------------------------------------------------------
type Mode = "text" | "bitmap" | "shapes";
const ALL_MODES: Mode[] = ["text", "bitmap", "shapes"];

const argv = process.argv.slice(2);
const flag = (name: string) => argv.includes(`--${name}`);
const value = (name: string, fallback: string) => {
  const i = argv.indexOf(`--${name}`);
  return i >= 0 && argv[i + 1] !== undefined ? argv[i + 1]! : fallback;
};
if (flag("help") || flag("h")) {
  console.log(`usage: bun bad-apple-tests.ts [--text] [--bitmap] [--shapes] [options]
  --loops N     play the clip N times per mode (default 1)
  --frames N    cap the clip at N frames (default 300, the miniapp's clip)
  --fps N       playback rate (default 10, the miniapp's clip)
  --gif PATH    source GIF (default ./bad_apple_quarter.gif)
  --dry-run     decode + encode + report without connecting
  --trace       print every frame's bytes and ack time`);
  process.exit(0);
}
const modes = ALL_MODES.filter((m) => flag(m));
const MODES: Mode[] = modes.length ? modes : ALL_MODES;
const LOOPS = Math.max(1, Number(value("loops", "1")));
const MAX_FRAMES = Math.max(1, Number(value("frames", "300")));
const FPS = Math.max(1, Math.min(30, Number(value("fps", "10"))));
const GIF = value("gif", new URL("./bad_apple_quarter.gif", import.meta.url).pathname);
const DRY_RUN = flag("dry-run") || process.env.G2_DRY_RUN === "1";
const TRACE = flag("trace") || process.env.G2_TRACE === "1";
for (const a of argv) {
  if (a.startsWith("--") && !["--text", "--bitmap", "--shapes", "--loops", "--frames", "--fps", "--gif", "--dry-run", "--trace"].includes(a)) {
    console.error(`unknown option ${a} (try --help)`);
    process.exit(1);
  }
}

// ---- the clip: the miniapp's frames --------------------------------------------
// 156×80 = the bitmap test's full resolution; the text test samples every
// second pixel for its 78×40 half-block grid.
const CLIP_W = 156;
const CLIP_H = 80;
const THRESHOLD = 128;
const FRAME_BYTES = Math.ceil((CLIP_W * CLIP_H) / 8);

/** The panel the CFW draws, and where the phone's 576×288 window sits in it. */
const PANEL_W = 640;
const PANEL_H = 480;
const CANVAS_W = 576;
const CANVAS_H = 288;
const ORIGIN = { x: 32, y: 128 };

interface Clip { count: number; pixels: Uint8Array; source: string }

/** Decode the GIF the way scripts/generate-bad-apple.ts does: stride to FPS, box-sample, threshold. */
async function loadClip(path: string, fps: number, maxFrames: number): Promise<Clip> {
  const bytes = new Uint8Array(await Bun.file(path).arrayBuffer());
  const gif = new GifReader(bytes);
  const srcW = gif.width, srcH = gif.height;
  const delayCs = Math.max(1, gif.frameInfo(0).delay);
  const srcFps = 100 / delayCs;
  const stride = Math.max(1, Math.round(srcFps / fps));
  const rgba = new Uint8Array(srcW * srcH * 4);
  const count = Math.min(maxFrames, Math.floor(gif.numFrames() / stride));
  const pixels = new Uint8Array(FRAME_BYTES * count);
  for (let f = 0; f < count; f++) {
    for (let s = 0; s < stride; s++) gif.decodeAndBlitFrameRGBA(f * stride + s, rgba);
    const base = f * FRAME_BYTES;
    for (let y = 0; y < CLIP_H; y++) {
      const sy = Math.min(srcH - 1, Math.floor(((y + 0.5) * srcH) / CLIP_H));
      for (let x = 0; x < CLIP_W; x++) {
        const sx = Math.min(srcW - 1, Math.floor(((x + 0.5) * srcW) / CLIP_W));
        const o = (sy * srcW + sx) * 4;
        const lum = 0.299 * rgba[o]! + 0.587 * rgba[o + 1]! + 0.114 * rgba[o + 2]!;
        if (lum >= THRESHOLD) {
          const bit = y * CLIP_W + x;
          pixels[base + (bit >> 3)] |= 0x80 >> (bit & 7);
        }
      }
    }
  }
  return { count, pixels, source: `${path.split("/").pop()} ${srcW}×${srcH} @${srcFps.toFixed(0)} fps, every ${stride}${stride === 1 ? "st" : stride === 2 ? "nd" : stride === 3 ? "rd" : "th"} frame` };
}

type BitAt = (x: number, y: number) => boolean;
const frameBits = (clip: Clip, frame: number): BitAt => {
  const base = frame * FRAME_BYTES;
  return (x, y) => {
    const index = y * CLIP_W + x;
    return (clip.pixels[base + (index >> 3)]! & (0x80 >> (index & 7))) !== 0;
  };
};

// ---- shapes: the miniapp's badAppleShapes.ts -------------------------------------
interface Rect { x: number; y: number; w: number; h: number }
interface TrackedRect extends Rect { id: string }

/** Rect budget: enough for a silhouette, small enough to ack at frame rate (the miniapp's MAX_SHAPE_RECTS). */
const MAX_SHAPE_RECTS = 80;

/**
 * Cover the frame's set pixels with rects on a `cell`×`cell` grid (a cell is
 * set when at least half its pixels are), greedily: each unvisited set cell
 * starts a rect that grows right as far as the row allows, then down while
 * every row below is set across the same span.
 */
function coverFrame(bit: BitAt, width: number, height: number, cell: number): Rect[] {
  const cols = Math.floor(width / cell);
  const rows = Math.floor(height / cell);
  const need = Math.ceil((cell * cell) / 2);
  const grid = new Uint8Array(cols * rows);
  for (let cy = 0; cy < rows; cy++) {
    for (let cx = 0; cx < cols; cx++) {
      let set = 0;
      for (let y = 0; y < cell; y++) for (let x = 0; x < cell; x++) if (bit(cx * cell + x, cy * cell + y)) set++;
      if (set >= need) grid[cy * cols + cx] = 1;
    }
  }
  const rects: Rect[] = [];
  for (let cy = 0; cy < rows; cy++) {
    for (let cx = 0; cx < cols; cx++) {
      if (grid[cy * cols + cx] !== 1) continue;
      let w = 1;
      while (cx + w < cols && grid[cy * cols + cx + w] === 1) w++;
      let h = 1;
      outer: while (cy + h < rows) {
        for (let x = 0; x < w; x++) if (grid[(cy + h) * cols + cx + x] !== 1) break outer;
        h++;
      }
      for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) grid[(cy + y) * cols + cx + x] = 2;
      rects.push({ x: cx * cell, y: cy * cell, w: w * cell, h: h * cell });
    }
  }
  return rects;
}

/** The finest cover that fits `maxRects`, trying each cell size in turn (the last is used regardless). */
function coverFrameWithin(bit: BitAt, width: number, height: number, maxRects: number, cells = [2, 3, 4, 5, 8]): Rect[] {
  let rects: Rect[] = [];
  for (const cell of cells) {
    rects = coverFrame(bit, width, height, cell);
    if (rects.length <= maxRects) return rects;
  }
  return rects.slice(0, maxRects);
}

/** Scale rects from clip pixels to the canvas (edges rounded, so neighbours stay flush). */
function scaleRects(rects: Rect[], srcW: number, srcH: number, dstW: number, dstH: number): Rect[] {
  const sx = dstW / srcW;
  const sy = dstH / srcH;
  return rects.map((r) => {
    const x0 = Math.round(r.x * sx);
    const y0 = Math.round(r.y * sy);
    return { x: x0, y: y0, w: Math.round((r.x + r.w) * sx) - x0, h: Math.round((r.y + r.h) * sy) - y0 };
  });
}

/**
 * Carries ids from one frame's rects to the next: a rect continues the
 * nearest unmatched previous rect by centre when it is not wildly different in
 * size, everything else gets a fresh id, and a vanished id rests a frame before
 * it is handed out again so a new rect never tweens from a stale slot.
 */
class RectTracker {
  private previous: TrackedRect[] = [];
  private cooling: string[] = [];
  private free: string[] = [];
  private next = 0;

  constructor(private readonly reach: number) {}

  track(rects: Rect[]): TrackedRect[] {
    const unmatched = new Set(this.previous.map((_, i) => i));
    const out: (TrackedRect | undefined)[] = new Array(rects.length);
    const order = rects.map((_, i) => i).sort((a, b) => rects[b]!.w * rects[b]!.h - rects[a]!.w * rects[a]!.h);
    for (const i of order) {
      const r = rects[i]!;
      const cx = r.x + r.w / 2;
      const cy = r.y + r.h / 2;
      let best = -1;
      let bestScore = Infinity;
      for (const j of unmatched) {
        const p = this.previous[j]!;
        const dx = p.x + p.w / 2 - cx;
        const dy = p.y + p.h / 2 - cy;
        const dist = Math.sqrt(dx * dx + dy * dy);
        if (dist > this.reach) continue;
        const size = Math.abs(p.w - r.w) + Math.abs(p.h - r.h);
        const score = dist + size / 2;
        if (score < bestScore) { bestScore = score; best = j; }
      }
      if (best >= 0) {
        unmatched.delete(best);
        out[i] = { ...r, id: this.previous[best]!.id };
      }
    }
    const released = [...unmatched].map((j) => this.previous[j]!.id);
    for (let i = 0; i < rects.length; i++) {
      if (out[i]) continue;
      const id = this.free.pop() ?? `r${this.next++}`;
      out[i] = { ...rects[i]!, id };
    }
    this.free.push(...this.cooling);
    this.cooling = released;
    this.previous = out as TrackedRect[];
    return this.previous;
  }
}

// ---- wire: object-cache frames for filled rects -------------------------------------
const FRAME_PERIOD_MS = 33;
/** Every tweenable rect parameter (x y w h r) plus color and width, as the suite sends. */
const RECT_TWEEN_MASK = 0x1f | 0x100 | 0x200;

const clampCoord = (v: number) => Math.max(-32768, Math.min(32767, Math.trunc(v)));
const rectParams = (r: Rect) => [clampCoord(r.x + ORIGIN.x), clampCoord(r.y + ORIGIN.y), clampCoord(r.w), clampCoord(r.h), 0];
const rectRecord = (r: Rect) => geometricRecord(SHAPES.RECT_FILL, 15, 0, ...rectParams(r));
const rectVersion = (r: Rect) => hash31(rectParams(r).join(","));

/**
 * A hidden two-frame tween before the first frame: the firmware creates its
 * animation timer on the first tween, so the first visible transition of a
 * lease should not also be its first animation.
 */
const WARM_UP_ID = hash31("warm-up");
const WARM_UP = show({
  puts: [putObject(WARM_UP_ID, 1, geometricRecord(SHAPES.CIRCLE_FILL, 0, 0, 0, 0, 1))],
  refs: [{ id: WARM_UP_ID, version: 1 }],
  ops: [ops.tween(WARM_UP_ID, 0x01, 2, EASE.linear, [1])],
});

/**
 * The retained scene for the shapes test: one object per tracked rect id, on
 * the active list for as long as the id lives. A continuing rect that moved
 * becomes a TWEEN over the frame period, a new one an embedded PUT, a
 * vanished one leaves the list (it stays cached until the LRU needs the room;
 * an id the tracker hands out again is redefined, never tweened from a stale
 * position).
 */
class RectScene {
  private known = new Map<string, { version: number; state: Rect }>();
  private active = new Set<string>();

  encode(rects: TrackedRect[], transitionMs: number): { payload: Uint8Array; sets: number; tweens: number; deletes: number } {
    const frames = transitionMs > 0 ? Math.max(2, Math.min(255, Math.round(transitionMs / FRAME_PERIOD_MS))) : 0;
    const puts: number[][] = [], refs: Ref[] = [], opList: number[][] = [];
    let sets = 0, tweens = 0;
    const next = new Set<string>();
    for (const r of rects) {
      const id = hash31(r.id);
      next.add(r.id);
      const k = this.known.get(r.id);
      const geometry = { x: r.x, y: r.y, w: r.w, h: r.h };
      if (k && this.active.has(r.id)) {
        const prev = k.state;
        if (prev.x !== r.x || prev.y !== r.y || prev.w !== r.w || prev.h !== r.h) {
          if (frames >= 2) { opList.push(ops.tween(id, RECT_TWEEN_MASK, frames, EASE.linear, [...rectParams(r), 15, 0])); tweens++; k.state = geometry; }
          else { k.version = rectVersion(r); k.state = geometry; puts.push(putObject(id, k.version, rectRecord(r))); sets++; }
        }
        refs.push({ id, version: k.version });
        continue;
      }
      const version = rectVersion(r);
      if (k && k.version === version) { k.state = geometry; refs.push({ id, version }); continue; }   /* a cache hit */
      this.known.set(r.id, { version, state: geometry });
      puts.push(putObject(id, version, rectRecord(r)));
      sets++;
      refs.push({ id, version });
    }
    const deletes = [...this.active].filter((id) => !next.has(id)).length;
    this.active = next;
    return { payload: show({ puts, refs, ops: opList }), sets, tweens, deletes };
  }
}

// ---- wire: the raster path (video-bench's mode 6 keyframes + mode 3 deltas) ----------------
function pack4bpp(gray: Uint8Array): Uint8Array {
  const stride = (PANEL_W + 1) >> 1;
  const out = new Uint8Array(stride * PANEL_H);
  for (let y = 0; y < PANEL_H; y++) {
    for (let x = 0; x < PANEL_W; x += 2) {
      const hi = gray[y * PANEL_W + x]! >> 4;
      const lo = x + 1 < PANEL_W ? gray[y * PANEL_W + x + 1]! >> 4 : 0;
      out[y * stride + (x >> 1)] = (hi << 4) | lo;
    }
  }
  return out;
}

/** RLE over the pixel nibbles: [cnt4|color4], or [0|color4][cnt8], or [0|color4][0][cntLo][cntHi]. */
function rleEncode(pix: Uint8Array): Uint8Array {
  const n = pix.length * 2;
  const out = new Uint8Array(n);
  let o = 0;
  const nib = (i: number) => (i & 1 ? pix[i >> 1]! & 0x0f : pix[i >> 1]! >> 4);
  let i = 0;
  while (i < n) {
    const v = nib(i);
    let j = i + 1;
    while (j < n && nib(j) === v) j++;
    let run = j - i;
    while (run > 0) {
      const c = Math.min(run, 0xffff);
      if (c <= 15) out[o++] = (c << 4) | v;
      else if (c <= 255) { out[o++] = v; out[o++] = c; }
      else { out[o++] = v; out[o++] = 0; out[o++] = c & 0xff; out[o++] = c >> 8; }
      run -= c;
    }
    i = j;
  }
  return out.subarray(0, o);
}

/** mode 6: the whole 640×480 panel as headerless 4bpp, RLE'd then deflated. */
function keyframe(gray: Uint8Array): Uint8Array {
  const z = deflateSync(rleEncode(pack4bpp(gray)));
  return Uint8Array.from([6, ...z]);
}

/** mode 3: the bounding box of pixels whose 4bpp value changed, quantized to ×4 wide and ×2 tall. */
function delta(cur: Uint8Array, prev: Uint8Array, fid: number): { payload: Uint8Array; area: number } {
  let minx = PANEL_W, miny = PANEL_H, maxx = -1, maxy = -1;
  for (let y = 0; y < PANEL_H; y++) {
    for (let x = 0; x < PANEL_W; x++) {
      const i = y * PANEL_W + x;
      if (cur[i]! >> 4 !== prev[i]! >> 4) {
        if (x < minx) minx = x;
        if (x > maxx) maxx = x;
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
      }
    }
  }
  if (maxx < 0) { minx = 0; maxx = 3; miny = 0; maxy = 1; }
  const left = minx & ~3;
  const right = Math.min(PANEL_W, (maxx + 4) & ~3);
  const top = miny & ~1;
  const bottom = Math.min(PANEL_H, (maxy + 2) & ~1);
  const w = right - left, h = bottom - top;
  const stride = w >> 1;
  const pixels = new Uint8Array(stride * h);
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x += 2) {
      const hi = cur[(top + y) * PANEL_W + left + x]! >> 4;
      const lo = cur[(top + y) * PANEL_W + left + x + 1]! >> 4;
      pixels[y * stride + (x >> 1)] = (hi << 4) | lo;
    }
  }
  const z = deflateSync(rleEncode(pixels));
  return { payload: Uint8Array.from([3, left >> 2, top >> 1, w >> 2, h >> 1, fid & 0xff, (fid >> 8) & 0xff, ...z]), area: w * h };
}

/** Fill a panel-space rect with white, in the 576×288 window. */
function fillBlock(gray: Uint8Array, x0: number, y0: number, x1: number, y1: number): void {
  for (let y = y0; y < y1; y++) gray.fill(255, (ORIGIN.y + y) * PANEL_W + ORIGIN.x + x0, (ORIGIN.y + y) * PANEL_W + ORIGIN.x + x1);
}

/**
 * The text test's frame: every second clip pixel becomes a half-block cell of
 * a 78-column, 20-row mono text wall, which the phone would rasterize on the
 * canvas grid. Each ▀ ▄ █ half is a solid block, so the raster is the block
 * grid itself. Also returns the string, which is what crosses the bridge.
 */
function textFrame(bit: BitAt): { gray: Uint8Array; text: string } {
  const cols = CLIP_W / 2, rows = CLIP_H / 2;
  const gray = new Uint8Array(PANEL_W * PANEL_H);
  const lines: string[] = [];
  for (let row = 0; row < rows / 2; row++) {
    let line = "";
    for (let col = 0; col < cols; col++) {
      const top = bit(col * 2, row * 4);
      const bottom = bit(col * 2, row * 4 + 2);
      line += top ? (bottom ? "█" : "▀") : bottom ? "▄" : " ";
      const x0 = Math.round((col * CANVAS_W) / cols), x1 = Math.round(((col + 1) * CANVAS_W) / cols);
      if (top) fillBlock(gray, x0, Math.round((row * 2 * CANVAS_H) / rows), x1, Math.round(((row * 2 + 1) * CANVAS_H) / rows));
      if (bottom) fillBlock(gray, x0, Math.round(((row * 2 + 1) * CANVAS_H) / rows), x1, Math.round(((row * 2 + 2) * CANVAS_H) / rows));
    }
    lines.push(line);
  }
  return { gray, text: lines.join("\n") };
}

/** The bitmap test's frame: the 156×80 image element scaled to the canvas, nearest neighbour. */
function bitmapFrame(bit: BitAt): Uint8Array {
  const gray = new Uint8Array(PANEL_W * PANEL_H);
  for (let y = 0; y < CLIP_H; y++) {
    const y0 = Math.round((y * CANVAS_H) / CLIP_H), y1 = Math.round(((y + 1) * CANVAS_H) / CLIP_H);
    let x = 0;
    while (x < CLIP_W) {
      if (!bit(x, y)) { x++; continue; }
      let end = x + 1;
      while (end < CLIP_W && bit(end, y)) end++;
      fillBlock(gray, Math.round((x * CANVAS_W) / CLIP_W), y0, Math.round((end * CANVAS_W) / CLIP_W), y1);
      x = end;
    }
  }
  return gray;
}

/** The miniapp's per-frame 4bpp BMP: what the bitmap test puts across the phone bridge each frame. */
function bmpBytes(): number {
  const rowBytes = Math.ceil(CLIP_W / 2);
  const paddedRow = (rowBytes + 3) & ~3;
  const headerSize = 14 + 40 + 16 * 4;
  const fileSize = headerSize + paddedRow * CLIP_H;
  return Math.ceil(fileSize / 3) * 4; // base64
}

// ---- link ----------------------------------------------------------------------------
const ACK_MS = 8_000;
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));
let magic = 100;
const nextMagic = () => (magic = magic >= 255 ? 100 : magic + 1);
const varint = (v: number) => { const out: number[] = []; do { let b = v & 0x7f; v >>>= 7; if (v) b |= 0x80; out.push(b); } while (v); return out; };

/** Framebuffer lease over sid 0x09 field 101: ['F','C',1,op,nonceLo,nonceHi]. */
function leasePb(op: number): { pb: Uint8Array; magic: number } {
  const m = nextMagic();
  const ctl = [0x46, 0x43, 1, op, 1, 0];
  return { pb: Uint8Array.from([0x08, 2, 0x10, ...varint(m), 0xaa, 0x06, ctl.length, ...ctl]), magic: m };
}

interface Link { send(payload: Uint8Array): Promise<void>; close(): Promise<void> }

async function openLink(): Promise<Link> {
  const session = await G2Session.open();
  const settings = await querySettings(session, nextMagic());
  if (settings) console.log(`firmware: L=${settings.leftSoftwareVersion} R=${settings.rightSoftwareVersion}`);
  let cfw = await queryGlasslyCfw(session, nextMagic());
  if (!cfw) cfw = await queryGlasslyCfw(session, nextMagic());
  if (!cfw || cfw.revision < REQUIRED_REVISION) {
    console.log(describeCfw(cfw));
    console.log(`these tests need the glassly-cfw build, revision ${REQUIRED_REVISION} or later`);
    await session.close();
    process.exit(1);
  }
  console.log(`CFW detected: ${cfw.raw}`);

  const hb = startHeartbeat({ session, nextMagic });
  const suffix = String(Date.now() % 10_000).padStart(4, "0");
  const create = buildCreateStartUpPageContainer({ name: `b${suffix}`, items: ["."], containerId: 1, captureEvents: false, magic: nextMagic() });
  if (!(await session.sendPb(0xe0, create.pb, create.magic, { ackTimeoutMs: ACK_MS }))) throw new Error("CREATE did not ack");
  await sleep(300);

  const lease = async (op: number) => {
    const { pb, magic: m } = leasePb(op);
    await session.sendPb(0x09, pb, m, { ackTimeoutMs: ACK_MS });
  };
  await lease(5); // FB_ACQUIRE (90 s, fail-open)
  const renew = setInterval(() => void lease(5), 30_000);

  // GLASSLYCFW/38: every payload rides the SID-0xf0 transport; cache messages share one session epoch.
  const transport = new CfwTransport(session, ACK_MS);
  const cache = new CacheLink(transport);
  await cache.reset();
  const send = async (payload: Uint8Array) => { await cache.send(payload); };
  return {
    send,
    async close() {
      clearInterval(renew);
      try { await send(hide()); } catch {}
      transport.close();
      await lease(6).catch(() => {});                  // FB_RELEASE
      hb.stop();
      await session.close();
    },
  };
}

// ---- the tests -----------------------------------------------------------------------------
interface Stats { mode: Mode; sent: number; skipped: number; wireBytes: number; bridgeBytes: number; seconds: number; worstAckMs: number; note: string }

/**
 * Play by the clock: the frame due now goes out, one message at a time, and
 * frames the ack cadence cannot keep up with are skipped rather than queued.
 * `render(frame)` returns the payload for the clip frame, or null to skip it.
 */
async function play(mode: Mode, clip: Clip, link: Link | null, render: (frame: number, sentBefore: number) => { payload: Uint8Array; bridge: number; detail?: string } | null): Promise<Omit<Stats, "note">> {
  const total = clip.count * LOOPS;
  const t0 = performance.now();
  let sent = 0, skipped = 0, wireBytes = 0, bridgeBytes = 0, worstAckMs = 0;
  let last = -1;
  while (true) {
    const due = link ? Math.floor(((performance.now() - t0) * FPS) / 1000) : last + 1;
    if (due >= total) break;
    if (due === last) { await sleep(2); continue; }
    if (last >= 0 && due > last + 1) skipped += due - last - 1;
    last = due;
    const out = render(due % clip.count, sent);
    if (!out) continue;
    const t = performance.now();
    if (link) await link.send(out.payload);
    const ack = performance.now() - t;
    worstAckMs = Math.max(worstAckMs, ack);
    sent++;
    wireBytes += out.payload.length;
    bridgeBytes += out.bridge;
    if (TRACE) console.log(`      ${mode} #${String(due).padStart(3)}  ${String(out.payload.length).padStart(5)} B wire  ${String(out.bridge).padStart(5)} B bridge  ${String(Math.round(ack)).padStart(4)} ms ack${out.detail ? `  ${out.detail}` : ""}`);
  }
  return { mode, sent, skipped, wireBytes, bridgeBytes, seconds: link ? (performance.now() - t0) / 1000 : total / FPS, worstAckMs };
}

/** mode 6 keyframe, then mode 3 deltas; a keyframe again every `keyEvery` frames as the phone's insurance does. */
function rasterRenderer(frameGray: (frame: number) => Uint8Array, bridgeBytes: (frame: number) => number, keyEvery = 60) {
  let prev: Uint8Array | null = null;
  let fid = 0;
  let sinceKey = 0;
  return (frame: number): { payload: Uint8Array; bridge: number; detail?: string } => {
    const gray = frameGray(frame);
    const key = prev === null || sinceKey >= keyEvery;
    let payload: Uint8Array;
    let detail: string;
    if (key) { payload = keyframe(gray); sinceKey = 0; detail = "keyframe"; }
    else { const d = delta(gray, prev!, fid++ & 0xffff); payload = d.payload; detail = `box ${((100 * d.area) / (PANEL_W * PANEL_H)).toFixed(0)}%`; }
    sinceKey++;
    prev = gray;
    return { payload, bridge: bridgeBytes(frame), detail };
  };
}

async function runText(clip: Clip, link: Link | null): Promise<Stats> {
  const frames = new Map<number, { gray: Uint8Array; text: string }>();
  const frame = (f: number) => { let v = frames.get(f); if (!v) { v = textFrame(frameBits(clip, f)); frames.set(f, v); } return v; };
  const render = rasterRenderer((f) => frame(f).gray, (f) => new TextEncoder().encode(frame(f).text).length);
  const stats = await play("text", clip, link, (f) => render(f));
  return { ...stats, note: "78×40 half-block cells; phone rasterizes (▀▄█ are outside mode 14's ASCII), so: mode-6 keyframe + mode-3 deltas" };
}

async function runBitmap(clip: Clip, link: Link | null): Promise<Stats> {
  const bmp = bmpBytes();
  const render = rasterRenderer((f) => bitmapFrame(frameBits(clip, f)), () => bmp);
  const stats = await play("bitmap", clip, link, (f) => render(f));
  return { ...stats, note: `156×80 4bpp BMP image element (${bmp} B base64 per frame on the bridge); a full-canvas tile misses the texture cache, so: mode-6 keyframe + mode-3 deltas` };
}

async function runShapes(clip: Clip, link: Link | null): Promise<Stats> {
  const tracker = new RectTracker(90);
  const scene = new RectScene();
  if (link) { await link.send(WARM_UP); await sleep(150); }
  const transitionMs = Math.round(1000 / FPS);
  const stats = await play("shapes", clip, link, (f) => {
    const bit = frameBits(clip, f);
    const rects = tracker.track(scaleRects(coverFrameWithin(bit, CLIP_W, CLIP_H, MAX_SHAPE_RECTS), CLIP_W, CLIP_H, CANVAS_W, CANVAS_H));
    const encoded = scene.encode(rects, transitionMs);
    // ~20 B per rect element across the bridge: id, box, style, transition.
    return { payload: encoded.payload, bridge: rects.length * 20, detail: `${rects.length} rects: ${encoded.sets} set, ${encoded.tweens} tween, ${encoded.deletes} delete` };
  });
  return { ...stats, note: `≤${MAX_SHAPE_RECTS} filled rects with stable ids and a ${transitionMs} ms linear transition: one SHOW per frame, moving parts as TWEENs, new parts as embedded PUTs` };
}

// ---- main ------------------------------------------------------------------------------------
console.log(`[clip] decoding ${GIF}`);
const clip = await loadClip(GIF, FPS, MAX_FRAMES);
console.log(`[clip] ${clip.source} → ${clip.count} frames of ${CLIP_W}×${CLIP_H} at ${FPS} fps${LOOPS > 1 ? `, ×${LOOPS}` : ""}`);
if (DRY_RUN) console.log("[dry-run] encoding only; nothing is sent");

const link = DRY_RUN ? null : await openLink();
const results: Stats[] = [];
try {
  for (const mode of MODES) {
    console.log(`\n[${mode}] playing`);
    const stats = mode === "text" ? await runText(clip, link) : mode === "bitmap" ? await runBitmap(clip, link) : await runShapes(clip, link);
    results.push(stats);
    if (link) {
      // Blank between tests, and hand the panel back from the scene to the shadow.
      if (mode === "shapes") await link.send(hide());
      else await link.send(keyframe(new Uint8Array(PANEL_W * PANEL_H)));
      await sleep(800);
    }
  }
} finally {
  if (link) await link.close();
}

console.log(`\n=== Bad Apple: ${clip.count} frames${LOOPS > 1 ? ` ×${LOOPS}` : ""} at ${FPS} fps${DRY_RUN ? " (dry run)" : ""} ===`);
for (const s of results) {
  const per = s.sent ? s.wireBytes / s.sent : 0;
  const bridge = s.sent ? s.bridgeBytes / s.sent : 0;
  console.log(
    `${s.mode.padEnd(7)} ${String(s.sent).padStart(4)} sent, ${String(s.skipped).padStart(3)} skipped, ` +
    `${(s.sent / Math.max(0.001, s.seconds)).toFixed(1).padStart(5)} fps, ` +
    `wire ${(s.wireBytes / 1024).toFixed(0).padStart(4)} KiB (${per.toFixed(0)} B/frame), ` +
    `bridge ${bridge.toFixed(0)} B/frame` +
    (link ? `, worst ack ${s.worstAckMs.toFixed(0)} ms` : ""),
  );
  console.log(`        ${s.note}`);
}
process.exit(0);
