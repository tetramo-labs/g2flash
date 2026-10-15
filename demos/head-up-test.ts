#!/usr/bin/env bun
// Live head-up test against GLASSLYCFW/45+: arms the stock head-up switch and
// angle, opens an EvenHub page under both framebuffer leases (the same state
// the phone keeps), then prints every sys event and, every 10 s, the head-up
// counters the firmware keeps in settings field 107 for both input paths.
// bun head-up-test.ts [--timeout 120] [--count 3] [--angle 10] [--no-page] [--diag-interval 10000]
import { G2Session, buildCreateStartUpPageContainer, buildShutDown, querySettings, setHeadUp } from "g2-kit/ble";
import { startHeartbeat } from "g2-kit/ui";
import { findLenDelimField, queryGlasslyCfw } from "./glassly-cfw";
import { framebufferLease } from "./vector-protocol";
import { create, toBinary } from "@bufbuild/protobuf";
import { SID_UI_SETTING } from "g2-kit/ble";
import { G2SettingPackageSchema, g2_settingCommandId, APPRequestSettingType } from "./node_modules/g2-kit/ble/gen/g2_setting_pb";

const args = process.argv.slice(2);
function option(name: string, fallback: number): number {
  const i = args.indexOf(name);
  const value = i < 0 ? fallback : Number(args[i + 1]);
  if (!Number.isInteger(value) || value <= 0) throw new Error(`${name} needs a positive integer`);
  return value;
}
if (args.includes("--help")) {
  console.log("bun head-up-test.ts [--timeout 120] [--count 3] [--angle 10] [--no-page] [--diag-interval ms]\nWait for READY, then lift your head. Exit 0 requires head-up events (12).");
  process.exit(0);
}
const seconds = option("--timeout", 120);
const required = option("--count", 3);
const angle = option("--angle", 10);
const withPage = !args.includes("--no-page");
const diagEvery = option("--diag-interval", 10000);
let magic = 100;
const nextMagic = () => magic = magic >= 255 ? 100 : magic + 1;
const sleep = (ms: number) => new Promise<void>(resolve => setTimeout(resolve, ms));
const names: Record<number, string> = { 0: "tap", 1: "swipe-up", 2: "swipe-down", 3: "double-tap", 7: "system-exit", 9: "long-press", 10: "release", 11: "tap-then-hold", 12: "HEAD-UP", 14: "ring-touch-down", 127: "HEAD-UP-DIAG" };
const BITS: Array<[number, string]> = [[0x01, "switch"], [0x02, "BUSY"], [0x04, "lease"], [0x08, "evenhub"], [0x10, "stock-ok"], [0x20, "right"], [0x40, "ctx"]];
const decodeBits = (v: number) => BITS.map(([m, n]) => (v & m) ? n : `no-${n}`).join(" ");

async function readDiag(session: G2Session): Promise<string> {
  const m = nextMagic();
  const req = create(G2SettingPackageSchema, {
    commandId: g2_settingCommandId.DeviceReceiveRequest, magicRandom: m,
    deviceReceiveRequestFromApp: { settingInfoType: APPRequestSettingType.APP_REQUIRE_BASIC_SETTING },
  });
  const ack = await session.sendPb(SID_UI_SETTING, toBinary(G2SettingPackageSchema, req), m, { ackTimeoutMs: 4000 });
  const d = ack && findLenDelimField(ack.pb, 107);
  if (!d || d.length < 24) return "diag: n/a";
  let out = `diag(R): idle-policy type7=${d[9]} head-up=${d[10]} gate=0x${d[12]!.toString(16)} [${decodeBits(d[12]!)}] mode=0x${d[13]!.toString(16)} last-subtype=${d[14]} | forwarded=${d[11]}`;
  if (d.length >= 30) out += ` | ui-dispatcher records=${d[24]} head-up=${d[25]} last-subtype=${d[26]} last-source=${d[27]} gate=0x${d[28]!.toString(16)} [${decodeBits(d[28]!)}] mode=0x${d[29]!.toString(16)}`;
  return out;
}

