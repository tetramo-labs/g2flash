#!/usr/bin/env bun
// The glassly example-miniapp "shapes" test suite, run straight against the
// glasses over BLE. Every case is the same data as the miniapp's
// src/shared/shapesSuite.ts; what the phone does between render() and the
// radio (anchor resolution, validate / clamp / budget, frame diffing, the
// mode-17 retained-scene encoder from G2CfwScene.swift) is ported below, so
// the verdicts mean the same thing here as in the tester page.
//
// What differs from the phone: default-font text is not wrapped or measured
// (the CFW draws it inline, one slot per line, and clips), so a truncated
// text box never marks a frame degraded here. Images are not supported.
//
// Pacing: a step's frame is sent `holdMs` after the previous step's frame was
// sent (never before that frame was acked), so each frame is on the glasses
// for about its hold and a later frame never waits for an earlier frame's
// transitions to finish — captions scroll while they glide, a readout ticks
// beside a tweening gauge, a second glide composes with the first. Only a
// case's last frame waits for its transitions before the hold and blank.
//
//     bun shapes-suite.ts                    # every case
//     bun shapes-suite.ts rect animation     # by group
//     bun shapes-suite.ts anim-glide         # by id
//     bun shapes-suite.ts --list             # ids and groups, no connection
//     bun shapes-suite.ts --self-test        # encoder vs the phone's unit-test vectors
//     bun shapes-suite.ts --dump FILE        # write every payload + wait for the host replay
//     G2_TRACE=1 bun shapes-suite.ts         # per-frame op counts and timings
//     G2_DRY_RUN=1 bun shapes-suite.ts       # host pipeline only, no glasses
//     G2_HOLD_SCALE=0.5 G2_OUT=results.json bun shapes-suite.ts
//
// Needs the glassly-cfw firmware (capability tokens scene17 shapes16 anim18).

import {
  G2Session,
  buildCreateStartUpPageContainer,
  buildImageContainers,
  buildImageRawData,
  planImageFragments,
  querySettings,
  queryCapabilities,
  hasFeature,
  type ImageContainerSpec,
} from "g2-kit/ble";
import { startHeartbeat } from "g2-kit/ui";

// ============================================================================
// Render API types (the subset of @glassly/miniapp the suite uses)
// ============================================================================

type Anchor = "top-left" | "top-right" | "bottom-left" | "bottom-right";
interface Box { x: number; y: number; w: number; h: number; anchor?: Anchor }
interface Point { x: number; y: number }
type Easing = "linear" | "ease" | "ease-in" | "ease-out" | "ease-in-out";
interface Transition { durationMs: number; easing?: Easing }
interface Style {
  border?: number;
  radius?: number;
  fill?: boolean;
  /** Gray level 0..15 (15 = brightest). Default 15. */
  color?: number;
  /** Stroke width for open strokes (line, bezier, arc). */
  width?: number;
  overflow?: "clip" | "ellipsis";
  font?: "default" | "mono" | "builtin";
}
type ElementType = "text" | "image" | "rect" | "circle" | "arc" | "pie" | "line" | "triangle" | "quad" | "bezier";
interface RenderElement {
  type: ElementType;
  id?: string;
  box?: Box;
  text?: string;
  data?: string;
  points?: Point[];
  startDeg?: number;
  endDeg?: number;
  style?: Style;
  transition?: Transition;
}
interface RenderResult {
  status: "displayed" | "blocked";
  degraded?: boolean;
  dropped?: string[];
  presented?: boolean;
  reason?: string;
}

// ============================================================================
// Suite data — src/shared/shapesSuite.ts, verbatim where the types allow
// ============================================================================

interface ShapesCanvas {
  width: number;
  height: number;
  shapes: string[];
  animation: boolean;
  maxTextElements: number;
}
interface ShapesStep { elements: RenderElement[]; holdMs: number }
interface ShapesExpectation { status: "displayed" | "blocked"; degraded: boolean; dropped: string[] }
type ShapesGroup = "rect" | "circle" | "arc" | "line" | "polygon" | "bezier" | "scene" | "limits" | "animation" | "text" | "stages";
interface ShapesCase {
  id: string;
  group: ShapesGroup;
  title: string;
  description: string;
  steps: (canvas: ShapesCanvas) => ShapesStep[];
  expect: (canvas: ShapesCanvas) => ShapesExpectation;
  blankAfter?: boolean;
}

const SHAPE_TYPES = ["rect", "circle", "arc", "pie", "line", "triangle", "quad", "bezier"] as const;
const EASE: Transition = { durationMs: 400, easing: "ease-in-out" };
const HOLD = 700;
const ok: ShapesExpectation = { status: "displayed", degraded: false, dropped: [] };

function unsupported(canvas: ShapesCanvas, ids: Array<[id: string, type: string]>): ShapesExpectation {
  const dropped = ids.filter(([, type]) => type !== "rect" && !canvas.shapes.includes(type)).map(([id]) => id);
  return { status: "displayed", degraded: dropped.length > 0, dropped };
}
function single(type: string, id: string): (canvas: ShapesCanvas) => ShapesExpectation {
  return (canvas) => unsupported(canvas, [[id, type]]);
}
function frame(canvas: ShapesCanvas, ...elements: RenderElement[]): ShapesStep[] {
  return [{ elements: [border(canvas), ...elements], holdMs: HOLD }];
}
function border(canvas: ShapesCanvas): RenderElement {
  return { type: "rect", id: "suite-border", box: { x: 0, y: 0, w: canvas.width, h: canvas.height }, style: { border: 1 } };
}
function center(canvas: ShapesCanvas, w: number, h: number): Box {
  return { x: Math.floor((canvas.width - w) / 2), y: Math.floor((canvas.height - h) / 2), w, h };
}

