import {describe, test, expect} from "bun:test";
import {fromBinary} from "@bufbuild/protobuf";
import {APPRequestSettingType, G2SettingPackageSchema, g2_settingCommandId} from "./node_modules/g2-kit/ble/gen/g2_setting_pb";
import {framebufferLease, svgPath, rotate, path, scene, shape, traceBitmap, contourSvg} from "./vector-protocol";
import {hash31, parseReply, putObject, show, withEpoch, REPLY} from "./object-cache";

describe("framebuffer lease setup", () => {
  test("acquire, renew and release include a settings read that produces a reply", () => {
    for (const op of [5, 5, 6] as const) for (const magic of [100, 127, 128, 255]) {
      const pb = framebufferLease(op, magic);
      const request = fromBinary(G2SettingPackageSchema, pb);
      expect(request.commandId).toBe(g2_settingCommandId.DeviceReceiveRequest);
      expect(request.magicRandom).toBe(magic);
      expect(request.deviceReceiveRequestFromApp?.settingInfoType).toBe(APPRequestSettingType.APP_REQUIRE_BASIC_SETTING);
      expect(request.deviceReceiveInfoFromApp).toBeUndefined();
      expect([...pb.slice(-9)]).toEqual([0xaa, 6, 6, 70, 67, 1, op, 1, 0]);
    }
  });
});

describe("revision 38 encoder", () => {
  test("rotation op wire fixture and bounds (slot 2 is object id 3)", () => {
    expect(rotate(2,90,140,120,1000)).toEqual([9,3,0,0,0, 0,90,0,0, 140,0, 120,0, 232,3, 0,0,255,255]);
    expect(() => rotate(256,0,0,0)).toThrow();
    expect(() => rotate(0,0,0,0,65536)).toThrow();
    expect(() => rotate(0,36001,0,0)).toThrow();
  });
  test("path record fixture and normalized equivalent syntax", () => {
    const expected=[0,0,0,0,0,1,160,0,0,0,1,160,0,160,0,4];
    expect(svgPath("M0,0 10,0 10,10 Z")).toEqual(expected);
    expect(svgPath("m0 0 h10 v10z")).toEqual(expected);
    expect(path(0,expected).record.slice(0,12)).toEqual([18,1,15,0, 0,0, 0,0, 0,1, 16,0]);
  });
  test("a scene is one SHOW: embedded puts, references in slot order, ops", () => {
    const box = shape(1,3,[100,100,80,40]);
    const version = hash31(box.record.join(","));
    const msg = scene([box], {ops:[rotate(1,90,140,120,700)], bg: 2});
    expect([...msg.slice(0,8)]).toEqual([38, 0,0, 0,0, 0x01, 2, 1]);            // mode, epoch 0, request 0, PRESENT, bg, one put
    expect([...msg.slice(8,17)]).toEqual([1, 2,0,0,0, ...[version & 255, (version>>>8)&255, (version>>>16)&255, (version>>>24)&255]]);
    expect([...msg.slice(17,37)]).toEqual(box.record);
    expect([...msg.slice(37,39)]).toEqual([1,0]);                                 // one reference
    expect([...msg.slice(39,43)]).toEqual([2,0,0,0]);
    expect(msg.length).toBe(47 + 19);
    expect([...withEpoch(msg, 0x1234).slice(0,5)]).toEqual([38, 0x34, 0x12, 0, 0]);
    expect([...withEpoch(msg, 0x1234, 0x0102).slice(0,5)]).toEqual([38, 0x34, 0x12, 2, 1]);
    expect([...withEpoch(scene([box], {request: 9}), 1, 7).slice(3,5)]).toEqual([9, 0]);
    expect(() => scene([], {keep:true, refs:[box]})).toThrow();
    expect([...scene([], {keep:true, ops:[rotate(1,0,0,0)]}).slice(5,8)]).toEqual([0x03, 0, 0]);
  });
  test("replies decode", () => {
    const applied = parseReply(Uint8Array.from([38, 7,0, 0, 0x34,0x12, 5,0,0,0, 2]));
    expect(applied).toEqual({mode:38, request:7, status:REPLY.APPLIED, epoch:0x1234, revision:5, evicted:2});
    const missing = parseReply(Uint8Array.from([38, 1,0, 2, 1,0, 0,0,0,0, 2, 1, 9,0,0,0, 0,1,0,0]));
    expect(missing?.missing).toEqual([9, 256]); expect(missing?.more).toBe(true);
    const snap = parseReply(Uint8Array.from([40, 0,0, 7, 1,0, 3,0,0,0, 1,0, 0,0, 1, 0, 5,0,0,0, 2,0,0,0, 9,0,0,0]));
    expect(snap?.entries).toEqual([{asset:false, id:5, version:2, lastUse:9}]);
    expect(parseReply(Uint8Array.from([1,2,3]))).toBeNull();
    expect(() => putObject(0, 1, [])).toThrow();
    expect(() => show({refs:[{id:1, version:0}]})).toThrow();
  });
  test("smooth curve reflection and exponents", () => {
    expect(svgPath("M0 0 Q10 20 20 0 T40 0Z")).toEqual(svgPath("M0 0 Q10 20 20 0 Q30 -20 40 0Z"));
    expect(svgPath("M0 0 C1 2 3 4 5 6 S9 10 11 12Z")).toEqual(svgPath("M0 0 C1 2 3 4 5 6 C7 8 9 10 11 12Z"));
    expect(svgPath("M1e1 0h10v10z")).toEqual(svgPath("M10 0L20 0L20 10Z"));
  });
  test("unsupported and malformed paths fail explicitly", () => {
    for(const d of ["", "M0 0L1", "M0 0L1 1", "M0 0A10 10 0 0 0 2 2Z", "M0 0@Z", "Z", "M0 0M2 2Z", "M3000 0Z"])
      expect(() => svgPath(d)).toThrow();
  });
});

describe("Bad Apple contour tracing", () => {
  test("holes, disjoint regions, diagonal contacts and merged straight runs", () => {
    const ring = new Uint8Array([1,1,1,1,0,1,1,1,1]);
    const contours=traceBitmap(ring,3,3);
    expect(contours.length).toBe(2);expect(contours.map(c=>c.length)).toEqual([4,4]);
    expect(traceBitmap(new Uint8Array([1,0,0,1]),2,2).length).toBe(2);
    expect(traceBitmap(new Uint8Array(100).fill(1),10,10)[0].length).toBe(4);
    expect(svgPath(contourSvg([],1))).toEqual([0,0,0,0,0,4]);
  });
  test("exhaustive 3x3 masks preserve every pixel under even-odd fill", () => {
    for(let mask=0;mask<512;mask++) {
      const bits=Uint8Array.from({length:9},(_,i)=>(mask>>i)&1);
      const contours=traceBitmap(bits,3,3);
      for(let y=0;y<3;y++)for(let x=0;x<3;x++) {
        let hits=0;
        for(const pts of contours)for(let i=0;i<pts.length;i++) {
          const a=pts[i],b=pts[(i+1)%pts.length],py=y+0.5,px=x+0.5;
          if((a.y>py)!==(b.y>py) && px<a.x+(py-a.y)*(b.x-a.x)/(b.y-a.y))hits++;
        }
        expect(hits%2).toBe(bits[y*3+x]);
      }
    }
  });
});
