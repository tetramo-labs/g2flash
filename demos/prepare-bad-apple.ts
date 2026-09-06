#!/usr/bin/env bun
/** Convert a downloaded video into smooth paths, checked by the firmware C compiler. */
import {mkdir} from "node:fs/promises";
import {resolve} from "node:path";
import {potracePath, type SvgVideo} from "./svg-video";

const args = process.argv.slice(2);
const option = (name: string, fallback: string) => {
  const i = args.indexOf(name); if (i < 0) return fallback;
  if (!args[i + 1] || args[i + 1].startsWith("--")) throw new Error(`Missing ${name} value`);
  return args[i + 1];
};
if (args.includes("--help")) {
  console.log("bun prepare-bad-apple.ts [--input VIDEO] [--out DIR] [--frames 300] [--fps 10] [--potrace PATH]\nNeeds ffmpeg, cc and Potrace. Downloads are separate; no BLE or firmware flashing.");
  process.exit(0);
}
const input = resolve(option("--input", ".cache/bad-apple/source.mp4"));
const out = resolve(option("--out", ".cache/bad-apple/smooth"));
const frames = Number(option("--frames", "300")), fps = Number(option("--fps", "10"));
if (!Number.isInteger(frames) || frames < 1 || frames > 10000 || !Number.isFinite(fps) || fps < 1 || fps > 30) throw new Error("Invalid frame count or fps");
const localPotrace = new URL(".cache/potrace/bin/potrace", import.meta.url).pathname;
const potrace = option("--potrace", await Bun.file(localPotrace).exists() ? localPotrace : "potrace");
const width = 576, height = 432;
await mkdir(out, {recursive: true});
async function run(cmd: string[]) {
  const proc = Bun.spawn(cmd, {stdout: "inherit", stderr: "inherit"});
  if (await proc.exited) throw new Error(`${cmd[0]} failed`);
}
const check = `${out}/path-check`;
const root = new URL("../", import.meta.url).pathname;
await run(["cc", "-O1", "-Wno-unused-function", `-I${root}patches`, "-o", check, `${root}patches/host/vector_path_check.c`]);
await run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", input, "-vf", `fps=${fps},scale=${width}:${height}:flags=lanczos,format=gray`,
  "-frames:v", String(frames), "-pix_fmt", "gray", "-f", "rawvideo", "-y", `${out}/frames.gray`]);
const raw = new Uint8Array(await Bun.file(`${out}/frames.gray`).arrayBuffer());
const count = Math.min(frames, Math.floor(raw.length / (width * height)));
if (!count) throw new Error("No video frames decoded");
const source = option("--source-url", args.includes("--input") ? input : "https://www.youtube.com/watch?v=i41KoE0iMYU");
const video: SvgVideo = {version: 1, fps, width, height, source, frames: []};
let maxEdges = 0, maxPaths = 0;
for (let f = 0; f < count; f++) {
  const pixels = raw.subarray(f * width * height, (f + 1) * width * height);
  const paths: SvgVideo["frames"][number]["paths"] = [];
  function trace(x: number, y: number, w: number, h: number, depth = 0): void {
    const header = new TextEncoder().encode(`P5\n${w} ${h}\n255\n`);
    const pgm = new Uint8Array(header.length + w * h); pgm.set(header);
    for (let row = 0; row < h; row++) pgm.set(pixels.subarray((y + row) * width + x, (y + row) * width + x + w), header.length + row * w);
    const traced = Bun.spawnSync([potrace, "--svg", "--flat", "--invert", "--unit", "2", "--opttolerance", "0.4", "--turdsize", "2", "-o", "-"], {stdin: pgm});
    if (traced.exitCode) throw new Error(`Potrace failed: ${traced.stderr}`);
    const {d, commands} = potracePath(traced.stdout.toString());
    const checked = Bun.spawnSync([check], {stdin: Uint8Array.from(commands)});
    if (checked.exitCode === 0) {
      const edges = Number(checked.stdout.toString().trim());
      if (!Number.isInteger(edges) || edges < 0 || edges > 512) throw new Error("Invalid compiler result");
      if (edges) paths.push({d, x, y, edges});
    } else if (checked.exitCode === 1 && depth < 8 && w > 8 && h > 8) {
      if (w >= h) { const half = Math.floor(w / 2); trace(x, y, half, h, depth + 1); trace(x + half, y, w - half, h, depth + 1); }
      else { const half = Math.floor(h / 2); trace(x, y, w, half, depth + 1); trace(x, y + half, w, h - half, depth + 1); }
    } else throw new Error(`Frame ${f} cannot fit the firmware path compiler`);
  }
  trace(0, 0, width, height);
  const edges = paths.reduce((n, p) => n + p.edges, 0);
  if (paths.length > 128 || edges > 4096) throw new Error(`Frame ${f} exceeds the scene budget (${edges} edges)`);
  // Source probes stay well inside solid regions; smooth contours deliberately
  // differ from the source at antialiased edges and tiny removed speckles.
  const probes: [number, number, number][] = [];
  let seed = f + 1;
  for (let attempt = 0; attempt < 1000 && probes.length < 24; attempt++) {
    seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
    const x = 4 + seed % (width - 8); seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
    const y = 4 + seed % (height - 8), white = pixels[y * width + x] >= 128;
    let solid = true;
    for (let yy = y - 4; yy <= y + 4 && solid; yy++) for (let xx = x - 4; xx <= x + 4; xx++)
      if (white ? pixels[yy * width + xx] < 240 : pixels[yy * width + xx] > 15) { solid = false; break; }
    if (solid) probes.push([32 + x, 24 + y, white ? 15 : 0]);
  }
  video.frames.push({paths, probes}); maxEdges = Math.max(maxEdges, edges); maxPaths = Math.max(maxPaths, paths.length);
  if (f % 25 === 0 || f === count - 1) console.log(`traced ${f + 1}/${count}: ${paths.length} paths, ${edges} compiled edges`);
}
await Bun.write(`${out}/video.json`, JSON.stringify(video));
console.log(`Prepared ${count} smooth frames at ${width}×${height}; max ${maxPaths} paths / ${maxEdges} edges.\n${out}/video.json`);