const SHAPE_CASES: ShapesCase[] = [
  // ── rect ──────────────────────────────────────────────────────────────
  {
    id: "rect-outline",
    group: "rect",
    title: "rect outline + radius",
    description: "A 2 px rounded outline in the middle of the screen.",
    steps: (c) => frame(c, { type: "rect", id: "r", box: center(c, 200, 100), style: { border: 2, radius: 12 } }),
    expect: () => ok,
  },
  {
    id: "rect-fill",
    group: "rect",
    title: "rect fill + border",
    description: "A solid mid-gray block with a bright 3 px border.",
    steps: (c) => frame(c, { type: "rect", id: "r", box: center(c, 200, 100), style: { fill: true, color: 8, border: 3, radius: 8 } }),
    expect: () => ok,
  },
  {
    id: "rect-gray-ramp",
    group: "rect",
    title: "16 gray levels",
    description: "Sixteen filled bars from black (0) to brightest (15), left to right.",
    steps: (c) => {
      const w = Math.floor((c.width - 32) / 16);
      const bars: RenderElement[] = Array.from({ length: 16 }, (_, i) => ({
        type: "rect",
        id: `bar-${i}`,
        box: { x: 16 + i * w, y: 40, w: w - 2, h: c.height - 80 },
        style: { fill: true, color: i },
      }));
      return frame(c, ...bars);
    },
    expect: (c) => {
      const ids = ["suite-border", ...Array.from({ length: 16 }, (_, i) => `bar-${i}`)];
      const dropped = ids.slice(c.maxTextElements);
      return { status: "displayed", degraded: dropped.length > 0, dropped };
    },
  },

  // ── circle ────────────────────────────────────────────────────────────
  {
    id: "circle-outline",
    group: "circle",
    title: "circle outline",
    description: "A thin ring centered on screen.",
    steps: (c) => frame(c, { type: "circle", id: "c", box: center(c, 120, 120), style: { border: 1 } }),
    expect: single("circle", "c"),
  },
  {
    id: "circle-fill-ring",
    group: "circle",
    title: "disc + thick ring",
    description: "A filled disc on the left, an 8 px ring on the right.",
    steps: (c) =>
      frame(
        c,
        { type: "circle", id: "disc", box: { x: 60, y: center(c, 100, 100).y, w: 100, h: 100 }, style: { fill: true } },
        { type: "circle", id: "ring", box: { x: c.width - 160, y: center(c, 100, 100).y, w: 100, h: 100 }, style: { border: 8, color: 11 } },
      ),
    expect: (c) => unsupported(c, [["disc", "circle"], ["ring", "circle"]]),
  },
  {
    id: "circle-inscribed",
    group: "circle",
    title: "inscribed in a wide box",
    description: "The circle uses the shorter side: a disc centered in the outlined box, never an ellipse.",
    steps: (c) => {
      const box = center(c, 240, 80);
      return frame(c, { type: "rect", id: "box", box, style: { border: 1, color: 6 } }, { type: "circle", id: "c", box, style: { fill: true } });
    },
    expect: single("circle", "c"),
  },

  // ── arc + pie ─────────────────────────────────────────────────────────
  {
    id: "arc-quarter",
    group: "arc",
    title: "quarter arc",
    description: "A 4 px arc from the top (−90°) clockwise to the right (0°).",
    steps: (c) => frame(c, { type: "arc", id: "a", box: center(c, 140, 140), startDeg: -90, endDeg: 0, style: { width: 4 } }),
    expect: single("arc", "a"),
  },
  {
    id: "arc-gauge",
    group: "arc",
    title: "gauge arc",
    description: "A thick 270° arc open at the bottom, like a gauge dial.",
    steps: (c) => frame(c, { type: "arc", id: "a", box: center(c, 160, 160), startDeg: 135, endDeg: 405, style: { width: 10, color: 12 } }),
    expect: single("arc", "a"),
  },
  {
    id: "pie-sector",
    group: "arc",
    title: "pie sector",
    description: "A filled wedge from the top clockwise to the lower right (225° sweep).",
    steps: (c) => frame(c, { type: "pie", id: "p", box: center(c, 140, 140), startDeg: -90, endDeg: 135 }),
    expect: single("pie", "p"),
  },
  {
    id: "pie-full",
    group: "arc",
    title: "full pie",
    description: "A 360° pie equals a filled disc.",
    steps: (c) => frame(c, { type: "pie", id: "p", box: center(c, 100, 100), startDeg: 0, endDeg: 360, style: { color: 9 } }),
    expect: single("pie", "p"),
  },

  // ── line ──────────────────────────────────────────────────────────────
  {
    id: "line-hairline",
    group: "line",
    title: "hairline",
    description: "A 1 px diagonal from top-left to bottom-right of the screen.",
    steps: (c) => frame(c, { type: "line", id: "l", points: [{ x: 8, y: 8 }, { x: c.width - 9, y: c.height - 9 }] }),
    expect: single("line", "l"),
  },
  {
    id: "line-wide",
    group: "line",
    title: "wide strokes",
    description: "Three horizontal lines 2, 6 and 12 px thick.",
    steps: (c) =>
      frame(
        c,
        ...[2, 6, 12].map<RenderElement>((width, i) => ({
          type: "line",
          id: `l${width}`,
          points: [{ x: 40, y: 60 + i * 60 }, { x: c.width - 40, y: 60 + i * 60 }],
          style: { width },
        })),
      ),
    expect: (c) => unsupported(c, [["l2", "line"], ["l6", "line"], ["l12", "line"]]),
  },
  {
    id: "line-clipped",
    group: "line",
    title: "clipped by the device",
    description: "A line that runs off the right edge is drawn up to the edge and reported as degraded, not dropped.",
    steps: (c) => frame(c, { type: "line", id: "l", points: [{ x: 40, y: c.height - 40 }, { x: c.width + 200, y: 40 }], style: { width: 3 } }),
    expect: (c) => ({ ...unsupported(c, [["l", "line"]]), degraded: true }),
  },

  // ── polygons ──────────────────────────────────────────────────────────
  {
    id: "triangle",
    group: "polygon",
    title: "triangle outline + fill",
    description: "An outlined triangle on the left and a filled one on the right.",
    steps: (c) => {
      const y0 = c.height - 40;
      const y1 = 40;
      return frame(
        c,
        { type: "triangle", id: "t-out", points: [{ x: 40, y: y0 }, { x: 220, y: y0 }, { x: 130, y: y1 }], style: { border: 2 } },
        { type: "triangle", id: "t-fill", points: [{ x: c.width - 220, y: y0 }, { x: c.width - 40, y: y0 }, { x: c.width - 130, y: y1 }], style: { fill: true, color: 10 } },
      );
    },
    expect: (c) => unsupported(c, [["t-out", "triangle"], ["t-fill", "triangle"]]),
  },
  {
    id: "quad",
    group: "polygon",
    title: "quad outline + fill",
    description: "A skewed outlined quad on the left and a filled one on the right.",
    steps: (c) =>
      frame(
        c,
        { type: "quad", id: "q-out", points: [{ x: 60, y: 60 }, { x: 230, y: 80 }, { x: 210, y: c.height - 60 }, { x: 40, y: c.height - 80 }], style: { border: 2 } },
        { type: "quad", id: "q-fill", points: [{ x: c.width - 230, y: 60 }, { x: c.width - 60, y: 80 }, { x: c.width - 40, y: c.height - 60 }, { x: c.width - 210, y: c.height - 80 }], style: { fill: true } },
      ),
    expect: (c) => unsupported(c, [["q-out", "quad"], ["q-fill", "quad"]]),
  },

  // ── bezier ────────────────────────────────────────────────────────────
  {
    id: "bezier-quadratic",
    group: "bezier",
    title: "quadratic bezier",
    description: "A single arch pulled up by one control point.",
    steps: (c) =>
      frame(c, { type: "bezier", id: "b", points: [{ x: 40, y: c.height - 40 }, { x: Math.floor(c.width / 2), y: -40 }, { x: c.width - 40, y: c.height - 40 }], style: { width: 3 } }),
    expect: (c) => ({ ...unsupported(c, [["b", "bezier"]]), degraded: true }),
  },
  {
    id: "bezier-cubic",
    group: "bezier",
    title: "cubic bezier",
    description: "An S-curve from left to right.",
    steps: (c) =>
      frame(c, {
        type: "bezier",
        id: "b",
        points: [
          { x: 40, y: Math.floor(c.height / 2) },
          { x: Math.floor(c.width / 3), y: 20 },
          { x: Math.floor((2 * c.width) / 3), y: c.height - 20 },
          { x: c.width - 40, y: Math.floor(c.height / 2) },
        ],
        style: { width: 4, color: 13 },
      }),
    expect: single("bezier", "b"),
  },

  // ── scene composition ─────────────────────────────────────────────────
  {
    id: "scene-painters-order",
    group: "scene",
    title: "painter's order",
    description: "Filled rect, then text on top of it, then a bright disc over both: later elements paint over earlier ones.",
    steps: (c) =>
      frame(
        c,
        { type: "rect", id: "bg", box: center(c, 300, 140), style: { fill: true, color: 5 } },
        { type: "text", id: "label", box: center(c, 280, 40), text: "shapes on top" },
        { type: "circle", id: "dot", box: { x: center(c, 300, 140).x + 240, y: center(c, 300, 140).y + 20, w: 60, h: 60 }, style: { fill: true } },
      ),
    expect: single("circle", "dot"),
  },
  {
    id: "scene-anchored",
    group: "scene",
    title: "anchored boxes",
    description: "A circle flush in the bottom-right corner and a rect flush top-right, on every canvas size.",
    steps: (c) =>
      frame(
        c,
        { type: "circle", id: "br", box: { x: 8, y: 8, w: 60, h: 60, anchor: "bottom-right" }, style: { fill: true } },
        { type: "rect", id: "tr", box: { x: 8, y: 8, w: 120, h: 40, anchor: "top-right" }, style: { border: 2, radius: 6 } },
      ),
    expect: single("circle", "br"),
  },
  {
    id: "scene-clear",
    group: "scene",
    title: "render([]) clears",
    description: "The screen goes blank.",
    steps: () => [{ elements: [], holdMs: 200 }],
    expect: () => ok,
  },

  // ── limits + validation ───────────────────────────────────────────────
  {
    id: "limits-budget",
    group: "limits",
    title: "element budget",
    description: "More discs than the budget: the tail is dropped and reported, the rest draws.",
    steps: (c) => {
      // Small enough discs that budget + 4 of them fit on the canvas: the
      // budget, not the clamp, must be what drops the tail.
      const n = c.maxTextElements + 4;
      const pitch = 24;
      const cols = Math.max(1, Math.floor((c.width - 8) / pitch));
      const discs: RenderElement[] = Array.from({ length: n }, (_, i) => ({
        type: "circle",
        id: `d${i}`,
        box: { x: 8 + (i % cols) * pitch, y: 8 + Math.floor(i / cols) * pitch, w: 20, h: 20 },
        style: { fill: true, color: 4 + (i % 12) },
      }));
      return [{ elements: discs, holdMs: HOLD }];
    },
    expect: (c) => {
      const n = c.maxTextElements + 4;
      const ids = Array.from({ length: n }, (_, i) => `d${i}`);
      const dropped = c.shapes.includes("circle") ? ids.slice(c.maxTextElements) : ids;
      return { status: "displayed", degraded: true, dropped };
    },
  },
  {
    id: "limits-bad-points",
    group: "limits",
    title: "invalid point counts",
    description: "A 2-point triangle and a 5-point bezier are rejected; the valid line still draws.",
    steps: (c) =>
      frame(
        c,
        { type: "triangle", id: "bad-tri", points: [{ x: 10, y: 10 }, { x: 100, y: 10 }] },
        { type: "bezier", id: "bad-bez", points: [{ x: 10, y: 10 }, { x: 20, y: 20 }, { x: 30, y: 30 }, { x: 40, y: 40 }, { x: 50, y: 50 }] },
        { type: "line", id: "good", points: [{ x: 40, y: Math.floor(c.height / 2) }, { x: c.width - 40, y: Math.floor(c.height / 2) }], style: { width: 2 } },
      ),
    expect: (c) => {
      const dropped = ["bad-tri", "bad-bez", ...(c.shapes.includes("line") ? [] : ["good"])];
      return { status: "displayed", degraded: true, dropped };
    },
  },
  {
    id: "limits-unsupported",
    group: "limits",
    title: "capability gate",
    description: "Every shape type at once. Types the device lists draw; the others are dropped and reported, in order.",
    steps: (c) => {
      const y = Math.floor(c.height / 2);
      return frame(
        c,
        { type: "circle", id: "g-circle", box: { x: 20, y: y - 30, w: 60, h: 60 }, style: { fill: true } },
        { type: "arc", id: "g-arc", box: { x: 100, y: y - 30, w: 60, h: 60 }, startDeg: 0, endDeg: 270, style: { width: 4 } },
        { type: "pie", id: "g-pie", box: { x: 180, y: y - 30, w: 60, h: 60 }, startDeg: 0, endDeg: 120 },
        { type: "line", id: "g-line", points: [{ x: 260, y: y - 30 }, { x: 320, y: y + 30 }] },
        { type: "triangle", id: "g-tri", points: [{ x: 340, y: y + 30 }, { x: 400, y: y + 30 }, { x: 370, y: y - 30 }] },
        { type: "quad", id: "g-quad", points: [{ x: 420, y: y - 30 }, { x: 480, y: y - 30 }, { x: 470, y: y + 30 }, { x: 430, y: y + 30 }] },
        { type: "bezier", id: "g-bez", points: [{ x: 500, y: y + 30 }, { x: 530, y: y - 40 }, { x: 560, y: y + 30 }] },
      );
    },
    expect: (c) =>
      unsupported(c, [
        ["g-circle", "circle"],
        ["g-arc", "arc"],
        ["g-pie", "pie"],
        ["g-line", "line"],
        ["g-tri", "triangle"],
        ["g-quad", "quad"],
        ["g-bez", "bezier"],
      ]),
  },

  // ── animation ─────────────────────────────────────────────────────────
  {
    id: "anim-glide",
    group: "animation",
    title: "glide (move)",
    description: "A disc eases from the left edge to the right edge and back. Without animation it jumps.",
    steps: (c) => {
      const y = Math.floor(c.height / 2) - 25;
      const at = (x: number): RenderElement => ({ type: "circle", id: "ball", box: { x, y, w: 50, h: 50 }, style: { fill: true }, transition: EASE });
      return [
        { elements: [border(c), at(20)], holdMs: HOLD },
        { elements: [border(c), at(c.width - 70)], holdMs: HOLD },
        { elements: [border(c), at(20)], holdMs: HOLD },
      ];
    },
    expect: single("circle", "ball"),
  },
  {
    id: "anim-resize",
    group: "animation",
    title: "tween (resize)",
    description: "A progress bar grows from empty to full inside its track.",
    steps: (c) => {
      const track = center(c, c.width - 80, 24);
      const bar = (w: number): RenderElement[] => [
        border(c),
        { type: "rect", id: "track", box: track, style: { border: 1, radius: 12, color: 7 } },
        { type: "rect", id: "bar", box: { x: track.x, y: track.y, w: Math.max(2, w), h: 24 }, style: { fill: true, radius: 12 }, transition: { durationMs: 900, easing: "linear" } },
      ];
      return [
        { elements: bar(2), holdMs: 300 },
        { elements: bar(track.w), holdMs: 1200 },
      ];
    },
    expect: () => ok,
  },
  {
    id: "anim-arc-sweep",
    group: "animation",
    title: "tween (angles)",
    description: "A gauge arc sweeps from 0° to 270° and a pie sector opens with it.",
    steps: (c) => {
      const at = (end: number): RenderElement[] => [
        border(c),
        { type: "arc", id: "gauge", box: { x: 60, y: center(c, 140, 140).y, w: 140, h: 140 }, startDeg: 135, endDeg: 135 + end, style: { width: 10 }, transition: { durationMs: 800, easing: "ease-out" } },
        { type: "pie", id: "wedge", box: { x: c.width - 200, y: center(c, 140, 140).y, w: 140, h: 140 }, startDeg: -90, endDeg: -90 + end, style: { color: 10 }, transition: { durationMs: 800, easing: "ease-out" } },
      ];
      return [
        { elements: at(1), holdMs: 300 },
        { elements: at(270), holdMs: 1100 },
      ];
    },
    expect: (c) => unsupported(c, [["gauge", "arc"], ["wedge", "pie"]]),
  },
  {
    id: "anim-color-stroke",
    group: "animation",
    title: "tween (color + stroke)",
    description: "A ring brightens from dim to full and thickens from 1 px to 12 px.",
    steps: (c) => {
      const ring = (color: number, stroke: number): RenderElement => ({ type: "circle", id: "ring", box: center(c, 140, 140), style: { border: stroke, color }, transition: { durationMs: 700 } });
      return [
        { elements: [border(c), ring(2, 1)], holdMs: 300 },
        { elements: [border(c), ring(15, 12)], holdMs: 1000 },
      ];
    },
    expect: single("circle", "ring"),
  },
  {
    id: "anim-points",
    group: "animation",
    title: "tween (points)",
    description: "A triangle morphs as its apex slides from left to right.",
    steps: (c) => {
      const tri = (apexX: number): RenderElement[] => [
        border(c),
        { type: "triangle", id: "tri", points: [{ x: 60, y: c.height - 40 }, { x: c.width - 60, y: c.height - 40 }, { x: apexX, y: 40 }], style: { fill: true, color: 12 }, transition: { durationMs: 600, easing: "ease-in-out" } },
      ];
      return [
        { elements: tri(80), holdMs: 300 },
        { elements: tri(c.width - 80), holdMs: 900 },
        { elements: tri(80), holdMs: 900 },
      ];
    },
    expect: single("triangle", "tri"),
  },
  {
    id: "anim-compose",
    group: "animation",
    title: "compose in-flight glides",
    description: "A second move lands while the first is still running; the disc ends at the final target without a snap.",
    steps: (c) => {
      const at = (x: number, y: number): RenderElement[] => [
        border(c),
        { type: "circle", id: "ball", box: { x, y, w: 40, h: 40 }, style: { fill: true }, transition: { durationMs: 800 } },
      ];
      return [
        { elements: at(20, 20), holdMs: 200 },
        { elements: at(c.width - 60, 20), holdMs: 250 },
        { elements: at(c.width - 60, c.height - 60), holdMs: 1000 },
      ];
    },
    expect: single("circle", "ball"),
  },
  {
    id: "anim-no-transition",
    group: "animation",
    title: "control: no transition",
    description: "The same move without a transition jumps instantly on every device.",
    steps: (c) => {
      const y = Math.floor(c.height / 2) - 25;
      const at = (x: number): RenderElement[] => [border(c), { type: "circle", id: "ball", box: { x, y, w: 50, h: 50 }, style: { fill: true } }];
      return [
        { elements: at(20), holdMs: HOLD },
        { elements: at(c.width - 70), holdMs: HOLD },
      ];
    },
    expect: single("circle", "ball"),
  },
  {
    id: "anim-content-switch",
    group: "animation",
    title: "content changes never animate",
    description: "Text with a transition changes wording: it switches immediately while a disc beside it glides.",
    steps: (c) => {
      const at = (x: number, label: string): RenderElement[] => [
        border(c),
        { type: "text", id: "t", box: { x: 20, y: 20, w: 300, h: 40 }, text: label, transition: EASE },
        { type: "circle", id: "ball", box: { x, y: 100, w: 50, h: 50 }, style: { fill: true }, transition: EASE },
      ];
      return [
        { elements: at(20, "before"), holdMs: HOLD },
        { elements: at(c.width - 70, "after"), holdMs: HOLD },
      ];
    },
    expect: single("circle", "ball"),
  },

  // ── text: fast updates + animation ──────────────────────────────────
  {
    id: "text-counter",
    group: "text",
    title: "counter at 10 fps",
    description: "A number counts 0 to 29 at ten updates per second; every frame should land without tearing.",
    steps: (c) =>
      Array.from({ length: 30 }, (_, i) => ({
        elements: [
          border(c),
          { type: "text" as const, id: "label", box: { x: 20, y: 20, w: 300, h: 30 }, text: "counter at 10 fps" },
          { type: "text" as const, id: "count", box: center(c, 200, 40), text: String(i) },
        ],
        holdMs: 100,
      })),
    expect: () => ok,
  },
  {
    id: "text-typewriter",
    group: "text",
    title: "typewriter",
    description: "A sentence appears one character at a time, 60 ms apart.",
    steps: (c) => {
      const sentence = "The glasses draw this text themselves.";
      return Array.from({ length: sentence.length }, (_, i) => ({
        elements: [border(c), { type: "text" as const, id: "typed", box: { x: 20, y: 40, w: c.width - 40, h: 60 }, text: sentence.slice(0, i + 1) }],
        holdMs: 60,
      }));
    },
    expect: () => ok,
  },
  {
    id: "text-slide",
    group: "text",
    title: "slide (glide)",
    description: "A label glides from the left edge to the right edge and back. Without animation it jumps.",
    steps: (c) => {
      const at = (x: number): RenderElement[] => [border(c), { type: "text", id: "slider", box: { x, y: 60, w: 180, h: 30 }, text: "sliding text", transition: EASE }];
      return [
        { elements: at(20), holdMs: HOLD },
        { elements: at(c.width - 200), holdMs: HOLD },
        { elements: at(20), holdMs: HOLD },
      ];
    },
    expect: () => ok,
  },
  {
    id: "text-captions",
    group: "text",
    title: "caption scroll",
    description: "New caption lines arrive at the bottom while older lines glide up and the oldest disappears.",
    steps: (c) => {
      const lines = ["first line arrives", "second line arrives", "third line arrives", "fourth line arrives", "fifth line arrives", "sixth line arrives"];
      const rows = 3;
      const pitch = 27;
      const top = c.height - 20 - rows * pitch;
      return lines.map((_, step) => {
        const visible = lines.slice(Math.max(0, step - rows + 1), step + 1);
        const first = step - visible.length + 1;
        return {
          elements: [
            border(c),
            ...visible.map((text, i): RenderElement => ({
              type: "text",
              id: `cap-${first + i}`,
              box: { x: 20, y: top + i * pitch, w: c.width - 40, h: pitch },
              text,
              transition: { durationMs: 250, easing: "ease-out" },
            })),
          ],
          holdMs: 500,
        };
      });
    },
    expect: () => ok,
  },
  {
    id: "text-fade",
    group: "text",
    title: "fade (color tween)",
    description: "A label fades in from black to full brightness, then back out.",
    steps: (c) => {
      const at = (color: number): RenderElement[] => [
        border(c),
        { type: "text", id: "fader", box: center(c, 240, 30), text: "fading in and out", style: { color }, transition: { durationMs: 600, easing: "linear" } },
      ];
      return [
        { elements: at(0), holdMs: 200 },
        { elements: at(15), holdMs: 900 },
        { elements: at(0), holdMs: 900 },
      ];
    },
    expect: () => ok,
  },
  {
    id: "text-clock",
    group: "text",
    title: "tenths clock",
    description: "A clock ticks in tenths of a second for three seconds next to a spinning arc.",
    steps: (c) =>
      Array.from({ length: 30 }, (_, i) => ({
        elements: [
          border(c),
          { type: "text" as const, id: "clock", box: { x: 20, y: 20, w: 200, h: 30 }, text: `00:${String(i / 10).padStart(4, "0")}` },
          { type: "arc" as const, id: "spinner", box: { x: c.width - 80, y: 20, w: 50, h: 50 }, startDeg: i * 36, endDeg: i * 36 + 270, style: { width: 4 }, transition: { durationMs: 100, easing: "linear" as const } },
        ],
        holdMs: 100,
      })),
    expect: single("arc", "spinner"),
  },
];

