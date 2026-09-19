// Glassly CFW revision detection.
//
// The firmware appends protobuf field 100 to the sid=0x09 settings READ
// response. Since revision 25 the string is just "GLASSLYCFW/<n>"; there are no
// feature tokens, so scripts gate on the revision number. Older builds sent
// "GLASSLYCFW/<n> <tokens>" or "EVENCFW/<n> <tokens>", which parse to their
// revision as well. g2-kit's queryCapabilities only accepts the EVENCFW prefix,
// so this does the same settings read and parses field 100 itself.

import { create, toBinary } from "@bufbuild/protobuf";
import type { G2SessionLike } from "g2-kit/ble";
import { SID_UI_SETTING } from "g2-kit/ble";
import {
  APPRequestSettingType,
  G2SettingPackageSchema,
  g2_settingCommandId,
} from "./node_modules/g2-kit/ble/gen/g2_setting_pb";

export const GLASSLY_CFW_MAGIC = "GLASSLYCFW/";
export const LEGACY_CFW_MAGIC = "EVENCFW/";

export interface GlasslyCfw {
  /** Firmware revision after the slash (25 for "GLASSLYCFW/25"). */
  revision: number;
  /** True for the GLASSLYCFW prefix, false for a legacy EVENCFW build. */
  branded: boolean;
  /** The full advertised string, verbatim. */
  raw: string;
  /** Settings field 107 (revision 36+): failure diagnostics, or null on older builds. */
  diag: GlasslyDiag | null;
}

/** Field 107 of the settings reply: the counters the debug overlay shows. */
export interface GlasslyDiag {
  nackCount: number; nackReason: number; workerFailMode: number; workerFailCount: number;
  gateTimeouts: number; execTimeouts: number; execState: number; execLast: number; execWaitMs: number;
  directPending: number; directActive: number; gateHeld: number;
  allocFailCount: number; allocFailHeap: number; lastWorkerUs: number;
}

export function parseGlasslyDiag(settingsPb: Uint8Array): GlasslyDiag | null {
  const d = findLenDelimField(settingsPb, 107);
  if (!d || d.length < 23 || d[0] !== 1) return null;
  const u16 = (i: number) => d[i]! | (d[i + 1]! << 8);
  return {
    nackCount: u16(1), nackReason: d[3]!, workerFailMode: d[4]!, workerFailCount: u16(5),
    gateTimeouts: u16(7), execTimeouts: u16(9), execState: d[11]!, execLast: d[12]!, execWaitMs: u16(13),
    directPending: d[15]!, directActive: d[16]!, gateHeld: d[17]!,
    allocFailCount: u16(18), allocFailHeap: d[20]!, lastWorkerUs: u16(21) * 100,
  };
}

const NACK_REASONS = ["", "flags", "context", "inflate", "crc", "handler"];

export function describeDiag(g: GlasslyDiag | null): string {
  if (!g) return "diag: n/a (needs revision 36)";
  return `diag: nack ${g.nackCount} (${NACK_REASONS[g.nackReason] ?? g.nackReason}) fail ${g.workerFailCount} mode ${g.workerFailMode}`
    + ` gate-timeouts ${g.gateTimeouts} pending ${g.directPending} active ${g.directActive} held ${g.gateHeld}`
    + ` alloc-fail ${g.allocFailCount} heap ${g.allocFailHeap} worker ${g.lastWorkerUs} us`;
}

// First top-level length-delimited field `fieldNo` of a protobuf message.
export function findLenDelimField(buf: Uint8Array, fieldNo: number): Uint8Array | null {
  let p = 0;
  const readVarint = (): number => {
    let v = 0, shift = 0;
    while (p < buf.length) {
      const b = buf[p++]!;
      v |= (b & 0x7f) << shift;
      if ((b & 0x80) === 0) break;
      shift += 7;
    }
    return v >>> 0;
  };
  while (p < buf.length) {
    const tag = readVarint();
    const field = tag >>> 3;
    const wire = tag & 7;
    if (wire === 0) readVarint();
    else if (wire === 2) {
      const len = readVarint();
      if (field === fieldNo) return buf.subarray(p, p + len);
      p += len;
    } else if (wire === 5) p += 4;
    else if (wire === 1) p += 8;
    else return null;
  }
  return null;
}

export function parseGlasslyCfw(settingsPb: Uint8Array): GlasslyCfw | null {
  const bytes = findLenDelimField(settingsPb, 100);
  if (!bytes) return null;
  const raw = new TextDecoder("utf-8", { fatal: false }).decode(bytes).replace(/\0+$/, "");
  const branded = raw.startsWith(GLASSLY_CFW_MAGIC);
  if (!branded && !raw.startsWith(LEGACY_CFW_MAGIC)) return null;
  const head = raw.split(/\s+/)[0] ?? "";
  const revision = Number(head.slice(head.indexOf("/") + 1));
  if (!Number.isInteger(revision) || revision <= 0) return null;
  return { revision, branded, raw, diag: parseGlasslyDiag(settingsPb) };
}

/** Settings read plus field-100 parse. Null on stock firmware or ack timeout. */
export async function queryGlasslyCfw(session: G2SessionLike, magic: number): Promise<GlasslyCfw | null> {
  const req = create(G2SettingPackageSchema, {
    commandId: g2_settingCommandId.DeviceReceiveRequest,
    magicRandom: magic,
    deviceReceiveRequestFromApp: {
      settingInfoType: APPRequestSettingType.APP_REQUIRE_BASIC_SETTING,
    },
  });
  const pb = toBinary(G2SettingPackageSchema, req);
  const ack = await session.sendPb(SID_UI_SETTING, pb, magic, { ackTimeoutMs: 4000 });
  if (!ack) return null;
  return parseGlasslyCfw(ack.pb);
}

/** Revision that introduced the bare numeric string; every demo here needs it. */
export const REQUIRED_REVISION = 31;

export function describeCfw(cfw: GlasslyCfw | null): string {
  return cfw ? `CFW ${cfw.raw}` : "no CFW capability field";
}
