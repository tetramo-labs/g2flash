#!/usr/bin/env bun
/** Run `bun vector-suite.ts --help`. Offline by default; --device uses BLE. */
import {vectorCases, type VectorStep} from "./vector-cases";
import {badAppleVectors} from "./bad-apple-vector";
import {loadSvgVideo} from "./svg-video";
import {framebufferLease, i32, u16} from "./vector-protocol";

const args=process.argv.slice(2);
const option=(name:string)=>{const i=args.indexOf(name);if(i<0)return undefined;const v=args[i+1];if(!v || v.startsWith("--"))throw new Error(`Missing value for ${name}`);return v;};
if(args.includes("--help")) {
  console.log(`Revision 21 SVG paths and rotation suite (offline by default)
  bun vector-suite.ts --dump /tmp/vector-suite.bin
  bun vector-suite.ts --device                  # visual checks on glasses
  bun vector-suite.ts --bad-apple --dump /tmp/bad-apple-vector.bin
  bun vector-suite.ts --bad-apple --device       # clocked 10 fps playback
  bun vector-suite.ts --svg-video FILE --device  # prepared smooth SVG video
  bun vector-suite.ts --bad-apple --svg-out /tmp/bad-apple-svg
  Options: --frames N, --gif FILE, --list, --dry-run, --help
Host replay checks pixel probes. Device ACKs only confirm transport;
inspect the display against the named test and generated host images.`);
  process.exit(0);
}
const frameCount=Number(option("--frames")??300);
if(!Number.isInteger(frameCount)||frameCount<1||frameCount>10000)throw new Error("--frames must be 1..10000");
const prepared=new URL(".cache/bad-apple/smooth/video.json",import.meta.url).pathname;
const svgVideo=option("--svg-video") ?? (args.includes("--bad-apple") && !option("--gif") && await Bun.file(prepared).exists()?prepared:undefined);
const video=args.includes("--bad-apple") || !!svgVideo;
const steps=svgVideo?await loadSvgVideo(svgVideo,frameCount,option("--svg-out")):
  video?await badAppleVectors(option("--gif")??new URL("bad_apple_quarter.gif",import.meta.url).pathname,frameCount,option("--svg-out")):vectorCases();
const frameMs=video?steps[0].waitMs:100;
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

const {G2Session,buildCreateStartUpPageContainer,buildImageContainers,buildImageRawData,planImageFragments,queryCapabilities,querySettings}=await import("g2-kit/ble");
const {startHeartbeat}=await import("g2-kit/ui");
const sleep=(ms:number)=>new Promise(r=>setTimeout(r,ms));
let magic=100,transfer=1;
const nextMagic=()=>magic=magic>=255?100:magic+1;
const session=await G2Session.open();
let heartbeat:ReturnType<typeof startHeartbeat>|undefined,renew:ReturnType<typeof setInterval>|undefined;
const suffix=String(Date.now()%10000).padStart(4,"0");
const container={name:`v${suffix}`,containerId:2,x:0,y:0,width:576,height:288};
const lease=async(op:5|6)=>{
  const m=nextMagic();
  const pb=framebufferLease(op,m);
  if(!await session.sendPb(9,pb,m,{ackTimeoutMs:8000}))throw new Error("Framebuffer lease did not ACK");
};
const send=async(payload:Uint8Array)=>{
  for(const frag of planImageFragments(payload,4000)) {
    const raw=buildImageRawData({containerId:2,containerName:container.name,mapSessionId:transfer,mapTotalSize:payload.length,
      mapFragmentIndex:frag.index,mapRawData:frag.data,magic:nextMagic(),compressMode:0});
    if(!await session.sendPb(0xe0,raw.pb,raw.magic,{ackTimeoutMs:8000}))throw new Error("Image payload did not ACK");
  }
  transfer++;
};
let acquired=false;
try {
  await querySettings(session,nextMagic());
  let caps=await queryCapabilities(session,nextMagic());if(!caps)caps=await queryCapabilities(session,nextMagic());
  const revision=Number(caps?.raw.match(/^EVENCFW\/(\d+)/)?.[1]??0);
  if(revision<21)throw new Error(`Requires EVENCFW/21 or later; got ${caps?.raw??"no capability response"}`);
  console.log(`firmware: ${caps!.raw}`);
  heartbeat=startHeartbeat({session,nextMagic});
  const create=buildCreateStartUpPageContainer({name:`s${suffix}`,items:["."],containerId:1,captureEvents:false,magic:nextMagic(),extraContainerNames:[container.name]});
  if(!await session.sendPb(0xe0,create.pb,create.magic,{ackTimeoutMs:8000}))throw new Error("CREATE did not ACK");
  const rebuild=buildImageContainers({containers:[container],magic:nextMagic()});
  if(!await session.sendPb(0xe0,rebuild.pb,rebuild.magic,{ackTimeoutMs:8000}))throw new Error("REBUILD did not ACK");
  await sleep(300);await lease(5);acquired=true;
  let leaseError:unknown;
  renew=setInterval(()=>void lease(5).catch(e=>{leaseError=e;}),30000);
  let sent=0,skipped=0,worst=0;const start=performance.now();
  for(let i=0;i<steps.length;i++) {
    if(leaseError)throw leaseError;
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
  if(acquired){await send(Uint8Array.from([18,2])).catch(()=>{});await lease(6).catch(()=>{});}
  heartbeat?.stop();await session.close();
}
// Noble's macOS adapter keeps native handles alive after the lenses disconnect.
// All display cleanup above is awaited before exiting, as in the other demos.
process.exit(0);