// ── stages: the g2flash shapes-demo, stage for stage, through render() ──

const STAGE_EASE = {
  linear: { durationMs: 660, easing: "linear" } as Transition,
  inOut: { durationMs: 1500, easing: "ease-in-out" } as Transition,
};

/** What the host drops from `elements` on this device: unsupported shapes, then text/rect past the budget, in element order. */
function budgeted(canvas: ShapesCanvas, elements: RenderElement[]): ShapesExpectation {
  const dropped: string[] = [];
  let pool = canvas.maxTextElements;
  for (const el of elements) {
    const id = el.id ?? "";
    if (el.type === "image") continue;
    if (el.type !== "text" && el.type !== "rect" && !canvas.shapes.includes(el.type)) {
      dropped.push(id);
      continue;
    }
    if (pool <= 0) {
      dropped.push(id);
      continue;
    }
    pool--;
  }
  return { status: "displayed", degraded: dropped.length > 0, dropped };
}

function stageLabel(canvas: ShapesCanvas, n: number, name: string): RenderElement[] {
  return [
    border(canvas),
    { type: "text", id: "stage-label", box: { x: 12, y: 6, w: canvas.width - 24, h: 24 }, text: `stage ${n}: ${name}`, style: { color: 7 } },
  ];
}

function stageShapes(c: ShapesCanvas): RenderElement[] {
  const s = Math.min(c.width / 576, c.height / 288);
  const px = (v: number) => Math.round(v * s);
  return [
    { type: "line", id: "s-line", points: [{ x: px(30), y: px(50) }, { x: px(540), y: px(50) }], style: { width: 3, color: 12 } },
    { type: "circle", id: "s-disc", box: { x: px(30), y: px(70), w: px(90), h: px(90) }, style: { fill: true } },
    { type: "circle", id: "s-ring", box: { x: px(140), y: px(70), w: px(90), h: px(90) }, style: { border: 5, color: 12 } },
    { type: "rect", id: "s-rect", box: { x: px(250), y: px(70), w: px(120), h: px(90) }, style: { fill: true, color: 10, radius: 14 } },
    { type: "triangle", id: "s-tri", points: [{ x: px(30), y: px(260) }, { x: px(120), y: px(260) }, { x: px(75), y: px(185) }], style: { fill: true } },
    { type: "quad", id: "s-quad", points: [{ x: px(150), y: px(185) }, { x: px(260), y: px(192) }, { x: px(252), y: px(262) }, { x: px(158), y: px(255) }], style: { border: 2 } },
    { type: "bezier", id: "s-bez", points: [{ x: px(290), y: px(250) }, { x: px(330), y: px(180) }, { x: px(380), y: px(280) }, { x: px(440), y: px(230) }], style: { width: 3 } },
    { type: "arc", id: "s-arc", box: { x: px(400), y: px(70), w: px(100), h: px(100) }, startDeg: 180, endDeg: 360, style: { width: 4 } },
    { type: "pie", id: "s-pie", box: { x: px(425), y: px(95), w: px(50), h: px(50) }, startDeg: -90, endDeg: 180, style: { color: 9 } },
  ];
}

const STAGE_SHAPE_IDS: Array<[string, string]> = [
  ["s-line", "line"],
  ["s-disc", "circle"],
  ["s-ring", "circle"],
  ["s-rect", "rect"],
  ["s-tri", "triangle"],
  ["s-quad", "quad"],
  ["s-bez", "bezier"],
  ["s-arc", "arc"],
  ["s-pie", "pie"],
];

