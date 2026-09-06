import {GifReader} from "omggif";
import {mkdir} from "node:fs/promises";
import {contourSvg, path, scene, svgPath, traceBitmap} from "./vector-protocol";
import type {VectorStep} from "./vector-cases";

/** Offline contour compiler. Exact contours at each sampled resolution; reduce
 * resolution explicitly until the one-slot 512-edge budget fits. No dropped holes. */
export async function badAppleVectors(gifFile: string, frames = 300, svgOut?: string): Promise<VectorStep[]> {
  const gif=new GifReader(new Uint8Array(await Bun.file(gifFile).arrayBuffer()));
  const rgba=new Uint8Array(gif.width*gif.height*4);
  const stride=Math.max(1,Math.round(100/Math.max(1,gif.frameInfo(0).delay)/10));
  const count=Math.min(frames,Math.floor(gif.numFrames()/stride));
  const steps: VectorStep[]=[];
  const histogram=new Map<number,number>();
  if(svgOut)await mkdir(svgOut,{recursive:true});
  for(let f=0;f<count;f++) {
    for(let s=0;s<stride;s++)gif.decodeAndBlitFrameRGBA(f*stride+s,rgba);
    let d="",edges=0,width=0, sampled=new Uint8Array(0);
    for(const w of [144,96,72,48,32,24,16]) {
      const h=w/2,bits=new Uint8Array(w*h);
      for(let y=0;y<h;y++)for(let x=0;x<w;x++) {
        const sx=Math.min(gif.width-1,Math.floor((x+0.5)*gif.width/w));
        const sy=Math.min(gif.height-1,Math.floor((y+0.5)*gif.height/h));
        const off=(sy*gif.width+sx)*4;
        bits[y*w+x]=(rgba[off]*0.299+rgba[off+1]*0.587+rgba[off+2]*0.114)>=128?1:0;
      }
      const contours=traceBitmap(bits,w,h);
      edges=contours.reduce((sum,c)=>sum+c.length,0);
      if(edges>512)continue;
      width=w;sampled=bits;d=contourSvg(contours,576/w);break;
    }
    if(!width)throw new Error(`Frame ${f} exceeds the edge budget at every resolution`);
    const commands=svgPath(d);
    histogram.set(width,(histogram.get(width)??0)+1);
    // Each frame replaces slot 0 in one validated transaction. No unrelated
    // scene objects need clearing; the retained scene redraw removes old ink.
    const probes: [number,number,number][]=[];
    let seed=f+1;
    for(let i=0;i<24;i++) {
      seed=(Math.imul(seed,1664525)+1013904223)>>>0;
      const index=seed%sampled.length,x=index%width,y=Math.floor(index/width),scale=576/width;
      probes.push([Math.floor(32+(x+0.5)*scale),Math.floor(96+(y+0.5)*scale),sampled[index]?15:0]);
    }
    steps.push({name:`bad-apple-${f}-edges-${edges}`,payload:scene([path(0,commands,"evenodd",32,96)],f===0),waitMs:100,
      probes,snapshot:[30,60,120,180,240,299].includes(f)});
    if(svgOut)await Bun.write(`${svgOut}/${String(f).padStart(4,"0")}.svg`,
      `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 576 288"><rect width="576" height="288" fill="black"/><path fill="white" fill-rule="evenodd" d="${d}"/></svg>\n`);
  }
  const sizes=steps.map(s=>s.payload.length).sort((a,b)=>a-b);
  console.log(`[vectors] ${count} frames; payload mean ${Math.round(sizes.reduce((a,b)=>a+b,0)/Math.max(1,count))} B, p95 ${sizes[Math.floor(count*0.95)]??0} B, max ${sizes.at(-1)??0} B`);
  console.log(`[vectors] source-width/frame-count: ${[...histogram].map(([w,n])=>`${w}/${n}`).join(", ")}`);
  steps.push({name:"release",payload:Uint8Array.from([18,2]),waitMs:0});
  return steps;
}