async function main(): Promise<number> {
  const session = await G2Session.open();
  let heartbeat: ReturnType<typeof startHeartbeat> | undefined;
  let off: (() => void) | undefined;
  let created = false, leased = false, interrupted = false;
  const interrupt = () => { interrupted = true; };
  process.on("SIGINT", interrupt);
  process.on("SIGTERM", interrupt);
  const lease = async (op: 5 | 6) => {
    for (const arm of ["L", "R"] as const) {
      const m = nextMagic();
      const ack = await session.sendPb(9, framebufferLease(op, m), m, { arm, ackTimeoutMs: arm === "L" ? 200 : 5000 });
      if (arm === "R" && !ack) throw new Error(`${arm}: lease ${op} not acknowledged`);
    }
  };
  try {
    const settings = await querySettings(session, nextMagic());
    console.log(`firmware: L=${settings?.leftSoftwareVersion} R=${settings?.rightSoftwareVersion} head-up switch=${settings?.headUpSwitchRestored} angle=${settings?.headUpAngleRestored}`);
    const cfw = await queryGlasslyCfw(session, nextMagic());
    console.log(`CFW: ${cfw?.raw ?? "not detected"}`);
    if (!cfw) throw new Error("Custom firmware required");
    console.log(`arming head-up: switch=1 angle=${angle}`);
    if (!await setHeadUp(session, nextMagic(), { headUpSwitch: 1 })) throw new Error("head-up switch not acknowledged");
    if (!await setHeadUp(session, nextMagic(), { headUpAngle: angle })) throw new Error("head-up angle not acknowledged");
    await sleep(500);
    const after = await querySettings(session, nextMagic());
    console.log(`echo: head-up switch=${after?.headUpSwitchRestored} angle=${after?.headUpAngleRestored}`);
    console.log(await readDiag(session));

    let ups = 0, diags = 0, events = 0, ready = false;
    off = session.onEvent((ev, frame) => {
      if (ev.kind !== "sys-event" && ev.kind !== "list-click" && ev.kind !== "text-click") return;
      events++;
      const type = Number(ev.eventType);
      const source = ev.kind === "sys-event" ? Number(ev.eventSource) : NaN;
      let extra = "";
      if (type === 127) { diags++; extra = ` bits=0x${source.toString(16)} [${decodeBits(source)}]`; }
      console.log(`${new Date().toISOString()} ${ev.kind} ${names[type] ?? type} type=${type} source=${Number.isNaN(source) ? "n/a" : source}${extra} pb=${Buffer.from(frame.pb).toString("hex")}`);
      if (ready && ev.kind === "sys-event" && type === 12) console.log(`CONFIRMED head-up ${++ups}/${required}`);
    });
    if (withPage) {
      const create = buildCreateStartUpPageContainer({ name: `h${Date.now() % 10000}`, items: ["Head-up test"], containerId: 1, captureEvents: false, magic: nextMagic() });
      created = true;
      if (!await session.sendPb(0xe0, create.pb, create.magic, { ackTimeoutMs: 8000 })) throw new Error("CREATE not acknowledged");
      heartbeat = startHeartbeat({ session, nextMagic });
      leased = true;
      await lease(5);
    }
    ready = true;
    console.log(`READY (${withPage ? "page up, leases held" : "no page"}): lift your head ${required} times, a few seconds apart. Listening for ${seconds}s.`);
    const deadline = Date.now() + seconds * 1000;
    let renewAt = Date.now() + 30000, diagAt = Date.now() + diagEvery, lastDiag = "";
    while (Date.now() < deadline && ups < required && !interrupted) {
      await sleep(100);
      if (leased && Date.now() >= renewAt) { await lease(5); renewAt = Date.now() + 30000; }
      if (Date.now() >= diagAt) {
        const diag = await readDiag(session);
        // Counters only change on input, so a fast poll stays quiet between lifts.
        if (diag !== lastDiag || diagEvery >= 10000) console.log(`${new Date().toISOString()} ${diag}`);
        lastDiag = diag;
        diagAt = Date.now() + diagEvery;
      }
    }
    console.log(await readDiag(session));
    console.log(`${ups >= required ? "PASS" : interrupted ? "INTERRUPTED" : "FAIL"}: ${ups}/${required} head-up events, ${diags} diagnostic echoes, ${events} input events total.`);
    return ups >= required ? 0 : 1;
  } finally {
    heartbeat?.stop();
    off?.();
    if (leased) await lease(6).catch(error => console.error("Lease release:", error));
    if (created) {
      const shutdown = buildShutDown({ magic: nextMagic() });
      await session.sendPb(0xe0, shutdown.pb, shutdown.magic, { ackTimeoutMs: 3000 }).catch(console.error);
    }
    await session.close();
    process.off("SIGINT", interrupt);
    process.off("SIGTERM", interrupt);
  }
}

main().then(code => process.exit(code), error => { console.error(error); process.exit(1); });