const STAGE_CASES: ShapesCase[] = [
  {
    id: "stage-1-shapes-card",
    group: "stages",
    title: "1 · shapes card",
    description: "Every shape type on one screen: line, disc, ring, rounded fill, triangle, quad, bezier, arc, pie.",
    steps: (c) => [{ elements: [...stageLabel(c, 1, "every shape at once"), ...stageShapes(c)], holdMs: 2500 }],
    expect: (c) => unsupported(c, STAGE_SHAPE_IDS),
    blankAfter: true,
  },
  {
    id: "stage-2-text-lines",
    group: "stages",
    title: "2 · text lines, gray levels, clipping",
    description: "Three sentences, five gray levels, a line clipped by a narrow box, and non-ASCII text.",
    steps: (c) => {
      const lines = ["The glasses draw this text themselves.", "Each line is one slot with its bytes inline.", "No bitmaps and no re-sends."];
      const w = c.width - 40;
      return [
        {
          elements: [
            ...stageLabel(c, 2, "text lines"),
            ...lines.map((text, i): RenderElement => ({ type: "text", id: `line-${i}`, box: { x: 20, y: 34 + i * 26, w, h: 26 }, text })),
            ...[15, 12, 9, 6, 3].map((color, i): RenderElement => ({ type: "text", id: `gray-${color}`, box: { x: 20, y: 114 + i * 20, w: 200, h: 20 }, text: `gray level ${color}`, style: { color } })),
            { type: "text", id: "clipped", box: { x: 240, y: 116, w: Math.min(220, c.width - 260), h: 24 }, text: "clipped at the box edge: the rest of this sentence is cut", style: { overflow: "clip" } },
            { type: "rect", id: "clip-box", box: { x: 238, y: 114, w: Math.min(224, c.width - 256), h: 28 }, style: { border: 1, color: 5 } },
            { type: "text", id: "utf8", box: { x: 240, y: 150, w: c.width - 260, h: 24 }, text: "UTF-8: café, naïve, 日本語, ∑" },
          ],
          holdMs: 3500,
        },
      ];
    },
    expect: (c) => budgeted(c, STAGE_CASES[1].steps(c)[0].elements),
    blankAfter: true,
  },
  {
    id: "stage-3-fast-text",
    group: "stages",
    title: "3 · fast text updates",
    description: "A counter, a typewriter and a tenths clock, one render per update with no hold; the rate shows how fast a small update lands.",
    steps: (c) => {
      const row = (id: string, y: number, text: string): RenderElement => ({ type: "text", id, box: { x: 200, y, w: c.width - 220, h: 26 }, text });
      const base = (count: string, typed: string, clock: string): RenderElement[] => [
        ...stageLabel(c, 3, "fast text updates"),
        { type: "text", id: "l-count", box: { x: 20, y: 50, w: 170, h: 26 }, text: "counter", style: { color: 9 } },
        row("count", 50, count),
        { type: "text", id: "l-typed", box: { x: 20, y: 90, w: 170, h: 26 }, text: "typewriter", style: { color: 9 } },
        row("typed", 90, typed),
        { type: "text", id: "l-clock", box: { x: 20, y: 130, w: 170, h: 26 }, text: "tenths clock", style: { color: 9 } },
        row("clock", 130, clock),
      ];
      const sentence = "typed one character per render";
      const steps: ShapesStep[] = [];
      for (let i = 0; i < 40; i++) steps.push({ elements: base(String(i + 1), " ", "00.0"), holdMs: 0 });
      for (let i = 0; i < sentence.length; i++) steps.push({ elements: base("40", sentence.slice(0, i + 1), "00.0"), holdMs: 0 });
      for (let i = 0; i < 40; i++) steps.push({ elements: base("40", sentence, `${String(Math.floor(i / 10)).padStart(2, "0")}.${i % 10}`), holdMs: 0 });
      steps[steps.length - 1].holdMs = 1500;
      return steps;
    },
    expect: (c) => {
      const steps = STAGE_CASES[2].steps(c);
      return budgeted(c, steps[steps.length - 1].elements);
    },
    blankAfter: true,
  },
  {
    id: "stage-4-text-animation",
    group: "stages",
    title: "4 · text animation",
    description: "Four lines slide across with staggered eases, captions scroll up as new lines arrive, then a line fades in, drifts and fades out.",
    steps: (c) => {
      const steps: ShapesStep[] = [];
      const slide = ["slide across", "with staggered", "eases", "on each line"];
      const eases: Easing[] = ["ease-out", "ease-in-out", "ease-in", "linear"];
      const slideAt = (x: number): RenderElement[] => [
        ...stageLabel(c, 4, "text animation: slide"),
        ...slide.map((text, i): RenderElement => ({ type: "text", id: `slide-${i}`, box: { x, y: 40 + i * 26, w: 200, h: 26 }, text, transition: { durationMs: 700 + i * 250, easing: eases[i] } })),
      ];
      steps.push({ elements: slideAt(20), holdMs: 300 }, { elements: slideAt(c.width - 220), holdMs: 2000 }, { elements: slideAt(20), holdMs: 1500 });

      const captions = ["captions arrive at the bottom", "older lines glide up", "and the oldest one leaves", "every step is one render", "at any rate the link allows", "until the transcript ends"];
      const rows = 3;
      const pitch = 26;
      const top = c.height - 12 - rows * pitch;
      captions.forEach((_, step) => {
        const visible = captions.slice(Math.max(0, step - rows + 1), step + 1);
        const first = step - visible.length + 1;
        steps.push({
          elements: [
            ...stageLabel(c, 4, "text animation: captions"),
            { type: "rect", id: "cap-box", box: { x: 12, y: top - 6, w: c.width - 24, h: rows * pitch + 10 }, style: { border: 1, color: 5, radius: 6 } },
            ...visible.map((text, i): RenderElement => ({ type: "text", id: `cap-${first + i}`, box: { x: 20, y: top + i * pitch, w: c.width - 40, h: pitch }, text, transition: { durationMs: 330, easing: "ease-out" } })),
          ],
          holdMs: 550,
        });
      });

      const fade = (x: number, color: number, hold: number): ShapesStep => ({
        elements: [
          ...stageLabel(c, 4, "text animation: fade"),
          { type: "text", id: "fader", box: { x, y: Math.round(c.height / 2), w: 300, h: 26 }, text: "fading in, drifting, fading out", style: { color }, transition: { durationMs: 1000, easing: "linear" } },
        ],
        holdMs: hold,
      });
      steps.push(fade(40, 0, 200), fade(40, 15, 1300), fade(c.width - 340, 15, 1500), fade(c.width - 340, 0, 1300));
      return steps;
    },
    expect: () => ok,
    blankAfter: true,
  },
  {
    id: "stage-5-shape-animation",
    group: "stages",
    title: "5 · shape animation with a text readout",
    description: "A disc glides, a bar and an arc gauge tween, and a percentage readout ticks alongside; then everything runs back and forth.",
    steps: (c) => {
      const trackW = c.width - 80;
      const frame = (ball: number, fill: number, gaugeEnd: number, readout: string, hold: number, transition: Transition): ShapesStep => ({
        elements: [
          ...stageLabel(c, 5, "shape animation"),
          { type: "circle", id: "ball", box: { x: ball, y: Math.round(c.height / 2) - 24, w: 48, h: 48 }, style: { fill: true }, transition },
          { type: "rect", id: "track", box: { x: 40, y: c.height - 40, w: trackW, h: 16 }, style: { border: 1, radius: 8, color: 6 } },
          { type: "rect", id: "bar", box: { x: 40, y: c.height - 40, w: Math.max(2, fill), h: 16 }, style: { fill: true, radius: 8, color: 12 }, transition },
          { type: "arc", id: "gauge", box: { x: c.width - 150, y: 40, w: 120, h: 120 }, startDeg: -90, endDeg: gaugeEnd, style: { width: 8 }, transition },
          { type: "text", id: "readout", box: { x: c.width - 150, y: 170, w: 120, h: 26 }, text: readout },
        ],
        holdMs: hold,
      });
      const steps: ShapesStep[] = [frame(40, 2, -90, "0 %", 0, STAGE_EASE.inOut)];
      for (let i = 1; i <= 10; i++) steps.push(frame(c.width - 90, trackW, 270, `${i * 10} %`, 180, STAGE_EASE.inOut));
      steps[steps.length - 1].holdMs = 800;
      for (let loop = 0; loop < 2; loop++) {
        steps.push(frame(40, 2, -90, "0 %", 2000, STAGE_EASE.inOut), frame(c.width - 90, trackW, 270, "100 %", 2000, STAGE_EASE.inOut));
      }
      return steps;
    },
    expect: (c) => unsupported(c, [["ball", "circle"], ["gauge", "arc"]]),
    blankAfter: true,
  },
  {
    id: "stage-6-composed-glides",
    group: "stages",
    title: "6 · composed glides",
    description: "A second move lands while the first is still in flight; on animating glasses the disc and its label blend the two into one path.",
    steps: (c) => {
      const at = (x: number, y: number, hold: number): ShapesStep => ({
        elements: [
          ...stageLabel(c, 6, "composed glides"),
          { type: "circle", id: "ball", box: { x, y, w: 48, h: 48 }, style: { fill: true }, transition: { durationMs: 1000, easing: "ease-out" } },
          { type: "text", id: "ball-label", box: { x, y: y + 54, w: 240, h: 26 }, text: "second glide adds to the first", transition: { durationMs: 1000, easing: "ease-out" } },
        ],
        holdMs: hold,
      });
      const cx = Math.round(c.width / 2) - 24;
      const cy = Math.round(c.height / 2) - 40;
      return [at(cx, cy, 0), at(Math.round(c.width * 0.15), 30, 400), at(Math.round(c.width * 0.1), c.height - 100, 2500)];
    },
    expect: (c) => unsupported(c, [["ball", "circle"]]),
    blankAfter: true,
  },
];

SHAPE_CASES.push(...STAGE_CASES);

interface Verdict { pass: boolean; reason: string }

function evaluateResult(result: RenderResult, expectation: ShapesExpectation): Verdict {
  if (result.status !== expectation.status) {
    return { pass: false, reason: `status ${result.status}${result.reason ? ` (${result.reason})` : ""}, expected ${expectation.status}` };
  }
  const dropped = result.dropped ?? [];
  const wantDropped = expectation.dropped;
  if (dropped.length !== wantDropped.length || dropped.some((id, i) => id !== wantDropped[i])) {
    return { pass: false, reason: `dropped [${dropped.join(", ")}], expected [${wantDropped.join(", ")}]` };
  }
  const degraded = result.degraded === true;
  if (degraded !== expectation.degraded) {
    return { pass: false, reason: `degraded ${degraded}, expected ${expectation.degraded}` };
  }
  return { pass: true, reason: dropped.length ? `dropped as expected: ${dropped.join(", ")}` : "displayed" };
}

// ============================================================================
// Host pipeline — what the phone does to a scene before it reaches the radio
// (mobile/modules/engine/src/utils/display/scene/{anchor,process,differ}.ts)
// ============================================================================

/** What the phone reports for a G2 on the Glassly CFW with the vector modes (effectiveCapabilities.ts). */
const CANVAS: ShapesCanvas = { width: 576, height: 288, shapes: [...SHAPE_TYPES], animation: true, maxTextElements: 120 };
const MAX_IMAGE_ELEMENTS = 16;

type Change = "created" | "updated" | "moved" | "unchanged";
interface Diffable {
  id?: string;
  type: ElementType;
  box: Box;
  text?: string;
  style?: Style;
  points?: Point[];
  startDeg?: number;
  endDeg?: number;
  transition?: Transition;
  contentHash: string;
}
interface FrameElement extends Diffable { id: string; change: Change }

