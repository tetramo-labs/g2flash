#!/usr/bin/env bun
// Shapes + animation demo for the Glassly CFW (image-handler modes 16, 17, 18).
//
//   1. draws a static test card with mode 16 (immediate vector shapes)
//   2. builds a retained scene with mode 17 and lets the glasses animate it:
//      a ball glides back and forth with ease-in-out, a progress bar and an arc
//      gauge tween, and a title fades in — each round trip is ONE small message,
//      the firmware renders every frame itself
//   3. releases the scene with mode 18 and the framebuffer lease
//
//     bun shapes-demo.ts
//
// Needs the glassly-cfw firmware (capabilities shapes16 scene17 anim18). The
// texture cache (mode 12) carries the string bytes that TEXT records point at.

import {
  G2Session,
  buildCreateStartUpPageContainer,
  buildImageContainers,
  buildImageRawData,
  planImageFragments,
  querySettings,
  queryCapabilities,
  hasFeature,
  type ImageContainerSpec,
} from "g2-kit/ble";
import { startHeartbeat } from "g2-kit/ui";

const ACK_MS = 8_000;
const LOOPS = Math.max(1, Number(process.env.G2_LOOPS ?? "3"));
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

let magic = 100;
const nextMagic = () => (magic = magic >= 255 ? 100 : magic + 1);

// ---- wire helpers ------------------------------------------------------------
const u16 = (v: number) => [v & 0xff, (v >> 8) & 0xff];
const i16 = (v: number) => u16(v < 0 ? v + 0x10000 : v);
const varint = (v: number) => { const out: number[] = []; do { let b = v & 0x7f; v >>>= 7; if (v) b |= 0x80; out.push(b); } while (v); return out; };

const T = { LINE: 1, RECT: 2, RECT_FILL: 3, CIRCLE: 4, CIRCLE_FILL: 5, TRI: 6, TRI_FILL: 7, QUAD: 8,
  QUAD_FILL: 9, BEZIER2: 10, BEZIER3: 11, ARC: 12, PIE: 13, IMAGE: 14, TEXT: 15, TEXT_CACHED: 16, TEXT_INLINE: 17 } as const;
const VISIBLE = 1;
// CSS cubic-bezier control points scaled to 0..255
const EASE = { linear: [0, 0, 255, 255], inOut: [107, 0, 148, 255], out: [0, 0, 148, 255], in: [107, 0, 255, 255] };

/** One 20-byte shape record: [type][flags][color][width][p0..p7 int16]. */
function record(type: number, color: number, width: number, ...p: number[]): number[] {
  const out = [type, VISIBLE, color, width];
  for (let i = 0; i < 8; i++) out.push(...i16(p[i] ?? 0));
  return out;
}
/** Inline built-in-font text: [17][flags][options][0][x][y][w][h][len][bytes], clipped to w/h when > 0. */
function inlineText(options: number, x: number, y: number, w: number, h: number, text: string): number[] {
  const bytes = [...new TextEncoder().encode(text)].slice(0, 128);
  return [T.TEXT_INLINE, VISIBLE, options, 0, ...i16(x), ...i16(y), ...i16(w), ...i16(h), bytes.length, ...bytes];
}
// mode 16: immediate shapes into the shadow
const immediate = (...recs: number[][]) => Uint8Array.from([16, recs.length, ...recs.flat()]);
// mode 12: texture cache update (strings for TEXT records live here)
const texcache = (offset: number, bytes: Uint8Array) => Uint8Array.from([12, ...u16(offset), ...u16(bytes.length), ...bytes]);
// mode 17: retained scene ops
const COMMIT = 1, CLEAR = 2;
const SET = (slot: number, rec: number[]) => [0, slot, ...rec];
const GLIDE = (slot: number, dx: number, dy: number, frames: number, curve: number[]) => [4, slot, ...i16(dx), ...i16(dy), frames, ...curve];
const P = (i: number) => 1 << i, COLOR = 1 << 8, WIDTH = 1 << 9;   // tween mask bits
const TWEEN = (slot: number, mask: number, frames: number, curve: number[], ...values: number[]) =>
  [5, slot, ...u16(mask), frames, ...curve, ...values.flatMap(i16)];
