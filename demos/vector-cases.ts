import {finish, freeze, glide, path, rotate, scene, shape, svgPath, tween, type SlotDef} from "./vector-protocol";
import {control, hide} from "./object-cache";

export interface VectorStep {
  name: string;
  payload: Uint8Array;
  waitMs: number;
  probes?: [number, number, number][];
  snapshot?: boolean;
}
/**
 * Every step is one message on the object cache. A step that defines slots
 * SHOWs them (previous slots leave the list but stay cached); a step with
 * only ops KEEPs the list. The first step resets both caches.
 */
export function vectorCases(): VectorStep[] {
  const steps: VectorStep[] = [{name:"reset",payload:control.reset(),waitMs:0}];
  const add = (name: string, defs: SlotDef[], ops: number[][] = [], waitMs = 700, probes?: VectorStep["probes"], keepRefs?: SlotDef[]) =>
    steps.push({name, payload: defs.length || keepRefs ? scene(defs, {ops, refs: keepRefs}) : scene([], {ops, keep: true}), waitMs, probes, snapshot:true});
  const ring = svgPath("M80 80H240V240H80Z M120 120H200V200H120Z");
  add("fill-evenodd-hole", [path(0,ring,"evenodd")],[],700,[[90,90,15],[160,160,0]]);
  add("fill-nonzero-same-winding", [path(0,ring)],[],700,[[160,160,15]]);
  add("fill-nonzero-opposite-winding", [path(0,svgPath("M80 80H240V240H80Z M120 120V200H200V120Z"))],[],700,[[160,160,0]]);
  add("fill-quadratic-cubic", [path(0,svgPath("M100 230C100 30 300 30 300 230Q200 350 100 230Z"))],[],700,[[200,170,15],[200,30,0]]);
  const box = shape(0,3,[100,100,80,40]);
  add("rotation-initial", [box],[],500,[[105,120,15],[140,85,0]]);
  add("rotation-quarter-turn", [], [rotate(0,90,140,120,700)],800,[[105,120,0],[140,85,15]]);
  add("rotation-full-turn", [], [rotate(0,450,140,120,1800)],1900,[[105,120,0],[140,85,15]]);
  add("rotation-negative-fraction", [], [rotate(0,-37.5,140,120,900)],1000);
  add("rotation-retarget-start", [], [rotate(0,720,140,120,3000)],650);
  add("rotation-retarget-and-move", [], [glide(0,220,100,40),rotate(0,0,140,120,1200)],1500,[[330,230,15]]);
  add("rotation-freeze-start", [], [rotate(0,360,360,220,3000)],400);
  add("rotation-freeze", [], [freeze(0)],500);
  add("rotation-finish-start", [], [rotate(0,0,360,220,3000)],300);
  add("rotation-finish", [], [finish(0)],600,[[330,230,15]]);
  const rect = path(0,svgPath("M0 0H60V30H0Z"),"nonzero",100,100);
  add("path-tween-initial", [rect],[],500);
  add("path-move-scale-color-rotate", [], [tween(0,0x107,30,[0,0,255,255],[300,180,512,9]), rotate(0,90,300,180,990)],1100,[[270,220,9],[330,190,0]]);
  add("path-atomic-replacement", [path(0,svgPath("M80 80H240V240H80Z M120 120H200V200H120Z"),"evenodd")],[],700,[[160,160,0],[90,90,15]]);
  steps.push({name:"path-hide",payload:hide(),waitMs:400,probes:[[90,90,0]],snapshot:true});
  const params: Record<number, number[]> = {
    1:[0,0,75,60],2:[0,0,80,60,15],3:[0,0,80,60,15],4:[40,30,28],5:[40,30,28],
    6:[0,60,40,0,80,60],7:[0,60,40,0,80,60],8:[0,0,70,10,80,60,10,70],9:[0,0,70,10,80,60,10,70],
    10:[0,60,40,-30,80,60],11:[0,60,10,-20,70,-20,80,60],12:[40,30,30,0,260],13:[40,30,30,0,260],
  };
  const geometries = Object.entries(params).map(([key,p]) => {
    const type=Number(key), col=(type-1)%5,row=Math.floor((type-1)/5),ox=40+col*120,oy=80+row*120;
    const count=(type===1?2:([6,7,10].includes(type)?3:([8,9,11].includes(type)?4:1)));
    const v=[...p];for(let i=0;i<count;i++){v[2*i]+=ox;v[2*i+1]+=oy;}
    return {type,v,px:ox+40,py:oy+30};
  });
  add("all-geometric-shapes-initial", geometries.map(g=>shape(19+g.type,g.type,g.v,15,3)),[],700);
  add("all-geometric-shapes-rotate-halfway", [], geometries.map(g=>rotate(19+g.type,360,g.px,g.py,2500)),900);
  steps.push({name:"all-geometric-shapes-rotate-finish",payload:scene([],{ops:[],keep:true}),waitMs:1800,snapshot:true});
  /* the cached path from before is still resident: reopening it is one SHOW without a definition */
  add("path-reopen-from-cache", [], [], 700, [[90,90,15],[160,160,0]], [path(0,svgPath("M80 80H240V240H80Z M120 120H200V200H120Z"),"evenodd")]);
  steps.push({name:"release",payload:hide(),waitMs:0});
  return steps;
}