const POINT_ARITY: Record<string, number[]> = { line: [2], triangle: [3], quad: [4], bezier: [3, 4] };
const isNum = (n: unknown): n is number => typeof n === "number" && Number.isFinite(n);

function resolveAnchors(input: RenderElement[], width: number, height: number): RenderElement[] {
  return input.map((el) => {
    if (!el || typeof el !== "object" || !el.box || !el.box.anchor) return el;
    const { anchor, x, y, w, h } = el.box;
    const right = anchor === "top-right" || anchor === "bottom-right";
    const bottom = anchor === "bottom-left" || anchor === "bottom-right";
    return { ...el, box: { x: right ? width - x - w : x, y: bottom ? height - y - h : y, w, h } };
  });
}

function contentHash(...parts: (string | number | undefined)[]): string {
  let h = 0x811c9dc5;
  for (const part of parts) {
    const s = part === undefined ? " " : String(part);
    for (let i = 0; i < s.length; i++) {
      h ^= s.charCodeAt(i);
      h = Math.imul(h, 0x01000193);
    }
    h ^= 0x1f;
    h = Math.imul(h, 0x01000193);
  }
  return (h >>> 0).toString(36);
}

function elementContentHash(el: { type: ElementType; text?: string; data?: string; style?: Style; points?: Point[]; startDeg?: number; endDeg?: number }): string {
  const style = (el.style ?? {}) as Record<string, unknown>;
  const styleKey = Object.keys(style).sort().map((k) => `${k}=${style[k]}`).join(",");
  const pointsKey = el.points?.map((p) => `${p.x},${p.y}`).join(";");
  const angleKey = el.startDeg !== undefined || el.endDeg !== undefined ? `${el.startDeg}-${el.endDeg}` : undefined;
  return contentHash(el.type, el.text, el.data, styleKey, pointsKey, angleKey);
}

function clampBox(box: Box, width: number, height: number): Box | null {
  const x1 = Math.max(0, Math.floor(box.x));
  const y1 = Math.max(0, Math.floor(box.y));
  const x2 = Math.min(width, Math.floor(box.x) + Math.max(0, Math.floor(box.w)));
  const y2 = Math.min(height, Math.floor(box.y) + Math.max(0, Math.floor(box.h)));
  if (x2 <= x1 || y2 <= y1) return null;
  return { x: x1, y: y1, w: x2 - x1, h: y2 - y1 };
}

function validPoints(el: RenderElement): Point[] | null {
  const raw = el.points as unknown;
  if (!Array.isArray(raw) || !(POINT_ARITY[el.type] ?? []).includes(raw.length)) return null;
  const points: Point[] = [];
  for (const p of raw) {
    if (!p || typeof p !== "object" || !isNum(p.x) || !isNum(p.y)) return null;
    points.push({ x: Math.floor(p.x), y: Math.floor(p.y) });
  }
  return points;
}

function pointsBox(points: Point[]): Box {
  const xs = points.map((p) => p.x);
  const ys = points.map((p) => p.y);
  const x = Math.min(...xs);
  const y = Math.min(...ys);
  return { x, y, w: Math.max(...xs) - x + 1, h: Math.max(...ys) - y + 1 };
}

function validTransition(t: unknown): Transition | undefined {
  if (!t || typeof t !== "object") return undefined;
  const { durationMs, easing } = t as Transition;
  if (!isNum(durationMs) || durationMs <= 0) return undefined;
  return easing ? { durationMs, easing } : { durationMs };
}

function boxShrunk(orig: Box, clamped: Box): boolean {
  return clamped.x !== Math.floor(orig.x) || clamped.y !== Math.floor(orig.y) || clamped.w !== Math.floor(orig.w) || clamped.h !== Math.floor(orig.h);
}

interface Processed { elements: Diffable[]; degraded: boolean; dropped: string[] }

function processScene(input: RenderElement[], caps: ShapesCanvas): Processed {
  const dropped: string[] = [];
  let degraded = false;
  const reportId = (el: RenderElement | undefined, index: number) => el?.id ?? `${el?.type ?? "text"}[${index}]`;

  const seenIds = new Set<string>();
  const valid: { el: RenderElement; index: number; box: Box; points?: Point[] }[] = [];
  input.forEach((raw, index) => {
    if (!raw || typeof raw !== "object") {
      dropped.push(reportId(raw, index));
      degraded = true;
      return;
    }
    let points: Point[] | undefined;
    if (raw.type in POINT_ARITY) {
      const v = validPoints(raw);
      if (!v) {
        dropped.push(reportId(raw, index));
        degraded = true;
        return;
      }
      points = v;
      raw = { ...raw, box: pointsBox(v) };
    }
    if (!raw.box) {
      dropped.push(reportId(raw, index));
      degraded = true;
      return;
    }
    const b = raw.box;
    if (![b.x, b.y, b.w, b.h].every(isNum)) {
      dropped.push(reportId(raw, index));
      degraded = true;
      return;
    }
    if ((raw.type === "arc" || raw.type === "pie") && !(isNum(raw.startDeg) && isNum(raw.endDeg))) {
      dropped.push(reportId(raw, index));
      degraded = true;
      return;
    }
    const el = raw.id?.startsWith("~") ? { ...raw, id: raw.id.replace(/^~+/, "") || undefined } : raw;
    if (el.id) {
      const key = `${el.type}:${el.id}`;
      if (seenIds.has(key)) {
        dropped.push(reportId(el, index));
        degraded = true;
        return;
      }
      seenIds.add(key);
    }
    valid.push({ el, index, box: el.box!, points });
  });

  let textBudget = caps.maxTextElements;
  let imageBudget = MAX_IMAGE_ELEMENTS;
  const out: Diffable[] = [];
  for (const { el, index, box, points } of valid) {
    const transition = validTransition(el.transition);
    const clamped = clampBox(box, caps.width, caps.height);
    if (!clamped) {
      dropped.push(reportId(el, index));
      degraded = true;
      continue;
    }
    const shrunk = boxShrunk(box, clamped);
    if (shrunk) degraded = true;

    if (el.type !== "text" && el.type !== "image" && el.type !== "rect") {
      if (!caps.shapes.includes(el.type) || textBudget <= 0) {
        dropped.push(reportId(el, index));
        degraded = true;
        continue;
      }
      textBudget--;
      const angles = el.type === "arc" || el.type === "pie" ? { startDeg: el.startDeg, endDeg: el.endDeg } : {};
      out.push({
        id: el.id,
        type: el.type,
        box: clamped,
        style: el.style,
        ...(points ? { points } : {}),
        ...angles,
        ...(transition ? { transition } : {}),
        contentHash: elementContentHash({ type: el.type, style: el.style, points, ...angles }),
      });
      continue;
    }

    if (el.type === "image") {
      // The phone rasterizes these into cached tiles; this port has no rasterizer.
      if (shrunk || imageBudget <= 0) {
        dropped.push(reportId(el, index));
        degraded = true;
        continue;
      }
      imageBudget--;
      dropped.push(reportId(el, index));
      degraded = true;
      continue;
    }

    if (textBudget <= 0) {
      dropped.push(reportId(el, index));
      degraded = true;
      continue;
    }
    textBudget--;
    if (el.type === "rect") {
      out.push({ id: el.id, type: "rect", box: clamped, style: el.style, ...(transition ? { transition } : {}), contentHash: elementContentHash({ type: "rect", style: el.style }) });
      continue;
    }
    // Text: on the phone the default face is wrapped with the phone's metrics;
    // here every face passes through as given and the glasses clip it.
    const text = el.text ?? "";
    out.push({ id: el.id, type: "text", box: clamped, text, style: el.style, ...(transition ? { transition } : {}), contentHash: elementContentHash({ type: "text", text, style: el.style }) });
  }
  return { elements: out, degraded, dropped };
}

const boxesEqual = (a: Box, b: Box) => a.x === b.x && a.y === b.y && a.w === b.w && a.h === b.h;

function diffScene(prev: FrameElement[], next: Diffable[], nextSyntheticId: () => string): { elements: FrameElement[]; removed: string[] } {
  const prevUnmatched = new Set(prev.map((_, i) => i));
  const prevByExplicitId = new Map<string, number>();
  prev.forEach((el, i) => {
    if (!el.id.startsWith("~")) prevByExplicitId.set(`${el.type}:${el.id}`, i);
  });
  const matches: (number | undefined)[] = new Array(next.length).fill(undefined);
  next.forEach((el, i) => {
    if (!el.id) return;
    const j = prevByExplicitId.get(`${el.type}:${el.id}`);
    if (j !== undefined && prevUnmatched.has(j)) {
      matches[i] = j;
      prevUnmatched.delete(j);
    }
  });
  next.forEach((el, i) => {
    if (el.id || matches[i] !== undefined) return;
    for (const j of prevUnmatched) {
      const p = prev[j];
      if (p.id.startsWith("~") && p.type === el.type && boxesEqual(p.box, el.box)) {
        matches[i] = j;
        prevUnmatched.delete(j);
        break;
      }
    }
  });
  next.forEach((el, i) => {
    if (el.id || matches[i] !== undefined) return;
    for (const j of prevUnmatched) {
      const p = prev[j];
      if (p.id.startsWith("~") && p.type === el.type) {
        matches[i] = j;
        prevUnmatched.delete(j);
        break;
      }
    }
  });
  const elements = next.map((el, i): FrameElement => {
    const j = matches[i];
    if (j !== undefined) {
      const p = prev[j];
      const change: Change = !boxesEqual(p.box, el.box) ? "moved" : p.contentHash !== el.contentHash ? "updated" : "unchanged";
      return { ...el, id: el.id ?? p.id, change };
    }
    return { ...el, id: el.id ?? nextSyntheticId(), change: "created" };
  });
  return { elements, removed: [...prevUnmatched].map((j) => prev[j].id) };
}

// ============================================================================
// Mode-17 retained-scene encoder — G2CfwScene.swift
// ============================================================================

const T = {
  LINE: 1, RECT: 2, RECT_FILL: 3, CIRCLE: 4, CIRCLE_FILL: 5, TRI: 6, TRI_FILL: 7, QUAD: 8, QUAD_FILL: 9,
  BEZIER2: 10, BEZIER3: 11, ARC: 12, PIE: 13, IMAGE: 14, TEXT: 15, TEXT_CACHED: 16, TEXT_INLINE: 17,
} as const;
const MAX_SLOTS = 128;
const FRAME_PERIOD_MS = 33;
const TEXT_LINE_HEIGHT = 27;
const TEXT_INSET = 2;
const MAX_INLINE_BYTES = 128;

/** The 576×288 canvas sits in the 640×480 panel at the default height level (4) and the pinned middle depth (1), same on both lenses. */
const ORIGIN = { x: 32, y: 128 };

interface Slot { type: number; color: number; width: number; params: number[]; text: number[] }

const slot = (type: number, color: number, width: number, params: number[], text: number[] = []): Slot => {
  const padded = params.slice(0, 8);
  while (padded.length < 8) padded.push(0);
  return { type, color, width, params: padded, text };
};
const slotsEqual = (a: Slot, b: Slot) =>
  a.type === b.type && a.color === b.color && a.width === b.width && a.params.every((v, i) => v === b.params[i]) && a.text.length === b.text.length && a.text.every((v, i) => v === b.text[i]);