const scene = (flags: number, bg: number, ...ops: number[][]) => Uint8Array.from([17, flags, bg, ...ops.flat()]);
// mode 18: animation control
const animCtl = (...b: number[]) => Uint8Array.from([18, ...b]);

// Framebuffer lease over sid 0x09 field 101 (['F','C',1,op,nonceLo,nonceHi]);
// commandId=2 / magicRandom keep the stock decoder happy.
function leasePb(op: number): { pb: Uint8Array; magic: number } {
  const m = nextMagic();
  const ctl = [0x46, 0x43, 1, op, 1, 0];
  return { pb: Uint8Array.from([0x08, 2, 0x10, ...varint(m), 0xaa, 0x06, ctl.length, ...ctl]), magic: m };
}

// ---- session ------------------------------------------------------------------
const session = await G2Session.open();
// A settings read first, as detect-cfw.ts does: the first request after connect
// can go unanswered, and it prints the firmware versions.
const settings = await querySettings(session, nextMagic());
if (settings) console.log(`firmware: L=${settings.leftSoftwareVersion} R=${settings.rightSoftwareVersion}`);
let caps = await queryCapabilities(session, nextMagic());
if (!caps) caps = await queryCapabilities(session, nextMagic());
if (!caps || !hasFeature(caps, "scene17")) {
  console.log(caps ? `CFW ${caps.raw}` : "no CFW capability field");
  console.log("this demo needs the glassly-cfw build (shapes16 scene17 anim18)");
  await session.close();
  process.exit(1);
}
console.log(`CFW detected: ${caps.raw}`);

const hb = startHeartbeat({ session, nextMagic });
const suffix = String(Date.now() % 10_000).padStart(4, "0");
let sid = 1;

async function sendImage(payload: Uint8Array, container: ImageContainerSpec): Promise<void> {
  for (const frag of planImageFragments(payload, 4000)) {
    const raw = buildImageRawData({
      containerId: container.containerId, containerName: container.name, mapSessionId: sid,
      mapTotalSize: payload.length, mapFragmentIndex: frag.index, mapRawData: frag.data,
      magic: nextMagic(), compressMode: 0,
    });
    if (!(await session.sendPb(0xe0, raw.pb, raw.magic, { ackTimeoutMs: ACK_MS })))
      throw new Error(`image message (mode ${payload[0]}) did not ack`);
  }
  sid++;
}

async function lease(op: number): Promise<void> {
  const { pb, magic: m } = leasePb(op);
  await session.sendPb(0x09, pb, m, { ackTimeoutMs: ACK_MS });
}

