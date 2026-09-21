#!/usr/bin/env bun
// Card-on-top with depth: content stays on the list while a card slides in
// from the right, painted after it (on top), at a nearer depth. Depth on the
// G2 is stereo disparity, so the card's objects are placed at x + d on the
// left lens and x - d on the right one with two lens-targeted SHOWs; the
// background objects keep the same geometry on both. Dismissal glides it
// out; the next pop-in is references only (cache hit).
//
//     bun card-demo.ts              # depth 6 px disparity
//     G2_DEPTH=10 bun card-demo.ts
import { G2Session, buildCreateStartUpPageContainer, querySettings } from "g2-kit/ble";
import { startHeartbeat } from "g2-kit/ui";
import { CacheLink, CfwTransport } from "./cfw-transport";
import { framebufferLease } from "./vector-protocol";
import { EASE, hash31, inlineText, ops, putObject, record, show, T, type Ref } from "./object-cache";
import { describeCfw, queryGlasslyCfw, REQUIRED_REVISION } from "./glassly-cfw";

const DEPTH = Number(process.env.G2_DEPTH ?? "6");
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));
let magic = 100;
const nextMagic = () => (magic = magic >= 255 ? 100 : magic + 1);
const ink = (level: number) => 0x10 | (level & 0x0f);

interface Obj { id: number; rec: (dx: number) => number[] }
const obj = (name: string, rec: (dx: number) => number[]): Obj => ({ id: hash31(name), rec });
const version = (o: Obj, dx: number) => hash31(`${o.id}:${o.rec(dx).join(",")}`);

// background: a title, a frame and a few lines that keep animating (a bar sweeps)
const ORIGIN = { x: 32, y: 128 };
const background: Obj[] = [
  obj("frame", () => record(T.RECT, 6, 1, ORIGIN.x + 8, ORIGIN.y + 8, 560, 272, 12)),
  obj("title", () => inlineText(ink(15), ORIGIN.x + 24, ORIGIN.y + 20, 0, 0, "Underlying content stays live")),
  obj("l1", () => inlineText(ink(9), ORIGIN.x + 24, ORIGIN.y + 60, 0, 0, "line one of the app view")),
  obj("l2", () => inlineText(ink(9), ORIGIN.x + 24, ORIGIN.y + 90, 0, 0, "line two keeps its objects")),
  obj("bar", () => record(T.RECT_FILL, 12, 0, ORIGIN.x + 24, ORIGIN.y + 240, 40, 12, 4)),
];
// card: 300x180 at the right, black fill occludes, light border, three lines
const CARD = { x: ORIGIN.x + 250, y: ORIGIN.y + 40, w: 300, h: 180 };
const card: Obj[] = [
  obj("card.fill", (dx) => record(T.RECT_FILL, 0, 0, CARD.x + dx, CARD.y, CARD.w, CARD.h, 16)),
  obj("card.border", (dx) => record(T.RECT, 15, 2, CARD.x + dx, CARD.y, CARD.w, CARD.h, 16)),
  obj("card.title", (dx) => inlineText(ink(15), CARD.x + dx + 20, CARD.y + 16, 0, 0, "Notification")),
  obj("card.body", (dx) => inlineText(ink(11), CARD.x + dx + 20, CARD.y + 56, CARD.w - 40, 60, "A card at a nearer depth,")),
  obj("card.body2", (dx) => inlineText(ink(11), CARD.x + dx + 20, CARD.y + 86, CARD.w - 40, 60, "on top of the live view.")),
  obj("card.dot", (dx) => record(T.CIRCLE_FILL, 15, 0, CARD.x + dx + CARD.w - 28, CARD.y + 28, 6)),
];
const OFFSCREEN = 640 - CARD.x + 8;   // start fully right of the panel