const clampCoord = (v: number) => Math.max(-1024, Math.min(1023, Math.trunc(v)));
const clampDim = (v: number) => Math.max(0, Math.min(2047, Math.trunc(v)));
const idiv = (a: number, b: number) => Math.trunc(a / b);
const i16 = (v: number) => { const u = v < 0 ? v + 0x10000 : v; return [u & 0xff, (u >> 8) & 0xff]; };

function tweenableMask(type: number): number {
  switch (type) {
    case T.LINE: return 0x0f;
    case T.RECT: case T.RECT_FILL: case T.ARC: case T.PIE: return 0x1f;
    case T.CIRCLE: case T.CIRCLE_FILL: return 0x07;
    case T.TRI: case T.TRI_FILL: case T.BEZIER2: return 0x3f;
    case T.QUAD: case T.QUAD_FILL: case T.BEZIER3: return 0xff;
    case T.IMAGE: case T.TEXT: case T.TEXT_CACHED: return 0x03;
    case T.TEXT_INLINE: return 0x0f;
    default: return 0;
  }
}

function easingCurve(easing: string | undefined): number[] {
  switch (easing) {
    case "linear": return [0, 0, 255, 255];
    case "ease-in": return [107, 0, 255, 255];
    case "ease-out": return [0, 0, 148, 255];
    case "ease-in-out": return [107, 0, 148, 255];
    default: return [64, 26, 64, 255]; // CSS "ease"
  }
}

function transitionFrames(ms: number): number {
  if (ms <= 0) return 0;
  return Math.max(2, Math.min(255, Math.round(ms / FRAME_PERIOD_MS)));
}

/** UTF-8 bytes of a line without firmware control bytes (1..31), cut at a codepoint boundary. */
function textBytes(line: string, limit = MAX_INLINE_BYTES): number[] {
  const out: number[] = [];
  const enc = new TextEncoder();
  for (const ch of line) {
    if (ch.codePointAt(0)! < 32) continue;
    const bytes = enc.encode(ch);
    if (out.length + bytes.length > limit) break;
    out.push(...bytes);
  }
  return out;
}

function slotsFor(el: FrameElement): Slot[] {
  const style = el.style ?? {};
  const gray = style.color === undefined || style.color < 0 ? 15 : Math.max(0, Math.min(15, Math.trunc(style.color)));
  // Open strokes carry their width as `width`; closed shapes and boxes as `border`.
  const border = Math.trunc(style.border ?? style.width ?? 0);
  const stroke = Math.max(1, Math.min(255, border));
  const x = clampCoord(el.box.x + ORIGIN.x);
  const y = clampCoord(el.box.y + ORIGIN.y);
  const w = clampDim(el.box.w);
  const h = clampDim(el.box.h);
  const radius = clampDim(style.radius ?? 0);
  const pt = (i: number): [number, number] => {
    const p = el.points?.[i];
    return p ? [clampCoord(p.x + ORIGIN.x), clampCoord(p.y + ORIGIN.y)] : [0, 0];
  };
  const circle = (): [number, number, number] => {
    const bw = Math.trunc(el.box.w), bh = Math.trunc(el.box.h);
    const r = Math.max(0, idiv(Math.min(bw, bh) - 1, 2));
    return [clampCoord(el.box.x + idiv(bw, 2) + ORIGIN.x), clampCoord(el.box.y + idiv(bh, 2) + ORIGIN.y), r];
  };
  switch (el.type) {
    case "rect": {
      if (style.fill) {
        const out = [slot(T.RECT_FILL, gray, 0, [x, y, w, h, radius])];
        if (border > 0) out.push(slot(T.RECT, gray, stroke, [x, y, w, h, radius]));
        return out;
      }
      return [slot(T.RECT, gray, stroke, [x, y, w, h, radius])];
    }
    case "circle": {
      const [cx, cy, r] = circle();
      return style.fill ? [slot(T.CIRCLE_FILL, gray, 0, [cx, cy, r])] : [slot(T.CIRCLE, gray, stroke, [cx, cy, r])];
    }
    case "arc":
    case "pie": {
      const [cx, cy, r] = circle();
      const a0 = clampCoord(el.startDeg ?? 0), a1 = clampCoord(el.endDeg ?? 0);
      return el.type === "pie" ? [slot(T.PIE, gray, 0, [cx, cy, r, a0, a1])] : [slot(T.ARC, gray, stroke, [cx, cy, r, a0, a1])];
    }
    case "line":
      return [slot(T.LINE, gray, stroke, [...pt(0), ...pt(1)])];
    case "triangle":
    case "quad": {
      const n = el.type === "triangle" ? 3 : 4;
      const params: number[] = [];
      for (let i = 0; i < n; i++) params.push(...pt(i));
      if (style.fill) return [slot(n === 3 ? T.TRI_FILL : T.QUAD_FILL, gray, 0, params)];
      return [slot(n === 3 ? T.TRI : T.QUAD, gray, stroke, params)];
    }
    case "bezier": {
      const n = el.points?.length ?? 0;
      if (n !== 3 && n !== 4) return [];
      const params: number[] = [];
      for (let i = 0; i < n; i++) params.push(...pt(i));
      return [slot(n === 3 ? T.BEZIER2 : T.BEZIER3, gray, stroke, params)];
    }
    case "text": {
      // One TEXT_INLINE slot per line, drawn by the glasses' font at a 2 px inset, clipped to the box.
      const out: Slot[] = [];
      if (border > 0) out.push(slot(T.RECT, gray, stroke, [x, y, w, h, radius]));
      const options = 0x10 | gray;
      const maxLines = Math.max(1, idiv(Math.trunc(el.box.h), TEXT_LINE_HEIGHT));
      const lines = (el.text ?? "").split("\n").slice(0, maxLines);
      lines.forEach((line, index) => {
        const bytes = textBytes(line);
        if (!bytes.length) return;
        const lineY = Math.trunc(y) + TEXT_INSET + index * TEXT_LINE_HEIGHT;
        const clipW = Math.max(0, Math.trunc(el.box.w) - TEXT_INSET);
        const clipH = Math.max(0, Math.trunc(el.box.h) - TEXT_INSET - index * TEXT_LINE_HEIGHT);
        out.push(slot(T.TEXT_INLINE, options, 0, [clampCoord(x + TEXT_INSET), clampCoord(lineY), Math.min(2047, clipW), Math.min(2047, clipH), bytes.length], bytes));
      });
      return out;
    }
    default:
      return [];
  }
}

/** `[0][slot][type][flags=visible][color][width][p0..p7 LE]`, or for TEXT_INLINE `[0][slot][17][1][color][width][x][y][w][h][len][bytes]`. */
function setRecord(s: Slot, index: number): number[] {
  const out = [0, index, s.type, 1, s.color, s.width];
  const count = s.type === T.TEXT_INLINE ? 4 : 8;
  for (const v of s.params.slice(0, count)) out.push(...i16(v));
  if (s.type === T.TEXT_INLINE) {
    out.push(Math.min(s.text.length, MAX_INLINE_BYTES));
    out.push(...s.text.slice(0, MAX_INLINE_BYTES));
  }
  return out;
}

/**
 * `[5][slot][mask:u16][frames][curve x4][value:i16 per set bit]`, or null when
 * the change is not a pure geometry/stroke change of the same shape type.
 *
 * The mask always covers every tweenable parameter (plus color and width where
 * the type allows), not just the ones that changed. The firmware takes an
 * unmasked parameter's end value from the slot's CURRENT value, which is
 * mid-flight when a previous tween is still running; a partial mask would then
 * freeze that parameter wherever the earlier tween had got to. The phone's
 * G2CfwScene.swift sends changed parameters only, so its composed glides land
 * off-target on this firmware.
 */
function tweenRecord(old: Slot, next: Slot, index: number, frames: number, curve: number[]): number[] | null {
  if (old.type !== next.type || old.text.length !== next.text.length || old.text.some((v, i) => v !== next.text[i])) return null;
  const tweenable = tweenableMask(next.type);
  for (let k = 0; k < 8; k++) {
    if (old.params[k] !== next.params[k] && !(tweenable & (1 << k))) return null;
  }
  const isTexture = next.type >= T.IMAGE;
  const inlineFade = next.type === T.TEXT_INLINE && (old.color & 0xf0) === (next.color & 0xf0);
  if (old.color !== next.color && isTexture && !inlineFade) return null;
  if (old.width !== next.width && isTexture) return null;
  let mask = tweenable;
  const values: number[] = [];
  for (let k = 0; k < 8; k++) if (tweenable & (1 << k)) values.push(next.params[k]);
  if (!isTexture || inlineFade) { mask |= 0x100; values.push(next.color); }
  if (!isTexture) { mask |= 0x200; values.push(next.width); }
  if (mask === 0) return null;
  return [5, index, mask & 0xff, mask >> 8, frames, ...curve.slice(0, 4), ...values.flatMap(i16)];
}

interface ElementState { id: string; slots: Slot[]; indices: number[] }

class CfwScene {
  private elements: ElementState[] = [];
  private needsRepack = true;

  invalidate() { this.needsRepack = true; }
  indices(): number[][] { return this.elements.map((e) => e.indices); }

  /** One mode-17 patch (COMMIT, plus CLEAR on a repack), or null when the frame does not fit the slot table. */
  encode(frame: FrameElement[], replay = false): { payload: Uint8Array; sets: number; deletes: number; tweens: number; repack: boolean; animMs: number } | null {
    const desired = frame.map((el) => ({
      id: el.id,
      slots: slotsFor(el),
      frames: transitionFrames(el.transition?.durationMs ?? 0),
      curve: easingCurve(el.transition?.easing),
    }));
    const total = desired.reduce((n, d) => n + d.slots.length, 0);
    if (total > MAX_SLOTS) return null;

    const previous = new Map(this.elements.map((s) => [s.id, s]));
    let repack = this.needsRepack || replay;
    let assigned: number[][] = [];
    if (!repack) {
      const used = new Set<number>();
      for (const item of desired) {
        const prev = previous.get(item.id);
        if (prev && prev.slots.length === item.slots.length) prev.indices.forEach((i) => used.add(i));
      }
      let next = (used.size ? Math.max(...used) : -1) + 1;
      let lastIndex = -1;
      for (const item of desired) {
        const prev = previous.get(item.id);
        let indices: number[];
        if (prev && prev.slots.length === item.slots.length) {
          indices = prev.indices;
        } else {
          indices = item.slots.map((_, k) => next + k);
          next += item.slots.length;
        }
        if (indices.length && indices[0] <= lastIndex) { repack = true; break; }
        lastIndex = indices.length ? indices[indices.length - 1] : lastIndex;
        assigned.push(indices);
      }
      if (next > MAX_SLOTS) repack = true;
    }
    if (repack) {
      assigned = [];
      let next = 0;
      for (const item of desired) {
        assigned.push(item.slots.map((_, k) => next + k));
        next += item.slots.length;
      }
    }

    const ops: number[] = [];
    let sets = 0;
    let deletes = 0;
    let tweens = 0;
    let animFrames = 0;
    if (!repack) {
      const kept = new Set(desired.map((d) => d.id));
      for (const state of this.elements) {
        if (kept.has(state.id)) continue;
        for (const index of state.indices) { ops.push(1, index); deletes++; }
      }
    }
    const committed: ElementState[] = [];
    desired.forEach((item, di) => {
      const indices = assigned[di];
      const prev = repack ? undefined : previous.get(item.id);
      item.slots.forEach((s, k) => {
        const index = indices[k];
        if (prev && prev.slots.length === item.slots.length) {
          const old = prev.slots[k];
          if (slotsEqual(old, s)) return;
          if (item.frames >= 2) {
            const tween = tweenRecord(old, s, index, item.frames, item.curve);
            if (tween) { ops.push(...tween); tweens++; animFrames = Math.max(animFrames, item.frames); return; }
          }
        }
        ops.push(...setRecord(s, index));
        sets++;
      });
      committed.push({ id: item.id, slots: item.slots, indices });
    });

    this.elements = committed;
    this.needsRepack = false;
    return { payload: Uint8Array.from([17, repack ? 0x03 : 0x01, 0, ...ops]), sets, deletes, tweens, repack, animMs: animFrames * FRAME_PERIOD_MS };
  }
}

