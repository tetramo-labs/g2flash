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

/** Parse a raw notify frame into the reply it carries, or null. */
export function parseAckFrame(raw: Uint8Array): Ack | null {
  if (raw.length < 19 || raw[0] !== 0xaa || raw[3] + 8 !== raw.length || raw[6] !== CFW_SID) return null;
  const body = raw.subarray(8, raw.length - 2);
  const crc = crc16(body);
  if (raw[raw.length - 2] !== (crc & 0xff) || raw[raw.length - 1] !== crc >> 8) return null;
  if ((body[0] !== 1 && body[0] !== 3) || (body[4] !== 1 && body[4] !== 2)) return null;
  return { success: body[0] === 1, streamId: body[1], ordinal: body[2] | (body[3] << 8), lens: body[4],
    size: body[5] | (body[6] << 8), crc: body[7] | (body[8] << 8) };
}

/** ACK-gated sender: one stream per payload, both lenses must ack. */
export class CfwTransport {
  private sequence = Math.floor(Math.random() * 256);
  private waiter: ((ack: Ack) => void) | undefined;
  private readonly off: () => void;

  constructor(private readonly session: G2Session, private readonly ackTimeoutMs = 8000) {
    this.off = session.onRawFrame((_frame, raw) => {
      const ack = parseAckFrame(raw);
      if (ack) this.waiter?.(ack);
    });
  }

  /** Outcome of the last send: which lenses acked, which NACKed, or a timeout. */
  lastOutcome = "";

  /** Resolves true when both lenses acked, false on NACK or timeout (see lastOutcome). */
  async send(payload: Uint8Array): Promise<boolean> {
    const streamId = this.sequence;
    const frames = packets(record(payload), streamId);
    this.sequence = (this.sequence + frames.length) & 0xff;
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
        if (acked === LENS_BOTH) { clearTimeout(timer); this.waiter = undefined; this.lastOutcome = "acked by both"; resolve(true); }
      };
    });
    await sendFrames(this.session.left, frames);
    return done;
  }

  close(): void { this.off(); }
}
