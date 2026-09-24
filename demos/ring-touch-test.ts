#!/usr/bin/env bun
// Live acceptance test: event 14 is ring finger-down, NOT long-press (9).
// bun ring-touch-test.ts [--timeout 120] [--count 3]
import { G2Session, buildCreateStartUpPageContainer, buildShutDown, querySettings } from "g2-kit/ble";
import { startHeartbeat } from "g2-kit/ui";
import { findLenDelimField, queryGlasslyCfw } from "./glassly-cfw";
import { framebufferLease } from "./vector-protocol";

const args = process.argv.slice(2);
function option(name: string, fallback: number): number {
  const i = args.indexOf(name);
  const value = i < 0 ? fallback : Number(args[i + 1]);
  if (!Number.isInteger(value) || value <= 0) throw new Error(`${name} needs a positive integer`);
  return value;
}
if (args.includes("--help")) {
  console.log("bun ring-touch-test.ts [--timeout 120] [--count 3]\nWait for READY, then touch and release the ring three times. Exit 0 requires ring touch-down events (14). No firmware flashing.");
  process.exit(0);
}
const seconds = option("--timeout", 120);
const required = option("--count", 3);
let magic = 100;
const nextMagic = () => magic = magic >= 255 ? 100 : magic + 1;
const sleep = (ms: number) => new Promise<void>(resolve => setTimeout(resolve, ms));
const names: Record<number, string> = { 0: "tap", 1: "swipe-up", 2: "swipe-down", 3: "double-tap", 9: "long-press", 10: "release", 11: "tap-then-hold", 12: "head-up", 14: "TOUCH-DOWN", 127: "unknown-ring-report" };

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
      // Settings replies are master/right-only. The left write still acquires
      // its independent lease, which is needed when the ring connects there.
      const ack = await session.sendPb(9, framebufferLease(op, m), m, { arm, ackTimeoutMs: arm === "L" ? 200 : 5000 });
      if (arm === "R" && !ack)
        throw new Error(`${arm}: lease ${op} not acknowledged`);
    }
  };
  try {
    const settings = await querySettings(session, nextMagic());
    console.log(`firmware: L=${settings?.leftSoftwareVersion} R=${settings?.rightSoftwareVersion}`);
    const cfw = await queryGlasslyCfw(session, nextMagic());
    console.log(`CFW: ${cfw?.raw ?? "not detected"}`);
    if (!cfw) throw new Error("Custom firmware required for ring touch-down forwarding");
    let downs = 0, events = 0, ready = false;
    const seenTicks = new Set<number>();
    off = session.onEvent((ev, frame) => {
      if (ev.kind !== "sys-event" && ev.kind !== "list-click" && ev.kind !== "text-click") return;
      events++;
      const source = ev.kind === "sys-event" ? Number(ev.eventSource) : undefined;
      const dev = findLenDelimField(frame.pb, 13);
      const sys = dev && findLenDelimField(dev, 3);
      const ri = sys && findLenDelimField(sys, 100);
      const validRi = ri?.length === 12 && ri[0] === 82 && ri[1] === 73 && ri[2] === 1 && ri[3] === 1 && ri[7] === 0;
      const tick = validRi ? new DataView(ri.buffer, ri.byteOffset, ri.byteLength).getUint32(8, true) : undefined;
      const type = Number(ev.eventType);
      console.log(`${new Date().toISOString()} ${ev.kind} ${names[type] ?? type} type=${type} source=${source ?? "n/a"}${validRi ? ` wire=${ri[4]} aux=${ri[5]} speed=${ri[6]} tick=${tick}` : ""} pb=${Buffer.from(frame.pb).toString("hex")}`);
      // Older event-14 firmware leaves source unspecified; the event ID itself
      // is ring-specific. New RI reports must also agree on wire type 10.
      if (ready && ev.kind === "sys-event" && type === 14 && (source === 0 || source === 2) &&
          (!ri || (validRi && ri[4] === 10)) && (tick === undefined || !seenTicks.has(tick))) {
        if (tick !== undefined) seenTicks.add(tick);
        console.log(`CONFIRMED ring touch-down ${++downs}/${required}`);
      }
    });
    const create = buildCreateStartUpPageContainer({ name: `r${Date.now() % 10000}`, items: ["Ring touch test"], containerId: 1, captureEvents: false, magic: nextMagic() });
    created = true;
    if (!await session.sendPb(0xe0, create.pb, create.magic, { ackTimeoutMs: 8000 })) throw new Error("CREATE not acknowledged");
    heartbeat = startHeartbeat({ session, nextMagic });
    leased = true;
    await lease(5);
    ready = true;
    console.log(`READY: touch and release the ring ${required} times, about two seconds apart. Listening for ${seconds}s.`);
    const deadline = Date.now() + seconds * 1000;
    let renewAt = Date.now() + 30000;
    while (Date.now() < deadline && downs < required && !interrupted) {
      await sleep(100);
      if (Date.now() >= renewAt) { await lease(5); renewAt = Date.now() + 30000; }
    }
    console.log(`${downs >= required ? "PASS" : interrupted ? "INTERRUPTED" : "FAIL"}: ${downs}/${required} ring touch-down events; ${events} total input events.`);
    return downs >= required ? 0 : 1;
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