// ============================================================================
// Runner
// ============================================================================

interface CaseResult {
  id: string;
  group: string;
  title: string;
  state: "pass" | "fail" | "error";
  reason: string;
  renderStatus?: "displayed" | "blocked";
  degraded?: boolean;
  dropped?: string[];
  presented?: boolean;
  /** Slowest BLE ack of a frame in the case. */
  ackMs?: number;
  /** Longest transition the case started on the glasses. */
  settleMs?: number;
  steps?: number;
  /** Renders per second over the whole case, holds included. */
  stepsPerSecond?: number;
  /** Total mode-17 bytes the case put on the air. */
  bytes?: number;
}

const args = process.argv.slice(2);
const DUMP = args.includes("--dump") ? args[args.indexOf("--dump") + 1] : undefined;
const DRY_RUN = process.env.G2_DRY_RUN === "1" || DUMP !== undefined;
const TRACE = process.env.G2_TRACE === "1";
const HOLD_SCALE = DRY_RUN ? 0 : Math.max(0, Number(process.env.G2_HOLD_SCALE ?? "1"));
const ACK_MS = 8_000;
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));
const sleepUntil = (t: number) => { const ms = t - performance.now(); return ms > 0 ? sleep(ms) : Promise.resolve(); };
const scaled = (ms: number) => Math.round(ms * HOLD_SCALE);

// ── self-test: the encoder vectors from G2CfwSceneTests.swift, byte for byte ──
// One deliberate difference: TWEEN masks here cover every tweenable parameter
// (see tweenRecord), where the Swift encoder masks only the changed ones.
async function selfTest(): Promise<void> {
  const failures: string[] = [];
  let checks = 0;
  const eq = (name: string, got: ArrayLike<number> | number[][], want: ArrayLike<number> | number[][]) => {
    checks++;
    const g = JSON.stringify(Array.from(got as ArrayLike<number>)), w = JSON.stringify(Array.from(want as ArrayLike<number>));
    if (g !== w) failures.push(`${name}: got ${g}, want ${w}`);
  };
  const le = (...values: number[]) => values.flatMap(i16);
  ORIGIN.y = 96; // the Swift tests place the canvas at (32, 96)
  const fresh = () => new Renderer(null);
  const sub = (b: Uint8Array, from: number, to: number) => b.subarray(from, to);

  // shapes become SET records with offset geometry
  let r = fresh();
  await r.render([
    { type: "line", id: "l", points: [{ x: 10, y: 20 }, { x: 30, y: 40 }], style: { width: 3, color: 12 } },
    { type: "circle", id: "c", box: { x: 100, y: 100, w: 41, h: 41 }, style: { fill: true } },
    { type: "arc", id: "a", box: { x: 0, y: 0, w: 101, h: 101 }, startDeg: -90, endDeg: 180, style: { width: 4 } },
    { type: "rect", id: "r", box: { x: 1, y: 2, w: 30, h: 20 }, style: { border: 2, radius: 6, fill: true } },
  ]);
  let b = r.lastPayload;
  eq("shapes header", sub(b, 0, 3), [17, 0x03, 0]);
  eq("line record", sub(b, 3, 9), [0, 0, 1, 1, 12, 3]);
  eq("line points", sub(b, 9, 17), le(42, 116, 62, 136));
  eq("circle record", sub(b, 25, 31), [0, 1, 5, 1, 15, 0]);
  eq("circle geometry", sub(b, 31, 37), le(152, 216, 20));
  eq("arc record", sub(b, 47, 53), [0, 2, 12, 1, 15, 4]);
  eq("arc angles", sub(b, 59, 63), le(-90, 180));
  eq("rect fill", sub(b, 69, 75), [0, 3, 3, 1, 15, 0]);
  eq("rect stroke", sub(b, 91, 97), [0, 4, 2, 1, 15, 2]);
  eq("shapes length", [b.length], [113]);
  eq("shapes indices", r.slotIndices(), [[0], [1], [2], [3, 4]]);

  // a moved element with a transition becomes a TWEEN
  r = fresh();
  await r.render([{ type: "circle", id: "ball", box: { x: 0, y: 0, w: 21, h: 21 }, style: { fill: true } }]);
  await r.render([{ type: "circle", id: "ball", box: { x: 200, y: 0, w: 21, h: 21 }, style: { fill: true }, transition: { durationMs: 330, easing: "linear" } }]);
  b = r.lastPayload;
  eq("tween header", sub(b, 0, 3), [17, 0x01, 0]);
  eq("tween record", sub(b, 3, 12), [5, 0, 0x07, 0x03, 10, 0, 0, 255, 255]);
  eq("tween target", sub(b, 12, 22), le(242, 106, 10, 15, 0)); // cx cy r color width: the unchanged ones ride along
  eq("tween length", [b.length], [22]);

  // a geometry change without a transition, and a type change, re-SET
  r = fresh();
  await r.render([{ type: "circle", id: "s", box: { x: 0, y: 0, w: 11, h: 11 } }]);
  await r.render([{ type: "circle", id: "s", box: { x: 5, y: 0, w: 11, h: 11 } }]);
  eq("moved without transition", sub(r.lastPayload, 3, 5), [0, 0]);
  await r.render([{ type: "circle", id: "s", box: { x: 5, y: 0, w: 11, h: 11 }, style: { fill: true }, transition: { durationMs: 500 } }]);
  eq("retyped", sub(r.lastPayload, 3, 6), [0, 0, 5]);

  // removal DELETEs slots; a reorder repacks
  r = fresh();
  await r.render([{ type: "circle", id: "a", box: { x: 0, y: 0, w: 11, h: 11 } }, { type: "circle", id: "b", box: { x: 0, y: 0, w: 11, h: 11 } }]);
  await r.render([{ type: "circle", id: "b", box: { x: 0, y: 0, w: 11, h: 11 } }]);
  eq("delete", r.lastPayload, [17, 0x01, 0, 1, 0]);
  eq("delete indices", r.slotIndices(), [[1]]);
  await r.render([{ type: "circle", id: "a", box: { x: 0, y: 0, w: 11, h: 11 } }, { type: "circle", id: "b", box: { x: 0, y: 0, w: 11, h: 11 } }]);
  eq("reorder repacks", [r.lastPayload[1]], [0x03]);
  eq("reorder indices", r.slotIndices(), [[0], [1]]);

  // inline text records carry their bytes and animate
  r = fresh();
  const t = (y: number, text: string, color: number, transition?: Transition): RenderElement => ({ type: "text", id: "t", box: { x: 10, y, w: 200, h: 60 }, text, style: { color }, ...(transition ? { transition } : {}) });
  await r.render([t(10, "hi\nthere", 9)]);
  b = r.lastPayload;
  eq("text indices", r.slotIndices(), [[0, 1]]);
  eq("text record", sub(b, 3, 9), [0, 0, 17, 1, 0x19, 0]);
  eq("text box", sub(b, 9, 17), le(44, 108, 198, 58));
  eq("text bytes", sub(b, 17, 20), [2, 0x68, 0x69]);
  eq("text line 2", sub(b, 20, 23), [0, 1, 17]);
  eq("text line 2 box", sub(b, 26, 34), le(44, 135, 198, 31));
  eq("text line 2 bytes", sub(b, 35, 40), Array.from(new TextEncoder().encode("there")));
  await r.render([t(40, "hi\nthere", 9, { durationMs: 330 })]);
  eq("text move tweens", sub(r.lastPayload, 3, 8), [5, 0, 0x0f, 0x01, 10]);
  eq("text move targets", sub(r.lastPayload, 12, 22), le(44, 138, 198, 58, 0x19));
  await r.render([t(40, "ho\nthere", 9, { durationMs: 330 })]);
  eq("text change re-SETs", sub(r.lastPayload, 3, 6), [0, 0, 17]);
  await r.render([t(40, "ho\nthere", 3, { durationMs: 330 })]);
  eq("text fade tweens", sub(r.lastPayload, 3, 8), [5, 0, 0x0f, 0x01, 10]);
  eq("text fade target", sub(r.lastPayload, 20, 22), le(0x13));

  eq("easing", easingCurve("ease-in-out"), [107, 0, 148, 255]);
  eq("text bytes strip controls", textBytes("a\u0001b\u00e9", 3), [0x61, 0x62]);

  ORIGIN.y = 128;
  for (const f of failures) console.log(`  FAIL ${f}`);
  console.log(`self-test: ${checks - failures.length}/${checks} checks ok`);
  process.exit(failures.length ? 1 : 0);
}

if (args.includes("--list")) {
  for (const c of SHAPE_CASES) console.log(`${c.id.padEnd(28)} ${c.group.padEnd(10)} ${c.title}`);
  process.exit(0);
}
const selectors = args.filter((a, i) => !a.startsWith("--") && args[i - 1] !== "--dump");
const selected = selectors.length ? SHAPE_CASES.filter((c) => selectors.includes(c.id) || selectors.includes(c.group)) : SHAPE_CASES;
if (!selected.length) {
  console.error(`no cases match ${selectors.join(", ")}; try --list`);
  process.exit(2);
}

let magic = 100;
const nextMagic = () => (magic = magic >= 255 ? 100 : magic + 1);
const varint = (v: number) => { const out: number[] = []; do { let b = v & 0x7f; v >>>= 7; if (v) b |= 0x80; out.push(b); } while (v); return out; };

// Framebuffer lease over sid 0x09 field 101 (['F','C',1,op,nonceLo,nonceHi]).
function leasePb(op: number): { pb: Uint8Array; magic: number } {
  const m = nextMagic();
  const ctl = [0x46, 0x43, 1, op, 1, 0];
  return { pb: Uint8Array.from([0x08, 2, 0x10, ...varint(m), 0xaa, 0x06, ctl.length, ...ctl]), magic: m };
}

interface Link { send(payload: Uint8Array): Promise<void>; close(): Promise<void>; firmware: string; cfw: string }

/**
 * A hidden 2-frame tween before the first case: creates the firmware's
 * animation timer and runs one tick while nothing is on screen, so the first
 * visible transition is not also the first animation of the session.
 */
