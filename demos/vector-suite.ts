#!/usr/bin/env bun
/** Run `bun vector-suite.ts --help`. Offline by default; --device uses BLE. */
import {vectorCases, type VectorStep} from "./vector-cases";
import {badAppleVectors} from "./bad-apple-vector";
import {loadSvgVideo} from "./svg-video";
import {framebufferLease, u32 as i32, u16} from "./vector-protocol";

const args=process.argv.slice(2);
const option=(name:string)=>{const i=args.indexOf(name);if(i<0)return undefined;const v=args[i+1];if(!v || v.startsWith("--"))throw new Error(`Missing value for ${name}`);return v;};
if(args.includes("--help")) {
  console.log(`Revision 38 SVG paths and rotation suite on the object cache (offline by default)
  bun vector-suite.ts --dump /tmp/vector-suite.bin
  bun vector-suite.ts --device                  # visual checks on glasses
  bun vector-suite.ts --bad-apple --dump /tmp/bad-apple-vector.bin
  bun vector-suite.ts --bad-apple --device       # clocked 10 fps playback
  bun vector-suite.ts --svg-video FILE --device  # prepared smooth SVG video
  bun vector-suite.ts --bad-apple --svg-out /tmp/bad-apple-svg
  Options: --frames N, --gif FILE, --idle SECONDS, --list, --dry-run, --help
  --idle holds the connection open (heartbeat + lease renewals) for that long
  before playback, e.g. --idle 75 to start past the stock 60 s slow-mode timer.
Host replay checks pixel probes. Device ACKs only confirm transport;
inspect the display against the named test and generated host images.`);
  process.exit(0);
}
const frameCount=Number(option("--frames")??300);
const idleSeconds=Number(option("--idle")??0);
if(!Number.isFinite(idleSeconds)||idleSeconds<0||idleSeconds>600)throw new Error("--idle must be 0..600 seconds");
if(!Number.isInteger(frameCount)||frameCount<1||frameCount>10000)throw new Error("--frames must be 1..10000");
const prepared=new URL(".cache/bad-apple/smooth/video.json",import.meta.url).pathname;
const svgVideo=option("--svg-video") ?? (args.includes("--bad-apple") && !option("--gif") && await Bun.file(prepared).exists()?prepared:undefined);
const video=args.includes("--bad-apple") || !!svgVideo;
const steps=svgVideo?await loadSvgVideo(svgVideo,frameCount,option("--svg-out")):
  video?await badAppleVectors(option("--gif")??new URL("bad_apple_quarter.gif",import.meta.url).pathname,frameCount,option("--svg-out")):vectorCases();
const frameMs=video?(steps.find(s=>s.name!=="reset")?.waitMs??100):100;
if(args.includes("--list")){console.log(steps.map(s=>s.name).join("\n"));process.exit(0);}

const dump=option("--dump");
if(dump) {
  const bytes:number[]=[];
  for(const step of steps) {
    const label=[...new TextEncoder().encode(step.name)];
    if(label.length>255 || step.payload.length>65535)throw new Error("Dump record exceeds format limit");
    bytes.push(3,label.length,...label,1,...u16(step.payload.length),...step.payload,2,...i32(Math.round(step.waitMs)));
    for(const [x,y,color] of step.probes??[])bytes.push(4,...u16(x),...u16(y),color);
    if(step.snapshot)bytes.push(5);
  }
  await Bun.write(dump,Uint8Array.from(bytes));console.log(`wrote ${dump}: ${steps.length} steps`);
}
if(!args.includes("--device") || args.includes("--dry-run")) {
  console.log(`offline: ${steps.length} steps, ${steps.reduce((n,s)=>n+s.payload.length,0)} encoded bytes; nothing sent`);
  process.exit(0);
}

const {G2Session,buildCreateStartUpPageContainer,querySettings}=await import("g2-kit/ble");
const {CfwTransport,CacheLink}=await import("./cfw-transport");
const {queryGlasslyCfw,REQUIRED_REVISION}=await import("./glassly-cfw");
const {startHeartbeat}=await import("g2-kit/ui");
const sleep=(ms:number)=>new Promise(r=>setTimeout(r,ms));
let magic=100;
const nextMagic=()=>magic=magic>=255?100:magic+1;
const session=await G2Session.open();
let heartbeat:ReturnType<typeof startHeartbeat>|undefined,renew:ReturnType<typeof setInterval>|undefined;
const suffix=String(Date.now()%10000).padStart(4,"0");
const lease=async(op:5|6)=>{
  const m=nextMagic();
  const pb=framebufferLease(op,m);
  if(!await session.sendPb(9,pb,m,{ackTimeoutMs:8000}))throw new Error("Framebuffer lease did not ACK");
};
// GLASSLYCFW/31: custom payloads ride the SID-0xf0 message transport (no image container).
const transport=new CfwTransport(session);
const link=new CacheLink(transport);
const send=(payload:Uint8Array)=>link.send(payload);
let acquired=false;
try {
  await querySettings(session,nextMagic());
  let cfw=await queryGlasslyCfw(session,nextMagic());if(!cfw)cfw=await queryGlasslyCfw(session,nextMagic());
  if(!cfw || cfw.revision<REQUIRED_REVISION)throw new Error(`Requires GLASSLYCFW/${REQUIRED_REVISION} or later; got ${cfw?.raw??"no capability response"}`);
  console.log(`firmware: ${cfw.raw}`);
  heartbeat=startHeartbeat({session,nextMagic});
  const create=buildCreateStartUpPageContainer({name:`s${suffix}`,items:["."],containerId:1,captureEvents:false,magic:nextMagic()});
  if(!await session.sendPb(0xe0,create.pb,create.magic,{ackTimeoutMs:8000}))throw new Error("CREATE did not ACK");
  await sleep(300);await lease(5);acquired=true;
  await link.reset();   /* one session epoch for both lenses; the fixtures' own reset step is skipped */
  if(idleSeconds>0){console.log(`idling ${idleSeconds}s with heartbeat and lease renewals before playback`);await sleep(idleSeconds*1000);}
  let leaseError:unknown;
  renew=setInterval(()=>void lease(5).catch(e=>{leaseError=e;}),30000);
  let sent=0,skipped=0,worst=0;const start=performance.now();
  for(let i=0;i<steps.length;i++) {
    if(leaseError)throw leaseError;
    if(steps[i].name==="reset")continue;
    if(video && i>0 && steps[i].name!=="release") {
      const due=Math.min(steps.length-2,Math.floor((performance.now()-start)/frameMs));
      if(due>i){skipped+=due-i;i=due;}
    }
    const step:VectorStep=steps[i],before=performance.now();
    await send(step.payload);sent++;worst=Math.max(worst,performance.now()-before);
    if(!video)console.log(`${step.name}: ${step.payload.length} B; inspect expected display${step.probes?` (${step.probes.length} host pixel probes)`:""}`);
    const wait=video?(i+1)*frameMs-(performance.now()-start):step.waitMs;
    if(step.name!=="release" && wait>0)await sleep(wait);
  }
  console.log(`${sent} payloads ACKed, ${skipped} video frames skipped; worst ACK ${worst.toFixed(0)} ms`);
} finally {
  if(renew)clearInterval(renew);
  if(acquired){await lease(6).catch(()=>{});}
  heartbeat?.stop();transport.close();await session.close();
}
// Noble's macOS adapter keeps native handles alive after the lenses disconnect.
// All display cleanup above is awaited before exiting, as in the other demos.
process.exit(0);