const session = await G2Session.open();
const settings = await querySettings(session, nextMagic());
if (settings) console.log(`firmware: L=${settings.leftSoftwareVersion} R=${settings.rightSoftwareVersion}`);
let cfw = await queryGlasslyCfw(session, nextMagic());
if (!cfw) cfw = await queryGlasslyCfw(session, nextMagic());
if (!cfw || cfw.revision < REQUIRED_REVISION) { console.log(describeCfw(cfw)); await session.close(); process.exit(1); }
const hb = startHeartbeat({ session, nextMagic });
const create = buildCreateStartUpPageContainer({ name: `c${String(Date.now() % 10000).padStart(4, "0")}`, items: ["."], containerId: 1, captureEvents: false, magic: nextMagic() });
if (!(await session.sendPb(0xe0, create.pb, create.magic, { ackTimeoutMs: 8000 }))) throw new Error("CREATE did not ack");
await sleep(300);
const lease = async (op: 5 | 6) => { const m = nextMagic(); await session.sendPb(9, framebufferLease(op, m), m, { ackTimeoutMs: 8000 }); };
await lease(5);
const renew = setInterval(() => void lease(5), 30_000);
const transport = new CfwTransport(session, 8000);
const link = new CacheLink(transport);

/** The residency mirror, per lens: object id -> version the lens holds. */
const resident: Record<number, Map<number, number>> = { 1: new Map(), 2: new Map() };
/** One SHOW for one lens: `items` in paint order with the lens's x disparity for card objects. */
function frameFor(lens: 1 | 2, items: { o: Obj; depth: number }[], opList: number[][] = [], tag = 0): Uint8Array {
  const puts: number[][] = [], refs: Ref[] = [];
  for (const { o, depth } of items) {
    const dx = lens === 1 ? depth : -depth;
    const rec = o.rec(dx), ver = version(o, dx);
    if (resident[lens]!.get(o.id) !== ver) { puts.push(putObject(o.id, ver, rec)); resident[lens]!.set(o.id, ver); }
    refs.push({ id: o.id, version: ver });
  }
  return show({ puts, refs, ops: opList, request: tag });
}
/** Send a per-lens pair: the same list, disparity applied per eye. */
async function showBoth(items: { o: Obj; depth: number }[], opList: number[][] = []) {
  const t0 = performance.now();
  for (const lens of [1, 2] as const) { link.targets = lens; await link.send(frameFor(lens, items, opList)); }
  link.targets = 3;
  return performance.now() - t0;
}

try {
  await link.reset();
  console.log(`cache reset: epoch ${link.epoch}, store ${link.storeKiB} KiB`);
  const bg = background.map((o) => ({ o, depth: 0 }));
  const cardItems = card.map((o) => ({ o, depth: DEPTH }));
  // 1. the app view, with a bar that sweeps back and forth
  console.log(`[1] content (${(await showBoth(bg, [ops.glide(background[4]!.id, 480, 0, 90, EASE.inOut)])).toFixed(0)} ms both lenses)`);
  await sleep(1500);
  // 2. card pops in from the right, on top, at depth: define it off-screen and glide it in this SHOW
  const offscreen = card.map((o) => ({ o: obj(`${o.id}`, (dx) => o.rec(dx + OFFSCREEN)), depth: DEPTH }));
  // (the off-screen definition is a different version of the same ids: id derives from the name, so reuse the id)
  for (let i = 0; i < offscreen.length; i++) offscreen[i]!.o.id = card[i]!.id;
  const slideIn = card.map((o) => ops.glide(o.id, -OFFSCREEN, 0, 9, EASE.out));
  console.log(`[2] card slides in (${(await showBoth([...bg, ...offscreen], [...slideIn, ops.glide(background[4]!.id, -480, 0, 90, EASE.inOut)])).toFixed(0)} ms)`);
  await sleep(2500);
  // 3. dismiss: glide out, then drop it from the list (content untouched)
  console.log(`[3] card slides out (${(await showBoth([...bg, ...offscreen.map((it) => ({ ...it }))], card.map((o) => ops.glide(o.id, OFFSCREEN, 0, 8, EASE.in)))).toFixed(0)} ms)`);
  await sleep(400);
  console.log(`[4] content only (${(await showBoth(bg)).toFixed(0)} ms)`);
  await sleep(1200);
  // 4. reopen from the cache: the off-screen definition is resident and the glide-out left the card
  //    exactly there, so the pop-in is references plus the same glides, no definitions.
  const reopen = await showBoth([...bg, ...offscreen], slideIn);
  console.log(`[5] card back from the cache (${reopen.toFixed(0)} ms, no definitions)`);
  await sleep(2500);
  await showBoth(bg);
  await sleep(800);
} finally {
  clearInterval(renew);
  try { link.targets = 3; await link.send(Uint8Array.from([39, 0, 0, 0, 0, 1, 0])); } catch {}
  transport.close();
  await lease(6).catch(() => {});
  hb.stop();
  await session.close();
}
process.exit(0);
