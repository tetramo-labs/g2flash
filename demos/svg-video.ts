import {mkdir} from "node:fs/promises";
import {path, scene, svgPath, u16} from "./vector-protocol";
import type {VectorStep} from "./vector-cases";

export interface SvgVideo {
  version: 1;
  fps: number;
  width: number;
  height: number;
  source: string;
  frames: Array<{paths: Array<{d: string; x: number; y: number; edges: number}>; probes?: [number, number, number][]}>;
}

/** Normalize the known Potrace SVG output to firmware coordinates. */
export function potracePath(xml: string): {d: string; commands: number[]} {
  const match = xml.match(/<g\s+transform="translate\(([-\d.]+),([-\d.]+)\)\s+scale\(([-\d.]+),([-\d.]+)\)"/);
  if (!match) throw new Error("Unexpected Potrace transform");
  const [tx, ty, sx, sy] = match.slice(1).map(Number);
  const paths = [...xml.matchAll(/<path\s+d="([^"]*)"/g)].map(m => m[1]);
  const commands = svgPath(paths.join(" ") || "M0 0Z");
  const parts: string[] = [];
  for (let i = 0; i < commands.length;) {
    const op = commands[i++];
    parts.push(["M", "L", "Q", "C", "Z"][op]);
    const pairs = op === 4 ? 0 : op === 3 ? 3 : op === 2 ? 2 : 1;
    for (let j = 0; j < pairs; j++, i += 4) {
      const x = (commands[i] | commands[i + 1] << 8) << 16 >> 16;
      const y = (commands[i + 2] | commands[i + 3] << 8) << 16 >> 16;
      const nx = Math.round(x * sx + tx * 16), ny = Math.round(y * sy + ty * 16);
      if (nx < -32768 || nx > 32767 || ny < -32768 || ny > 32767) throw new Error("Transformed path exceeds Q4 coordinates");
      commands.splice(i, 4, ...u16(nx), ...u16(ny));
      parts.push(`${nx / 16} ${ny / 16}`);
    }
  }
  return {d: parts.join(" "), commands};
}

export function svgVideoSteps(video: SvgVideo, limit = video.frames.length): VectorStep[] {
  if (video.version !== 1 || !Number.isFinite(video.fps) || video.fps < 1 || video.fps > 30 ||
      !Number.isInteger(video.width) || video.width < 1 || video.width > 640 ||
      !Number.isInteger(video.height) || video.height < 1 || video.height > 480 || !video.frames.length)
    throw new Error("Invalid SVG video dimensions, frame rate or version");
  const x = Math.floor((640 - video.width) / 2), y = Math.floor((480 - video.height) / 2);
  const steps = video.frames.slice(0, limit).map((frame, i): VectorStep => {
    if (frame.paths.length > 128 || frame.paths.some(p => !Number.isInteger(p.edges) || p.edges < 0 || p.edges > 512) ||
        frame.paths.reduce((n, p) => n + p.edges, 0) > 4096) throw new Error(`Frame ${i} exceeds the path budget`);
    // CLEAR and all new tiles commit in one scene transaction, including blank
    // frames and frames that now need fewer tiles than their predecessor.
    const payload = scene(frame.paths.map((p, slot) => path(slot, svgPath(p.d), "evenodd", x + p.x, y + p.y)), true);
    if (payload.length > 65535) throw new Error(`Frame ${i} exceeds the transfer size`);
    return {name: `svg-video-${i}`, payload, waitMs: 1000 / video.fps, probes: frame.probes,
      snapshot: [30, 60, 120, 180, 240, 299].includes(i)};
  });
  steps.push({name: "release", payload: Uint8Array.from([38, 2]), waitMs: 0});
  return steps;
}

export async function loadSvgVideo(file: string, limit: number, svgOut?: string): Promise<VectorStep[]> {
  const video: SvgVideo = await Bun.file(file).json();
  const steps = svgVideoSteps(video, limit);
  if (svgOut) {
    await mkdir(svgOut, {recursive: true});
    for (let i = 0; i < steps.length - 1; i++) {
      const paths = video.frames[i].paths.map(p => `<path fill="white" fill-rule="evenodd" transform="translate(${p.x} ${p.y})" d="${p.d}"/>`).join("");
      await Bun.write(`${svgOut}/${String(i).padStart(4, "0")}.svg`, `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${video.width} ${video.height}"><rect width="100%" height="100%" fill="black"/>${paths}</svg>\n`);
    }
  }
  const sizes = steps.slice(0, -1).map(s => s.payload.length).sort((a, b) => a - b);
  console.log(`[svg video] ${sizes.length} frames, ${video.width}×${video.height}, ${video.fps} fps; mean ${Math.round(sizes.reduce((a,b)=>a+b,0)/sizes.length)} B, p95 ${sizes[Math.floor(sizes.length*.95)]} B, max ${sizes.at(-1)} B`);
  return steps;
}
