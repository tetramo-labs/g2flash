import {expect, test} from "bun:test";
import {potracePath, svgVideoSteps, type SvgVideo} from "./svg-video";
import {svgPath} from "./vector-protocol";

test("Potrace relative curves and holes are transformed into panel coordinates", () => {
  const xml = '<g transform="translate(0.000000,20.000000) scale(0.500000,-0.500000)"><path d="M0 0 c0 20 20 20 20 0z M4 2 l4 0 0 4 -4 0z"/></g>';
  const result = potracePath(xml);
  expect(result.commands).toEqual(svgPath("M0 20C0 10 10 10 10 20Z M2 19L4 19L4 17L2 17Z"));
  expect(svgPath(result.d)).toEqual(result.commands);
  expect(() => potracePath('<g transform="rotate(90)"><path d="M0 0Z"/></g>')).toThrow();
});

test("video tile replacement clears old tiles and keeps frame timing", () => {
  const video: SvgVideo = {version: 1, fps: 5, width: 576, height: 432, source: "fixture", frames: [
    {paths: [{d: "M0 0L10 0L10 10Z", x: 0, y: 0, edges: 3}, {d: "M0 0L10 0L10 10Z", x: 288, y: 0, edges: 3}]},
    {paths: []},
  ]};
  const steps = svgVideoSteps(video);
  expect([...steps[0].payload.slice(0, 3)]).toEqual([17, 3, 0]);
  expect([...steps[0].payload.slice(8, 12)]).toEqual([32, 0, 24, 0]);
  expect([...steps[1].payload]).toEqual([17, 3, 0]);
  expect(steps[0].waitMs).toBe(200);
  expect([...steps[2].payload]).toEqual([18, 2]);
  expect(svgVideoSteps(video, 1).length).toBe(2);
  expect(() => svgVideoSteps({...video, fps: 0})).toThrow();
  expect(() => svgVideoSteps({...video, frames: [{paths: [{d: "M0 0Z", x: 0, y: 0, edges: 513}]}]})).toThrow();
});
