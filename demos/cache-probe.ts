#!/usr/bin/env bun
// Object-cache probe: opens a session with the stock carrier page and the
// framebuffer lease held (as the phone app and the demos do), resets both
// caches to one epoch, then sends the hex messages given on the command line
// and prints each lens's cache reply and ACK. Cache messages may leave their
// epoch/request bytes as 0; the link fills them in.
//
//     bun cache-probe.ts 28 000000000000                 # STATE query (epoch/revision/page filled/zero)
//     bun cache-probe.ts --no-reset 2500000100            # an empty PUT
//     bun cache-probe.ts --targets right 2500000100
//
// Needs the glassly-cfw firmware, revision 38 or later.
import { G2Session, buildCreateStartUpPageContainer, querySettings } from "g2-kit/ble";
import { startHeartbeat } from "g2-kit/ui";
import { CacheLink, CfwTransport } from "./cfw-transport";
import { framebufferLease } from "./vector-protocol";
import { describeReply, REPLY } from "./object-cache";
import { describeCfw, queryGlasslyCfw, REQUIRED_REVISION } from "./glassly-cfw";

const args = process.argv.slice(2);
const noReset = args.includes("--no-reset");
const targets = args.includes("--targets") ? { left: 1, right: 2, both: 3 }[args[args.indexOf("--targets") + 1] ?? "both"] ?? 3 : 3;
const messages = args.filter((a) => !a.startsWith("--")).map((h) => Uint8Array.from(Buffer.from(h.replace(/\s+/g, ""), "hex")));
if (!messages.length) { console.log("usage: bun cache-probe.ts [--no-reset] <hex message>..."); process.exit(2); }
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));
let magic = 100;
const nextMagic = () => (magic = magic >= 255 ? 100 : magic + 1);
const session = await G2Session.open();
const settings = await querySettings(session, nextMagic());
if (settings) console.log(`firmware: L=${settings.leftSoftwareVersion} R=${settings.rightSoftwareVersion}`);
let cfw = await queryGlasslyCfw(session, nextMagic());
if (!cfw) cfw = await queryGlasslyCfw(session, nextMagic());
if (!cfw || cfw.revision < REQUIRED_REVISION) { console.log(describeCfw(cfw)); await session.close(); process.exit(1); }
console.log(`CFW detected: ${cfw.raw}`);
const hb = startHeartbeat({ session, nextMagic });
const create = buildCreateStartUpPageContainer({ name: `p${String(Date.now() % 10000).padStart(4, "0")}`, items: ["."], containerId: 1, captureEvents: false, magic: nextMagic() });
if (!(await session.sendPb(0xe0, create.pb, create.magic, { ackTimeoutMs: 8000 }))) throw new Error("CREATE did not ack");
await sleep(300);
const lease = async (op: 5 | 6) => { const m = nextMagic(); await session.sendPb(9, framebufferLease(op, m), m, { ackTimeoutMs: 8000 }); };
await lease(5);
const transport = new CfwTransport(session, 8000);
const link = new CacheLink(transport, targets);
const show = (name: string) => {
  for (const [lens, r] of transport.lastReplies) console.log(`  ${lens === 1 ? "L" : "R"} reply: mode ${r.mode} request ${r.request} ${describeReply(r)} epoch ${r.epoch} revision ${r.revision}${r.entries ? ` entries ${JSON.stringify(r.entries)}` : ""}`);
  console.log(`  ${name}: ${transport.lastOutcome}`);
};
try {
  if (!noReset) { await link.reset(); console.log(`reset: epoch ${link.epoch}, store ${link.storeKiB} KiB`); show("reset"); }
  for (const m of messages) {
    const t0 = performance.now();
    try { await link.send(m, { allow: Object.values(REPLY) }); } catch (e) { console.log(`  error: ${(e as Error).message}`); }
    show(`mode ${m[0]} (${m.length} B, ${(performance.now() - t0).toFixed(0)} ms)`);
  }
} finally {
  transport.close();
  await lease(6).catch(() => {});
  hb.stop();
  await session.close();
}
process.exit(0);
