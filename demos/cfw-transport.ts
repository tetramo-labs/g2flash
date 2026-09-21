// SID-0xf0 message-stream transport for the demos (GLASSLYCFW/31, g2flash
// patches/message_transport.c). Custom payloads no longer ride an EvenHub
// image container: each payload is one record in a stream of packets written
// to the LEFT arm, the options byte selects both lenses (the right lens is
// reached over the frame bridge) and each lens answers with an ACK or NACK
// notification through the left arm. `send_message_probe.py --dry-run` in the
// repository root prints the same packets for the same input.
//
// Each record is deflated on its own (zlib finishing with a SYNC_FLUSH, never
// Z_FINISH) and flagged RESET_CONTEXT, so the glasses start a fresh inflater
// per message; the phone apps keep one context across messages instead, but
// the demos favour simplicity over the per-message init cost.
import { deflateSync, constants } from "node:zlib";
import type { G2Session } from "g2-kit/ble";
import { sendFrames } from "g2-kit/ble";
import { control, describeReply, isCacheMessage, parseReply, REPLY, withEpoch, type Reply } from "./object-cache";

export const CFW_SID = 0xf0;
const LENS_BOTH = 3;
const FLAG_COMPRESSED = 4;
const FLAG_RESET_CONTEXT = 8;
const PACKET_RESET = 0x80;
const PACKET_END = 0x40;
/** Stream bytes per packet: the firmware caps a packet at 252, the demos' BLE writes at 232 payload bytes. */
const CHUNK = 229;

export function crc16(bytes: Uint8Array): number {
  let crc = 0xffff;
  for (const b of bytes) {
    crc ^= b << 8;
    for (let i = 0; i < 8; i++) crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
  }
  return crc;
}

/** One stream record: `[flags][len:u16][crc16 of the plain message][body]`. */
export function record(message: Uint8Array, targets = LENS_BOTH): Uint8Array {
  if (message.length > 0xffff) throw new Error(`message of ${message.length} bytes exceeds the 65535-byte record limit`);
  const crc = crc16(message);
  let body: Uint8Array = new Uint8Array(deflateSync(message, { finishFlush: constants.Z_SYNC_FLUSH }));
  let flags = targets | FLAG_RESET_CONTEXT | FLAG_COMPRESSED;
  if (body.length > 0xffff) { body = message; flags = targets | FLAG_RESET_CONTEXT; }
  const out = new Uint8Array(5 + body.length);
  out[0] = flags; out[1] = body.length & 0xff; out[2] = body.length >> 8; out[3] = crc & 0xff; out[4] = crc >> 8;
  out.set(body, 5);
  return out;
}

/** Split a stream into `[aa 21 seq len+2 1 1 f0 0][options][chunk][crc16 LE]` packets. */
export function packets(stream: Uint8Array, sequence: number, targets = LENS_BOTH, chunk = CHUNK): Uint8Array[] {
  const out: Uint8Array[] = [];
  let offset = 0, index = 0;
  do {
    const end = Math.min(offset + chunk, stream.length);
    let options = targets & LENS_BOTH;
    if (offset === 0) options |= PACKET_RESET;
    if (end >= stream.length) options |= PACKET_END;
    const body = new Uint8Array(1 + end - offset);
    body[0] = options; body.set(stream.subarray(offset, end), 1);
    const crc = crc16(body);
    const packet = new Uint8Array(10 + body.length);
    packet.set([0xaa, 0x21, (sequence + index) & 0xff, body.length + 2, 1, 1, CFW_SID, 0], 0);
    packet.set(body, 8);
    packet[8 + body.length] = crc & 0xff; packet[9 + body.length] = crc >> 8;
    out.push(packet);
    index++; offset = end;
  } while (offset < stream.length);
  return out;
}

export interface Ack { success: boolean; streamId: number; ordinal: number; lens: number; size: number; crc: number }
/** Kind-5 object-cache reply (GLASSLYCFW/38): sent by each lens before its ACK. */
export interface CacheReplyFrame { streamId: number; ordinal: number; lens: number; reply: Reply }

/** Parse a raw notify frame into the ACK/NACK it carries, or null. */
export function parseAckFrame(raw: Uint8Array): Ack | null {
  if (raw.length < 19 || raw[0] !== 0xaa || raw[3] + 8 !== raw.length || raw[6] !== CFW_SID) return null;
  const body = raw.subarray(8, raw.length - 2);
  const crc = crc16(body);
  if (raw[raw.length - 2] !== (crc & 0xff) || raw[raw.length - 1] !== crc >> 8) return null;
  if ((body[0] !== 1 && body[0] !== 3) || (body[4] !== 1 && body[4] !== 2)) return null;
  return { success: body[0] === 1, streamId: body[1], ordinal: body[2] | (body[3] << 8), lens: body[4],
    size: body[5] | (body[6] << 8), crc: body[7] | (body[8] << 8) };
}

/** Parse a raw notify frame into the cache reply it carries, or null. */
export function parseReplyFrame(raw: Uint8Array): CacheReplyFrame | null {
  if (raw.length < 15 || raw[0] !== 0xaa || raw[3] + 8 !== raw.length || raw[6] !== CFW_SID) return null;
  const body = raw.subarray(8, raw.length - 2);
  const crc = crc16(body);
  if (raw[raw.length - 2] !== (crc & 0xff) || raw[raw.length - 1] !== crc >> 8) return null;
  if (body[0] !== 5 || (body[4] !== 1 && body[4] !== 2)) return null;
  const reply = parseReply(body.subarray(5));
  return reply ? { streamId: body[1], ordinal: body[2] | (body[3] << 8), lens: body[4], reply } : null;
}