let renew: ReturnType<typeof setInterval> | undefined;
try {
  const create = buildCreateStartUpPageContainer({
    name: `b${suffix}`, items: ["."], containerId: 1, captureEvents: false, magic: nextMagic(), extraContainerNames: [`c${suffix}`],
  });
  if (!(await session.sendPb(0xe0, create.pb, create.magic, { ackTimeoutMs: ACK_MS }))) throw new Error("CREATE did not ack");
  const container: ImageContainerSpec = { name: `c${suffix}`, containerId: 2, x: 0, y: 0, width: 576, height: 288 };
  const rebuild = buildImageContainers({ containers: [container], magic: nextMagic() });
  if (!(await session.sendPb(0xe0, rebuild.pb, rebuild.magic, { ackTimeoutMs: ACK_MS }))) throw new Error("REBUILD did not ack");
  await sleep(300);
  const send = (p: Uint8Array) => sendImage(p, container);

  await lease(5);                                  // FB_ACQUIRE (90 s, fail-open)
  renew = setInterval(() => void lease(5), 30_000);

  // strings for TEXT records
  const enc = new TextEncoder();
  const TITLE = enc.encode("Glassly"), SUB = enc.encode("shapes + animation in firmware");
  await send(texcache(0, TITLE));
  await send(texcache(32, SUB));

  // 1. immediate test card (mode 16)
  console.log("[16] test card");
  await send(immediate(
    record(T.RECT, 8, 2, 8, 8, 624, 464, 16),
    inlineText(0x1F, 40, 24, 0, 0, "Glassly (inline text)"),
    record(T.LINE, 12, 4, 40, 60, 600, 60),
    record(T.CIRCLE_FILL, 15, 0, 120, 200, 60),
    record(T.CIRCLE, 12, 6, 300, 200, 60),
    record(T.RECT_FILL, 10, 0, 400, 140, 160, 120, 20),
    record(T.TRI_FILL, 15, 0, 60, 420, 180, 420, 120, 320),
    record(T.QUAD, 15, 2, 220, 320, 380, 330, 370, 430, 230, 420),
    record(T.BEZIER3, 15, 3, 400, 400, 460, 300, 520, 500, 600, 380),
    record(T.ARC, 15, 5, 520, 200, 50, 180, 360),
    record(T.PIE, 9, 0, 520, 200, 30, -90, 180),
  ));
  await sleep(2500);

  // 2. retained scene (mode 17): slots in paint order
  const BALL = 2, BAR = 3, GAUGE = 5, TITLE_SLOT = 0;
  console.log("[17] scene + animations");
  await send(scene(COMMIT | CLEAR, 0,
    SET(TITLE_SLOT, record(T.TEXT, 0, 0, 40, 24, 0, TITLE.length)),           // starts black, fades in
    SET(1, record(T.RECT, 6, 2, 20, 20, 600, 440, 16)),
    SET(BALL, record(T.CIRCLE_FILL, 15, 0, 80, 240, 24)),
    SET(4, record(T.RECT, 6, 1, 60, 400, 520, 16, 8)),                        // bar track
    SET(BAR, record(T.RECT_FILL, 12, 0, 60, 400, 1, 16, 8)),
    SET(GAUGE, record(T.ARC, 15, 8, 480, 200, 70, -90, -90)),
    SET(6, inlineText(0x19, 40, 440, 300, 30, "inline text, clipped to 300 px ............")),
    TWEEN(TITLE_SLOT, COLOR, 30, EASE.out, 15),
    GLIDE(BALL, 480, 0, 45, EASE.inOut),
    TWEEN(BAR, P(2), 60, EASE.linear, 520),
    TWEEN(GAUGE, P(4), 60, EASE.out, 270),
  ));
  await sleep(2500);

  for (let i = 0; i < LOOPS; i++) {
    console.log(`[17] loop ${i + 1}/${LOOPS}: back`);
    await send(scene(COMMIT, 0,
      GLIDE(BALL, -480, 0, 45, EASE.inOut),
      GLIDE(6, 0, -20, 45, EASE.inOut),
      TWEEN(BAR, P(2), 45, EASE.inOut, 1),
      TWEEN(GAUGE, P(4) | WIDTH, 45, EASE.inOut, -90, 2),
    ));
    await sleep(2000);
    console.log(`[17] loop ${i + 1}/${LOOPS}: forth`);
    await send(scene(COMMIT, 0,
      GLIDE(BALL, 480, 0, 45, EASE.inOut),
      GLIDE(6, 0, 20, 45, EASE.inOut),
      SET(6, inlineText(0x1F, 40, 420, 300, 30, `inline text, update ${i + 1}`)),
      TWEEN(BAR, P(2), 45, EASE.inOut, 520),
      TWEEN(GAUGE, P(4) | WIDTH, 45, EASE.inOut, 270, 8),
    ));
    await sleep(2000);
  }

  // faster frame period, then a bouncing glide that composes with itself
  console.log("[18] period 16 ms, composed glides");
  await send(animCtl(1, 16));
  await send(scene(COMMIT, 0, GLIDE(BALL, -240, -120, 60, EASE.out)));
  await sleep(400);
  await send(scene(COMMIT, 0, GLIDE(BALL, -240, 120, 60, EASE.inOut)));   // adds to the in-flight glide
  await sleep(2500);

  console.log("[18] release scene");
  await send(animCtl(2));
} finally {
  if (renew) clearInterval(renew);
  await lease(6).catch(() => {});                  // FB_RELEASE
  hb.stop();
  await session.close();
}
process.exit(0);
