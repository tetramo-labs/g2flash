#!/usr/bin/env bun
// Switch the BLE link between the stock connection profile and the 7.5 ms fast
// profile, or query it. The firmware default is stock.
//
//     bun ble-mode.ts            # query
//     bun ble-mode.ts fast       # 7.5 ms interval, latency 0, slow mode suppressed
//     bun ble-mode.ts stock      # back to the stock 15-30 ms / slow-mode behaviour
//
// Each lens has its own link, so the control (sid 0x09 field 127, ['B','L',1,op])
// goes to both arms. It is paired with a basic-settings read so the reply
// arrives; the reply's field 128 carries the state:
//   ['B','L',1, fast, side, wantedMode, appliedMode, min16, max16, latency16,
//    liveInterval16, liveLatency16]
// Interval units are 1.25 ms. Modes: 0xa3 fast, 0xa4 slow, 0 = update pending.
// The first triple is the profile the last request asked for; the live pair is
// what the central actually granted (older builds omit it).

import { G2Session } from "g2-kit/ble";
import { describeCfw, findLenDelimField, queryGlasslyCfw, REQUIRED_REVISION } from "./glassly-cfw";

const OPS: Record<string, number> = { stock: 0, fast: 1, query: 2 };
const word = (process.argv[2] ?? "query").toLowerCase();
if (!(word in OPS)) {
  console.log("usage: bun ble-mode.ts [stock|fast|query]");
  process.exit(2);
}
const op = OPS[word]!;

let magic = 40;
const nextMagic = () => (magic = (magic % 200) + 1);
const varint = (v: number) => (v < 128 ? [v] : [(v & 127) | 128, v >>> 7]);

// commandId=2, magicRandom, field 4 = basic-settings read, field 127 = control.
function controlPb(m: number): Uint8Array {
  const ctl = [0x42, 0x4c, 1, op];
  return Uint8Array.from([8, 2, 16, ...varint(m), 0x22, 2, 8, 1, 0xfa, 0x07, ctl.length, ...ctl]);
}

function describeStatus(st: Uint8Array): string {
  const u16 = (i: number) => st[i]! | (st[i + 1]! << 8);
  const ms = (units: number) => (units * 1.25).toFixed(2);
  const mode = (m: number) => (m === 0xa3 ? "fast" : m === 0xa4 ? "slow" : m === 0 ? "pending" : `0x${m.toString(16)}`);
  const live = st.length >= 17 ? `  live ${ms(u16(13))} ms / latency ${u16(15)}` : "";
  return `${st[3] ? "FAST" : "stock"}  wanted=${mode(st[5]!)} applied=${mode(st[6]!)}` +
    `  requested ${ms(u16(7))}-${ms(u16(9))} ms / latency ${u16(11)}${live}`;
}

const session = await G2Session.open();
let cfw = await queryGlasslyCfw(session, nextMagic());
if (!cfw) cfw = await queryGlasslyCfw(session, nextMagic());
if (!cfw || cfw.revision < REQUIRED_REVISION) {
  console.log(describeCfw(cfw));
  console.log(`BLE link control needs the glassly-cfw build, revision ${REQUIRED_REVISION} or later`);
  await session.close();
  process.exit(1);
}

let failed = 0;
for (const arm of ["R", "L"] as const) {
  const m = nextMagic();
  const ack = await session.sendPb(0x09, controlPb(m), m, { ackTimeoutMs: 4000, arm });
  const st = ack ? findLenDelimField(ack.pb, 128) : null;
  if (!st || st.length < 13 || st[0] !== 0x42 || st[1] !== 0x4c || st[2] !== 1) {
    console.log(`${arm}: ${ack ? "reply without a BLE status field" : "no reply"}`);
    failed++;
    continue;
  }
  console.log(`${arm}: ${describeStatus(st)}`);
}
if (op !== 2) console.log("the interval renegotiates asynchronously; run `bun ble-mode.ts` again to see it applied");
await session.close();
process.exit(failed ? 1 : 0);
