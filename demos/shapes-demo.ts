#!/usr/bin/env bun
// Staged demo of the Glassly CFW's vector and text modes (image-handler modes
// 16, 17, 18). Each stage draws its own screen, runs, blanks the panel and
// pauses, so what you see on the glasses maps 1:1 onto the stage names printed
// here. Text stages use inline TEXT records: the glasses draw the string with
// their own font, so an update is one small message and the slot can glide,
// tween and fade like any shape.
//
//     bun shapes-demo.ts                # every stage
//     G2_STAGE=3 bun shapes-demo.ts     # one stage
//     G2_PAUSE_MS=1500 G2_LOOPS=2 bun shapes-demo.ts
//
// Needs the glassly-cfw firmware at contract revision 19 or later.

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
const LOOPS = Math.max(1, Number(process.env.G2_LOOPS ?? "2"));
const PAUSE_MS = Math.max(0, Number(process.env.G2_PAUSE_MS ?? "900"));
const ONLY_STAGE = process.env.G2_STAGE ? Number(process.env.G2_STAGE) : 0;
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
/** Text options byte: transparent background + gray level 0..15. */
const ink = (level: number) => 0x10 | (level & 0x0f);

/** One 20-byte shape record: [type][flags][color][width][p0..p7 int16]. */
function record(type: number, color: number, width: number, ...p: number[]): number[] {
  const out = [type, VISIBLE, color, width];
  for (let i = 0; i < 8; i++) out.push(...i16(p[i] ?? 0));
  return out;
}
/** Inline built-in-font text: [17][flags][options][0][x][y][w][h][len][bytes], clipped to w/h when > 0. */
function text(options: number, x: number, y: number, w: number, h: number, s: string): number[] {
  const bytes = [...new TextEncoder().encode(s)].slice(0, 128);
  return [T.TEXT_INLINE, VISIBLE, options, 0, ...i16(x), ...i16(y), ...i16(w), ...i16(h), bytes.length, ...bytes];
}
// mode 16: immediate shapes into the shadow
const immediate = (...recs: number[][]) => Uint8Array.from([16, recs.length, ...recs.flat()]);
// mode 17: retained scene ops
const COMMIT = 1, CLEAR = 2;
const SET = (slot: number, rec: number[]) => [0, slot, ...rec];
const DELETE = (slot: number) => [1, slot];
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
if (!caps || !hasFeature(caps, "scene17") || caps.version < 19) {
  console.log(caps ? `CFW ${caps.raw}` : "no CFW capability field");
  console.log("this demo needs the glassly-cfw build at EVENCFW/19 or later");
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

  // ---- stage runner ------------------------------------------------------------
  // Slot 0 is the stage label in every scene stage; content starts at slot 1.
  const LABEL = 0;
  const label = (n: number, name: string) => SET(LABEL, text(ink(7), 20, 12, 0, 0, `stage ${n}: ${name}`));
  let stageNo = 0;
  async function stage(name: string, body: (n: number) => Promise<void>): Promise<void> {
    stageNo++;
    if (ONLY_STAGE && ONLY_STAGE !== stageNo) return;
    console.log(`[stage ${stageNo}] ${name}`);
    const t0 = performance.now();
    await body(stageNo);
    console.log(`[stage ${stageNo}] done in ${((performance.now() - t0) / 1000).toFixed(1)} s`);
    await send(scene(COMMIT | CLEAR, 0));          // blank between stages
    await sleep(PAUSE_MS);
  }
  /** Send `count` scene patches as fast as the link acks them; returns the achieved rate. */
  async function burst(count: number, frame: (i: number) => Uint8Array): Promise<string> {
    const t0 = performance.now();
    let worst = 0;
    for (let i = 0; i < count; i++) {
      const t = performance.now();
      await send(frame(i));
      worst = Math.max(worst, performance.now() - t);
    }
    const secs = (performance.now() - t0) / 1000;
    return `${count} updates in ${secs.toFixed(2)} s = ${(count / secs).toFixed(1)}/s, worst ack ${worst.toFixed(0)} ms`;
  }

  // ---- 1. shapes card (mode 16, immediate) ---------------------------------------
  await stage("shapes card (mode 16)", async (n) => {
    await send(immediate(
      record(T.RECT, 8, 2, 8, 8, 624, 464, 16),
      text(ink(15), 40, 24, 0, 0, `stage ${n}: every shape, drawn immediately`),
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
  });

  // ---- 2. inline text: many lines, gray levels, clipping --------------------------
  await stage("inline text lines (mode 17)", async (n) => {
    const lines = ["The glasses draw this text themselves.", "Each line is one slot with its bytes inline.", "No texture cache, no bitmaps, no re-sends."];
    await send(scene(COMMIT | CLEAR, 0,
      label(n, "inline text lines"),
      SET(1, record(T.RECT, 6, 1, 20, 40, 600, 400, 12)),
      ...lines.map((s, i) => SET(2 + i, text(ink(15), 40, 60 + i * 30, 0, 0, s))),
      ...[15, 12, 9, 6, 3].map((level, i) => SET(10 + i, text(ink(level), 40, 170 + i * 28, 0, 0, `gray level ${level}`))),
      SET(20, text(ink(15), 40, 330, 260, 30, "clipped at 260 px: the rest of this sentence is cut")),
      SET(21, record(T.RECT, 5, 1, 40, 328, 262, 32, 0)),
      SET(22, text(ink(11), 40, 380, 0, 0, "UTF-8: café, naïve, 日本語, ∑")),
    ));
    await sleep(3500);
  });

  // ---- 3. fast text updates, one small message each --------------------------------
  await stage("fast text updates", async (n) => {
    await send(scene(COMMIT | CLEAR, 0,
      label(n, "fast text updates"),
      SET(1, text(ink(9), 40, 70, 0, 0, "counter")),
      SET(2, text(ink(15), 220, 70, 0, 0, "0")),
      SET(3, text(ink(9), 40, 130, 0, 0, "typewriter")),
      SET(4, text(ink(15), 220, 130, 0, 0, " ")),
      SET(5, text(ink(9), 40, 190, 0, 0, "tenths clock")),
      SET(6, text(ink(15), 220, 190, 0, 0, "00.0")),
      SET(7, text(ink(9), 40, 250, 0, 0, "updates/s")),
      SET(8, text(ink(15), 220, 250, 0, 0, "measuring...")),
    ));
    const counter = await burst(40, (i) => scene(COMMIT, 0, SET(2, text(ink(15), 220, 70, 0, 0, String(i + 1)))));
    console.log(`  counter: ${counter}`);
    const sentence = "typed one character per message";
    const typed = await burst(sentence.length, (i) => scene(COMMIT, 0, SET(4, text(ink(15), 220, 130, 0, 0, sentence.slice(0, i + 1)))));
    console.log(`  typewriter: ${typed}`);
    const t0 = performance.now();
    const clock = await burst(40, (i) => {
      const tenths = Math.round((performance.now() - t0) / 100);
      const s = `${String(Math.floor(tenths / 10)).padStart(2, "0")}.${tenths % 10}`;
      return scene(COMMIT, 0, SET(6, text(ink(15), 220, 190, 0, 0, s)), SET(8, text(ink(15), 220, 250, 0, 0, `${i + 1} sent`)));
    });
    console.log(`  clock: ${clock}`);
    await send(scene(COMMIT, 0, SET(8, text(ink(15), 220, 250, 0, 0, clock.split(" = ")[1]?.split(",")[0] ?? clock))));
    await sleep(2000);
  });

  // ---- 4. text animation: slide-in, caption scroll, fade ---------------------------------
  await stage("text animation", async (n) => {
    // four lines start off-screen left and glide in with staggered easing
    const slide = ["slide in", "with staggered", "eases", "on each line"];
    await send(scene(COMMIT | CLEAR, 0,
      label(n, "text animation: slide in"),
      ...slide.map((s, i) => SET(1 + i, text(ink(15), -320, 70 + i * 30, 0, 0, s))),
      ...slide.map((_, i) => GLIDE(1 + i, 360, 0, 20 + i * 8, [EASE.out, EASE.inOut, EASE.in, EASE.linear][i])),
    ));
    await sleep(2200);

    // captions: a new line arrives at the bottom while the others glide up and the top one leaves
    const captions = ["captions arrive at the bottom", "older lines glide up", "and the oldest one leaves", "every step is one message", "at any rate the link allows", "until the transcript ends"];
    const rows = 3, pitch = 30, top = 230;
    await send(scene(COMMIT | CLEAR, 0, label(n, "text animation: caption scroll"), SET(9, record(T.RECT, 5, 1, 30, top - 10, 580, rows * pitch + 14, 8))));
    for (let step = 0; step < captions.length; step++) {
      const slot = 10 + (step % 4);
      const ops: number[][] = [SET(slot, text(ink(15), 40, top + rows * pitch, 560, pitch, captions[step]))];
      for (let k = Math.max(0, step - rows); k <= step; k++) ops.push(GLIDE(10 + (k % 4), 0, -pitch, 10, EASE.out));
      if (step >= rows) ops.push(DELETE(10 + ((step - rows) % 4)));
      await send(scene(COMMIT, 0, ...ops));
      await sleep(550);
    }
    await sleep(800);

    // fade: the gray level tweens up and down while the line drifts
    await send(scene(COMMIT | CLEAR, 0,
      label(n, "text animation: fade"),
      SET(1, text(ink(0), 120, 220, 0, 0, "fading in, drifting right, fading out")),
      TWEEN(1, COLOR, 30, EASE.linear, ink(15)),
    ));
    await sleep(1300);
    await send(scene(COMMIT, 0, GLIDE(1, 120, 0, 45, EASE.inOut)));
    await sleep(1700);
    await send(scene(COMMIT, 0, TWEEN(1, COLOR, 30, EASE.linear, ink(0))));
    await sleep(1300);
  });

  // ---- 5. shapes animation: glides and tweens -----------------------------------------
  await stage("shape animation", async (n) => {
    const BALL = 2, BAR = 3, GAUGE = 5, READOUT = 7;
    await send(scene(COMMIT | CLEAR, 0,
      label(n, "shape animation"),
      SET(1, record(T.RECT, 6, 2, 20, 40, 600, 420, 16)),
      SET(BALL, record(T.CIRCLE_FILL, 15, 0, 80, 240, 24)),
      SET(4, record(T.RECT, 6, 1, 60, 400, 520, 16, 8)),                        // bar track
      SET(BAR, record(T.RECT_FILL, 12, 0, 60, 400, 1, 16, 8)),
      SET(GAUGE, record(T.ARC, 15, 8, 480, 200, 70, -90, -90)),
      SET(READOUT, text(ink(15), 440, 300, 0, 0, "0 %")),
      GLIDE(BALL, 480, 0, 45, EASE.inOut),
      TWEEN(BAR, P(2), 60, EASE.linear, 520),
      TWEEN(GAUGE, P(4), 60, EASE.out, 270),
    ));
    // the readout follows the bar with plain text updates while the firmware tweens the rest
    for (let i = 1; i <= 10; i++) {
      await sleep(180);
      await send(scene(COMMIT, 0, SET(READOUT, text(ink(15), 440, 300, 0, 0, `${i * 10} %`))));
    }
    await sleep(500);
    for (let i = 0; i < LOOPS; i++) {
      await send(scene(COMMIT, 0,
        GLIDE(BALL, -480, 0, 45, EASE.inOut),
        TWEEN(BAR, P(2), 45, EASE.inOut, 1),
        TWEEN(GAUGE, P(4) | WIDTH, 45, EASE.inOut, -90, 2),
        SET(READOUT, text(ink(15), 440, 300, 0, 0, "0 %")),
      ));
      await sleep(2000);
      await send(scene(COMMIT, 0,
        GLIDE(BALL, 480, 0, 45, EASE.inOut),
        TWEEN(BAR, P(2), 45, EASE.inOut, 520),
        TWEEN(GAUGE, P(4) | WIDTH, 45, EASE.inOut, 270, 8),
        SET(READOUT, text(ink(15), 440, 300, 0, 0, "100 %")),
      ));
      await sleep(2000);
    }
  });

  // ---- 6. composed glides at a 16 ms frame period ------------------------------------------
  await stage("composed glides at 16 ms", async (n) => {
    await send(animCtl(1, 16));
    await send(scene(COMMIT | CLEAR, 0,
      label(n, "composed glides, 16 ms frames"),
      SET(1, record(T.CIRCLE_FILL, 15, 0, 320, 240, 24)),
      SET(2, text(ink(15), 320, 300, 0, 0, "second glide adds to the first")),
      GLIDE(1, -240, -120, 60, EASE.out),
      GLIDE(2, -240, -120, 60, EASE.out),
    ));
    await sleep(400);
    await send(scene(COMMIT, 0, GLIDE(1, -40, 240, 60, EASE.inOut), GLIDE(2, -40, 240, 60, EASE.inOut)));
    await sleep(2500);
    await send(animCtl(1, 33));
  });

  console.log("[18] release scene");
  await send(animCtl(2));
} finally {
  if (renew) clearInterval(renew);
  await lease(6).catch(() => {});                  // FB_RELEASE
  hb.stop();
  await session.close();
}
process.exit(0);