const WARM_UP = Uint8Array.from([
  17, 0x03, 0,
  0, 0, T.CIRCLE_FILL, 0, 0, 0, ...i16(0), ...i16(0), ...i16(1), ...i16(0), ...i16(0), ...i16(0), ...i16(0), ...i16(0),
  5, 0, 0x01, 0x00, 2, 0, 0, 255, 255, ...i16(1),
]);

/** `--dump`: the payload stream for patches/host/scene_replay_host.c — [1][len:u16][bytes], [2][ms:u32] wait, [3][len:u8][label]. */
const dump: number[] = [];
const dumpMessage = (payload: Uint8Array) => { if (DUMP) dump.push(1, payload.length & 0xff, payload.length >> 8, ...payload); };
const dumpWait = (ms: number) => { if (DUMP && ms > 0) dump.push(2, ms & 0xff, (ms >> 8) & 0xff, (ms >> 16) & 0xff, (ms >> 24) & 0xff); };
const dumpLabel = (label: string) => { if (DUMP) { const b = new TextEncoder().encode(label).slice(0, 255); dump.push(3, b.length, ...b); } };

async function openLink(): Promise<Link> {
  const session = await G2Session.open();
  const settings = await querySettings(session, nextMagic());
  const firmware = settings ? `L=${settings.leftSoftwareVersion} R=${settings.rightSoftwareVersion}` : "unknown";
  console.log(`firmware: ${firmware}`);
  let caps = await queryCapabilities(session, nextMagic());
  if (!caps) caps = await queryCapabilities(session, nextMagic());
  const needed = ["scene17", "shapes16", "anim18"];
  if (!caps || !needed.every((f) => hasFeature(caps!, f))) {
    console.log(caps ? `CFW ${caps.raw}` : "no CFW capability field");
    console.log(`this suite needs the glassly-cfw build (${needed.join(" ")})`);
    await session.close();
    process.exit(1);
  }
  console.log(`CFW detected: ${caps.raw}`);

  const hb = startHeartbeat({ session, nextMagic });
  const suffix = String(Date.now() % 10_000).padStart(4, "0");
  let sid = 1;
  const create = buildCreateStartUpPageContainer({ name: `b${suffix}`, items: ["."], containerId: 1, captureEvents: false, magic: nextMagic(), extraContainerNames: [`c${suffix}`] });
  if (!(await session.sendPb(0xe0, create.pb, create.magic, { ackTimeoutMs: ACK_MS }))) throw new Error("CREATE did not ack");
  const container: ImageContainerSpec = { name: `c${suffix}`, containerId: 2, x: 0, y: 0, width: 576, height: 288 };
  const rebuild = buildImageContainers({ containers: [container], magic: nextMagic() });
  if (!(await session.sendPb(0xe0, rebuild.pb, rebuild.magic, { ackTimeoutMs: ACK_MS }))) throw new Error("REBUILD did not ack");
  await sleep(300);

  const lease = async (op: number) => {
    const { pb, magic: m } = leasePb(op);
    await session.sendPb(0x09, pb, m, { ackTimeoutMs: ACK_MS });
  };
  await lease(5); // FB_ACQUIRE (90 s, fail-open)
  const renew = setInterval(() => void lease(5), 30_000);

  const send = async (payload: Uint8Array) => {
    for (const frag of planImageFragments(payload, 4000)) {
      const raw = buildImageRawData({
        containerId: container.containerId, containerName: container.name, mapSessionId: sid,
        mapTotalSize: payload.length, mapFragmentIndex: frag.index, mapRawData: frag.data,
        magic: nextMagic(), compressMode: 0,
      });
      if (!(await session.sendPb(0xe0, raw.pb, raw.magic, { ackTimeoutMs: ACK_MS }))) throw new Error(`image message (mode ${payload[0]}) did not ack`);
    }
    sid++;
  };
  await send(WARM_UP);
  await sleep(150);
  return {
    firmware,
    cfw: caps.raw,
    send,
    async close() {
      clearInterval(renew);
      try { await send(Uint8Array.from([18, 2])); } catch {} // release the retained scene
      await lease(6).catch(() => {}); // FB_RELEASE
      hb.stop();
      await session.close();
    },
  };
}

/** The render() half of the host: one call = one frame on the glasses, resolved once acked and its transitions have played. */
class Renderer {
  private prev: FrameElement[] = [];
  private synthetic = 0;
  private scene = new CfwScene();
  lastAckMs = 0;
  lastBytes = 0;
  lastPayload: Uint8Array = new Uint8Array();
  /** Longest tween the last frame started on the glasses (frames × 33 ms). */
  lastSettleMs = 0;

  constructor(private readonly link: Link | null) {}

  slotIndices(): number[][] { return this.scene.indices(); }

  async render(elements: RenderElement[]): Promise<RenderResult> {
    const resolved = resolveAnchors(elements, CANVAS.width, CANVAS.height);
    const processed = processScene(resolved, CANVAS);
    const { elements: diffed } = diffScene(this.prev, processed.elements, () => `~${++this.synthetic}`);
    this.prev = diffed;
    const encoded = this.scene.encode(diffed);
    const base = { status: "displayed" as const, degraded: processed.degraded, dropped: processed.dropped };
    if (!encoded) {
      // The phone falls back to its raster path here; this port has none.
      this.scene.invalidate();
      this.prev = [];
      return { ...base, presented: false, reason: "frame does not fit the 128-slot table" };
    }
    this.lastBytes = encoded.payload.length;
    this.lastPayload = encoded.payload;
    this.lastSettleMs = encoded.animMs;
    dumpMessage(encoded.payload);
    const t0 = performance.now();
    if (this.link) {
      try {
        await this.link.send(encoded.payload);
      } catch (err) {
        this.scene.invalidate();
        this.prev = [];
        throw err;
      }
    }
    this.lastAckMs = this.link ? performance.now() - t0 : 0;
    if (TRACE) {
      const ops = [encoded.repack ? "repack" : "", encoded.sets ? `${encoded.sets} set` : "", encoded.tweens ? `${encoded.tweens} tween` : "", encoded.deletes ? `${encoded.deletes} delete` : ""].filter(Boolean).join(", ");
      console.log(`      ${String(encoded.payload.length).padStart(5)} B  ${String(Math.round(this.lastAckMs)).padStart(4)} ms ack  settle ${this.lastSettleMs} ms  ${ops || "no change"}${processed.dropped.length ? `  dropped ${processed.dropped.join(",")}` : ""}`);
    }
    return { ...base, presented: true };
  }
}

async function runCase(shapesCase: ShapesCase, renderer: Renderer): Promise<CaseResult> {
  const head = { id: shapesCase.id, group: shapesCase.group, title: shapesCase.title };
  const steps = shapesCase.steps(CANVAS);
  const expectation = shapesCase.expect(CANVAS);
  let ackMs = 0;
  let settleMs = 0;
  let bytes = 0;
  const started = performance.now();
  dumpLabel(shapesCase.id);
  try {
    // The glasses keep animating after a frame is acked; a later frame never
    // waits for that (it is the point of the compose cases), but the case's
    // last frame does before its hold, so nothing is blanked mid-transition.
    let animEndsAt = 0;
    for (let i = 0; i < steps.length; i++) {
      const sentAt = performance.now();
      const result = await renderer.render(steps[i].elements);
      const ackedAt = performance.now();
      ackMs = Math.max(ackMs, renderer.lastAckMs);
      settleMs = Math.max(settleMs, renderer.lastSettleMs);
      bytes += renderer.lastBytes;
      animEndsAt = Math.max(animEndsAt, ackedAt + renderer.lastSettleMs);
      if (i < steps.length - 1) {
        dumpWait(steps[i].holdMs);
        await sleepUntil(sentAt + scaled(steps[i].holdMs));
        continue;
      }
      const verdict = result.presented === false ? { pass: false, reason: `not shown on the glasses: ${result.reason ?? "unknown"}` } : evaluateResult(result, expectation);
      const elapsed = Math.max(1, performance.now() - started);
      dumpWait(Math.max(steps[i].holdMs, renderer.lastSettleMs));
      await sleepUntil(Math.max(sentAt + scaled(steps[i].holdMs), HOLD_SCALE > 0 ? animEndsAt : 0));
      if (shapesCase.blankAfter) {
        await renderer.render([]);
        dumpWait(700);
        await sleep(scaled(700));
      }
      return {
        ...head,
        state: verdict.pass ? "pass" : "fail",
        reason: verdict.reason,
        renderStatus: result.status,
        degraded: result.degraded === true,
        dropped: result.dropped ?? [],
        presented: result.presented,
        ackMs: Math.round(ackMs),
        settleMs,
        steps: steps.length,
        stepsPerSecond: Math.round((steps.length / elapsed) * 1000 * 10) / 10,
        bytes,
      };
    }
    return { ...head, state: "error", reason: "case has no steps" };
  } catch (err) {
    return { ...head, state: "error", reason: err instanceof Error ? err.message : String(err) };
  }
}

if (args.includes("--self-test")) await selfTest();
const link = DRY_RUN ? null : await openLink();
const renderer = new Renderer(link);
const results: CaseResult[] = [];
if (DUMP) { dumpLabel("warm-up"); dumpMessage(WARM_UP); dumpWait(150); }
console.log(`${DRY_RUN ? "dry run: " : ""}${selected.length} case${selected.length === 1 ? "" : "s"}, canvas ${CANVAS.width}×${CANVAS.height}, budget ${CANVAS.maxTextElements}`);
try {
  for (let i = 0; i < selected.length; i++) {
    const c = selected[i];
    const r = await runCase(c, renderer);
    results.push(r);
    const timing = r.state === "error" ? "" : `  ${String(r.ackMs).padStart(4)} ms ack, ${String(r.settleMs).padStart(4)} ms anim, ${r.steps} step${r.steps === 1 ? "" : "s"} @ ${r.stepsPerSecond}/s, ${r.bytes} B`;
    console.log(`[${String(i + 1).padStart(2)}/${selected.length}] ${r.state.toUpperCase().padEnd(5)} ${r.id.padEnd(26)} ${r.reason}${timing}`);
  }
} finally {
  if (link) {
    try { await renderer.render([]); } catch {}
    await link.close();
  } else if (DUMP) {
    dumpLabel("teardown");
    await renderer.render([]);
    dumpMessage(Uint8Array.from([18, 2]));
  }
}

if (DUMP) {
  await Bun.write(DUMP, Uint8Array.from(dump));
  console.log(`wrote ${dump.length} bytes to ${DUMP}`);
}
const passed = results.filter((r) => r.state === "pass").length;
const failed = results.filter((r) => r.state === "fail");
const errored = results.filter((r) => r.state === "error");
console.log(`\n${passed} passed, ${failed.length} failed, ${errored.length} errored`);
for (const r of [...failed, ...errored]) console.log(`  ${r.state.toUpperCase()} ${r.id}: ${r.reason}`);

if (process.env.G2_OUT) {
  const out = {
    ranAt: new Date().toISOString(),
    dryRun: DRY_RUN,
    firmware: link?.firmware ?? null,
    cfw: link?.cfw ?? null,
    canvas: CANVAS,
    holdScale: HOLD_SCALE,
    results,
  };
  await Bun.write(process.env.G2_OUT, JSON.stringify(out, null, 2) + "\n");
  console.log(`wrote ${process.env.G2_OUT}`);
}
process.exit(failed.length || errored.length ? 1 : 0);