/** ACK-gated sender: one stream per payload, both lenses must ack. */
export class CfwTransport {
  private sequence = Math.floor(Math.random() * 256);
  private waiter: ((ack: Ack) => void) | undefined;
  private readonly off: () => void;

  constructor(private readonly session: G2Session, private readonly ackTimeoutMs = 8000) {
    this.off = session.onRawFrame((_frame, raw) => {
      const ack = parseAckFrame(raw);
      if (ack) { this.waiter?.(ack); return; }
      const reply = parseReplyFrame(raw);
      if (reply && reply.streamId === this.currentStream && reply.ordinal === 0) this.lastReplies.set(reply.lens, reply.reply);
    });
  }

  /** Outcome of the last send: which lenses acked, which NACKed, or a timeout. */
  lastOutcome = "";
  /** Cache replies (kind 5) of the last send, by lens (1 = left, 2 = right). */
  lastReplies = new Map<number, Reply>();
  private currentStream = -1;

  /** Resolves true when every targeted lens acked, false on NACK or timeout (see lastOutcome). */
  async send(payload: Uint8Array, targets = LENS_BOTH): Promise<boolean> {
    const streamId = this.sequence;
    const frames = packets(record(payload, targets), streamId, targets);
    this.sequence = (this.sequence + frames.length) & 0xff;
    this.currentStream = streamId;
    this.lastReplies = new Map();
    let acked = 0;
    const name = (lens: number) => (lens === 1 ? "L" : "R");
    const done = new Promise<boolean>((resolve) => {
      const timer = setTimeout(() => {
        this.waiter = undefined;
        this.lastOutcome = `timeout after ${this.ackTimeoutMs} ms, acked by ${acked ? name(acked & 1 || 2) : "neither lens"}${acked === 1 ? "" : acked === 2 ? "" : ""}`;
        if (acked === 1) this.lastOutcome = `timeout: L acked, R silent`;
        if (acked === 2) this.lastOutcome = `timeout: R acked, L silent`;
        resolve(false);
      }, this.ackTimeoutMs + payload.length / 8);
      this.waiter = (ack) => {
        if (ack.streamId !== streamId || ack.ordinal !== 0) return;
        if (!ack.success) { clearTimeout(timer); this.waiter = undefined; this.lastOutcome = `NACK from ${name(ack.lens)}`; resolve(false); return; }
        acked |= ack.lens;
        if ((acked & targets) === targets) { clearTimeout(timer); this.waiter = undefined; this.lastOutcome = targets === LENS_BOTH ? "acked by both" : `acked by ${name(targets)}`; resolve(true); }
      };
    });
    await sendFrames(this.session.left, frames);
    return done;
  }

  close(): void { this.off(); }
}

export class StaleEpochError extends Error {
  constructor(public readonly lens: number, public readonly epoch: number) { super(`lens ${lens === 1 ? "L" : "R"} is at cache epoch ${epoch}`); }
}

/**
 * Object-cache session over the transport: owns the epoch both lenses share,
 * fills it into every cache message and turns replies into results. A RESET
 * hands the phone-chosen epoch to both lenses; any lens that later answers
 * STALE (its cache was lost with the lease or rebooted) raises StaleEpochError
 * so the caller can reset and rebuild.
 */
export class CacheLink {
  epoch = 0;
  /** Smallest asset store among the lenses, in KiB, learned from the RESET replies. */
  storeKiB = 0;
  private request = 1;
  constructor(private readonly transport: CfwTransport, public targets = LENS_BOTH) {}

  nextRequest(): number { const r = this.request; this.request = this.request >= 0xffff ? 1 : this.request + 1; return r; }

  /** Empty both caches and adopt one fresh session epoch. */
  async reset(): Promise<void> {
    this.epoch = 1 + Math.floor(Math.random() * 0xfffe);
    const replies = await this.send(control.reset(this.nextRequest(), CHUNK + 10), { expectEpoch: false });
    for (const [lens, r] of replies) if (r.epoch !== this.epoch) throw new Error(`lens ${lens} kept epoch ${r.epoch} after a reset to ${this.epoch}`);
    this.storeKiB = Math.min(...[...replies.values()].map((r) => r.storeKiB ?? 0));
  }

  /**
   * Send one message. Cache messages get the session epoch and must be answered
   * APPLIED/UNCHANGED/DELTA/SNAPSHOT by both lenses; a STALE reply throws
   * StaleEpochError, any other status throws with the reply text. Non-cache
   * messages are passed through.
   */
  async send(message: Uint8Array, o: { expectEpoch?: boolean; allow?: number[] } = {}): Promise<Map<number, Reply>> {
    const cache = isCacheMessage(message);
    const wire = cache ? withEpoch(message, this.epoch, this.nextRequest()) : message;
    if (!(await this.transport.send(wire, this.targets))) throw new Error(`message (mode ${message[0]}, ${message.length} B): ${this.transport.lastOutcome}`);
    if (!cache) return new Map();
    const replies = this.transport.lastReplies;
    const ok = new Set(o.allow ?? [REPLY.APPLIED, REPLY.UNCHANGED, REPLY.DELTA, REPLY.SNAPSHOT]);
    for (const [lens, r] of replies) {
      if (r.status === REPLY.STALE && o.expectEpoch !== false) throw new StaleEpochError(lens, r.epoch);
      if (!ok.has(r.status)) throw new Error(`mode ${message[0]} refused by lens ${lens === 1 ? "L" : "R"}: ${describeReply(r)}`);
    }
    const expected = this.targets === LENS_BOTH ? 2 : 1;
    if (replies.size < expected && !(o.allow && o.allow.length === 0)) throw new Error(`mode ${message[0]}: cache reply missing from ${replies.size ? (replies.has(1) ? "R" : "L") : "both lenses"}`);
    return replies;
  }
}
