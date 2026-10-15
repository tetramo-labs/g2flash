#!/usr/bin/env bun
// Live head-up test against GLASSLYCFW/45+: arms the stock head-up switch and
// angle, opens an EvenHub page under both framebuffer leases (the same state
// the phone keeps), then prints every sys event: 12 is a head lift, 15 the
// head returning.
// bun head-up-test.ts [--timeout 120] [--count 3] [--angle 10] [--no-page]
import { G2Session, buildCreateStartUpPageContainer, buildShutDown, querySettings, setHeadUp } from "g2-kit/ble";
import { startHeartbeat } from "g2-kit/ui";
import { queryGlasslyCfw } from "./glassly-cfw";
import { framebufferLease } from "./vector-protocol";

const args = process.argv.slice(2);
function option(name: string, fallback: number): number {
  const i = args.indexOf(name);
  const value = i < 0 ? fallback : Number(args[i + 1]);
  if (!Number.isInteger(value) || value <= 0) throw new Error(`${name} needs a positive integer`);
  return value;
}
if (args.includes("--help")) {
  console.log("bun head-up-test.ts [--timeout 120] [--count 3] [--angle 10] [--no-page]\nWait for READY, then lift your head. Exit 0 requires head-up events (12).");
  process.exit(0);
}
const seconds = option("--timeout", 120);
const required = option("--count", 3);
const angle = option("--angle", 10);
const withPage = !args.includes("--no-page");
let magic = 100;
const nextMagic = () => magic = magic >= 255 ? 100 : magic + 1;
const sleep = (ms: number) => new Promise<void>(resolve => setTimeout(resolve, ms));
const names: Record<number, string> = { 0: "tap", 1: "swipe-up", 2: "swipe-down", 3: "double-tap", 7: "system-exit", 9: "long-press", 10: "release", 11: "tap-then-hold", 12: "HEAD-UP", 14: "ring-touch-down", 15: "HEAD-DOWN", 127: "unknown" };

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

    let ups = 0, events = 0, ready = false;
    off = session.onEvent((ev, frame) => {
      if (ev.kind !== "sys-event" && ev.kind !== "list-click" && ev.kind !== "text-click") return;
      events++;
      const type = Number(ev.eventType);
      const source = ev.kind === "sys-event" ? Number(ev.eventSource) : NaN;
      console.log(`${new Date().toISOString()} ${ev.kind} ${names[type] ?? type} type=${type} source=${Number.isNaN(source) ? "n/a" : source} pb=${Buffer.from(frame.pb).toString("hex")}`);
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
    let renewAt = Date.now() + 30000;
    while (Date.now() < deadline && ups < required && !interrupted) {
      await sleep(100);
      if (leased && Date.now() >= renewAt) { await lease(5); renewAt = Date.now() + 30000; }
    }
    console.log(`${ups >= required ? "PASS" : interrupted ? "INTERRUPTED" : "FAIL"}: ${ups}/${required} head-up events, ${events} input events total.`);
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
