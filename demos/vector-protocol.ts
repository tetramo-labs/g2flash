/** Revision 38 path/rotation helpers on the object cache. No mobile dependencies; mirrors patches/VECTOR_PROTOCOL.md. */
import { EASE, hash31, ops, pathRecord, putObject, record as geometricRecord, show, type Ref } from "./object-cache";
export { u16, i16, u32 } from "./object-cache";
function integer(v: number, min: number, max: number, name: string): number {
  if (!Number.isInteger(v) || v < min || v > max) throw new Error(`${name} outside ${min}..${max}: ${v}`);
  return v;
}
/** sid 0x09 field 101 framebuffer control, paired with a basic-settings read.
 * The firmware applies control before decoding the stock message. The read
 * body (field 4) makes it send the reply that the demo waits for; field 101
 * alone changes the lease silently and does not produce an ACK. */
export function framebufferLease(op: 5 | 6, magic: number): Uint8Array {
  integer(op, 5, 6, "framebuffer lease operation");
  integer(magic, 0, 255, "request id");
  const id = magic < 128 ? [magic] : [(magic & 127) | 128, 1];
  return Uint8Array.from([8, 2, 16, ...id, 0x22, 2, 8, 1, 0xaa, 6, 6, 70, 67, 1, op, 1, 0]);
}

/** A scene slot as the fixtures address it: slot n is object id n + 1. */
export const slotId = (n: number) => integer(n, 0, 255, "slot") + 1;
/** An object definition for `scene()`: slot + record; the version is the record's content hash. */
export interface SlotDef { slot: number; record: number[] }
export const shape = (slot: number, type: number, params: number[], color = 15, width = 1): SlotDef =>
  ({ slot, record: geometricRecord(type, color, width, ...params) });
export const path = (slot: number, data: number[], rule: "nonzero" | "evenodd" = "nonzero", x = 0, y = 0, scale = 1, color = 15): SlotDef =>
  ({ slot, record: pathRecord(data, rule, x, y, scale, color) });
export const rotate = (slot: number, degrees: number, px: number, py: number, durationMs = 0, curve: readonly number[] = EASE.linear) =>
  ops.rotate(slotId(slot), degrees, px, py, durationMs, curve);
export const glide = (slot: number, dx: number, dy: number, frames: number, curve: readonly number[] = EASE.linear) => ops.glide(slotId(slot), dx, dy, frames, curve);
export const tween = (slot: number, mask: number, frames: number, curve: readonly number[], values: number[]) => ops.tween(slotId(slot), mask, frames, curve, values);
export const freeze = (slot: number) => ops.freeze(slotId(slot));
export const finish = (slot: number) => ops.finish(slotId(slot));

/**
 * One SHOW: the definitions become embedded PUTs and the active list (in slot
 * order, each version = content hash), `keep` continues the current list with
 * ops only, and `refs` names previously defined slots to keep on the list.
 */
export function scene(defs: SlotDef[], o: { ops?: number[][]; keep?: boolean; refs?: SlotDef[]; bg?: number; request?: number } = {}): Uint8Array {
  if (o.keep && (defs.length || o.refs?.length)) throw new Error("a KEEP scene carries only ops");
  const all = [...(o.refs ?? []), ...defs].sort((a, b) => a.slot - b.slot);
  const versionOf = (d: SlotDef) => hash31(d.record.join(","));
  const refs: Ref[] = all.map((d) => ({ id: slotId(d.slot), version: versionOf(d) }));
  const puts = defs.map((d) => putObject(slotId(d.slot), versionOf(d), d.record));
  return show({ request: o.request ?? 0, bg: o.bg ?? 0, puts, refs: o.keep ? [] : refs, keep: o.keep, ops: o.ops });
}

/** SVG d subset: absolute/relative M L H V Q C S T Z; implicit repetitions.
 * Unsupported syntax is rejected. Each filled contour must explicitly close. */
export function svgPath(d: string): number[] {
  const u16 = (v: number) => [v & 255, (v >>> 8) & 255];
  const tokens = d.match(/[a-zA-Z]|[-+]?(?:\d*\.\d+|\d+\.?\d*)(?:[eE][-+]?\d+)?/g) ?? [];
  const residue = d.replace(/[a-zA-Z]|[-+]?(?:\d*\.\d+|\d+\.?\d*)(?:[eE][-+]?\d+)?/g, "");
  if (/[^\s,]/.test(residue)) throw new Error("Invalid SVG path syntax");
  let i = 0, cmd = "", x = 0, y = 0, sx = 0, sy = 0, open = false, previous = "";
  let cx = 0, cy = 0;
  const out: number[] = [];
  const emit = (op: number, ...coords: number[]) => {
    out.push(op, ...coords.flatMap(v => u16(integer(Math.round(v * 16), -32768, 32767, "Q4 coordinate"))));
  };
  const number = () => {
    if (i >= tokens.length || /^[a-z]$/i.test(tokens[i])) throw new Error("Missing SVG coordinate");
    const v = Number(tokens[i++]); if (!Number.isFinite(v)) throw new Error("Invalid number"); return v;
  };
  while (i < tokens.length) {
    if (/^[a-z]$/i.test(tokens[i])) cmd = tokens[i++];
    if (!cmd || !/^[MLHVQCSTZ]$/i.test(cmd)) throw new Error(`Unsupported SVG command: ${cmd}`);
    const op = cmd.toUpperCase(), rel = cmd !== op;
    const pair = (): [number, number] => [number() + (rel ? x : 0), number() + (rel ? y : 0)];
    if (op === "Z") {
      if (!open) throw new Error("Z without an open contour");
      emit(4); x = sx; y = sy; open = false; previous = "Z"; cmd = ""; continue;
    }
    if (op !== "M" && !open) throw new Error("Path must start with M");
    if (op === "M") {
      if (open) throw new Error("Filled contours must close with Z before another M");
      [x, y] = pair(); sx = x; sy = y; open = true; emit(0, x, y); cmd = rel ? "l" : "L";
    } else if (op === "L" || op === "H" || op === "V") {
      if (op === "L") [x, y] = pair();
      else if (op === "H") x = number() + (rel ? x : 0);
      else y = number() + (rel ? y : 0);
      emit(1, x, y);
    } else if (op === "Q" || op === "T") {
      const c = op === "Q" ? pair() : (previous === "Q" || previous === "T" ? [2*x-cx, 2*y-cy] : [x, y]);
      const end = pair(); emit(2, ...c, ...end); [cx, cy] = c; [x, y] = end;
    } else {
      const a = op === "C" ? pair() : (previous === "C" || previous === "S" ? [2*x-cx, 2*y-cy] : [x, y]);
      const b = pair(), end = pair(); emit(3, ...a, ...b, ...end); [cx, cy] = b; [x, y] = end;
    }
    previous = op;
  }
  if (open || !out.length) throw new Error("Expected explicitly closed filled path");
  return out;
}

export interface Point {x: number; y: number}
/** Exact pixel-boundary contours, retaining holes. At diagonal contacts choose
 * the right turn to keep the two white regions separate. Collinear runs merge. */
export function traceBitmap(bits: Uint8Array, width: number, height: number): Point[][] {
  if (bits.length !== width * height) throw new Error("Invalid bitmap dimensions");
  interface Edge {x: number; y: number; nx: number; ny: number; dir: number; used: boolean}
  const edges: Edge[] = [], starts = new Map<number, Edge[]>();
  const key = (x: number, y: number) => y * (width + 1) + x;
  const add = (x: number, y: number, nx: number, ny: number, dir: number) => {
    const e = {x, y, nx, ny, dir, used: false}; edges.push(e);
    const k = key(x, y); const list = starts.get(k) ?? []; list.push(e); starts.set(k, list);
  };
  const set = (x: number, y: number) => x >= 0 && x < width && y >= 0 && y < height && bits[y*width+x] !== 0;
  for (let y=0;y<height;y++) for (let x=0;x<width;x++) if (set(x,y)) {
    if (!set(x,y-1)) add(x,y,x+1,y,0);
    if (!set(x+1,y)) add(x+1,y,x+1,y+1,1);
    if (!set(x,y+1)) add(x+1,y+1,x,y+1,2);
    if (!set(x-1,y)) add(x,y+1,x,y,3);
  }
  const contours: Point[][] = [];
  for (const start of edges) {
    if (start.used) continue;
    let e = start; const points: Point[] = [];
    for (let guard = 0; guard <= edges.length; guard++) {
      e.used = true; points.push({x:e.x,y:e.y});
      if (e.nx === start.x && e.ny === start.y) break;
      const candidates = (starts.get(key(e.nx,e.ny)) ?? []).filter(v => !v.used);
      const rank = (v: Edge) => [1,0,3,2][(v.dir-e.dir+4)%4];
      candidates.sort((a,b) => rank(a)-rank(b));
      if (!candidates.length) throw new Error("Open bitmap contour");
      e = candidates[0];
    }
    const corners = points.filter((p,i) => {
      const a=points[(i+points.length-1)%points.length], b=points[(i+1)%points.length];
      return (p.x-a.x)*(b.y-p.y) !== (p.y-a.y)*(b.x-p.x);
    });
    contours.push(corners);
  }
  return contours;
}
export function contourSvg(contours: Point[][], scale: number): string {
  if (!contours.length) return "M0 0Z";
  return contours.map(points => points.map((p,i) => `${i ? "L" : "M"}${p.x*scale} ${p.y*scale}`).join("")+"Z").join("");
}
